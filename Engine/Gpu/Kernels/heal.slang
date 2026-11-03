/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/common/heal.c,
 * Copyright (C) 2009-2026 darktable developers, distributed under the GNU GPL version 3 or later,
 * with the arithmetic of the CPU port in Engine/Nodes/Filter/EdgeAware/PoissonHeal.cpp. darktable
 * has no OpenCL heal kernel.
 *
 * Images are 4 interleaved channels, tightly packed, top-down. The red/black over-relaxation runs
 * on the full-size difference image: a sweep updates the pixels of one colour, (x + y) even being
 * black, from those of the other. Each iteration is a black sweep, a red sweep and heal_reduce,
 * which stops later sweeps once the update is small enough; only that sum's order differs from
 * the CPU's.
 */

RWStructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> destBuf;
// One float a pixel: masked where finite and not 0.
RWStructuredBuffer<float> maskBuf;
// dest - src, the samples where either is not finite as 0.
RWStructuredBuffer<float> diffBuf;
// The masked pixel count, the done flag, then the bits of the relaxation factor and of the error
// threshold; the host zeroes the first two.
RWStructuredBuffer<uint> stateBuf;
// Each sweep group's error sum, the black sweep's groups first.
RWStructuredBuffer<float> partialBuf;

// GpuHealParams in Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.cpp mirrors it.
struct HealParams
{
    uint width;
    uint height;
    // 0 black, 1 red.
    uint colour;
    // The sweep's groups along x, and in all.
    uint groupsX;
    uint groups;
    float epsilon;
};
[[vk::push_constant]] ConstantBuffer<HealParams> params;

static const uint kCount = 0;
static const uint kDone = 1;
static const uint kRelaxation = 2;
static const uint kErrExit = 3;
static const uint kSweepGroup = 256;

groupshared float sums[kSweepGroup];

bool isFiniteBits(float v)
{
    return (asuint(v) & 0x7f800000u) != 0x7f800000u;
}

bool masked(uint k)
{
    const float m = maskBuf[k];
    return isFiniteBits(m) && m != 0.0f;
}

// Sums sums[] into sums[0]; every thread of the group must call it.
void reduceGroup(uint index)
{
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kSweepGroup / 2; stride > 0; stride >>= 1)
    {
        if (index < stride)
        {
            precise float s = sums[index] + sums[index + stride];
            sums[index] = s;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

// The difference image and the masked pixel count.
[shader("compute")]
[numthreads(16, 16, 1)]
void heal_setup(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint k = tid.y * params.width + tid.x;
    if (masked(k))
        InterlockedAdd(stateBuf[kCount], 1u);
    for (uint c = 0; c < 4; ++c)
    {
        const float d = destBuf[4 * k + c];
        const float s = srcBuf[4 * k + c];
        precise float diff = d - s;
        diffBuf[4 * k + c] = (isFiniteBits(d) && isFiniteBits(s)) ? diff : 0.0f;
    }
}

// darktable's empirical relaxation factor for the mask's size and the error threshold.
[shader("compute")]
[numthreads(1, 1, 1)]
void heal_params(uint3 tid : SV_DispatchThreadID)
{
    precise float w = (2.0f - 1.0f / (0.1575f * sqrt(float(stateBuf[kCount])) + 0.8f)) * 0.25f;
    precise float errExit = params.epsilon * params.epsilon * w * w;
    stateBuf[kRelaxation] = asuint(w);
    stateBuf[kErrExit] = asuint(errExit);
}

float neighbour(bool present, uint k, uint c)
{
    return present ? diffBuf[4 * k + c] : 0.0f;
}

// One sweep over the masked pixels of params.colour, one thread per pixel of that colour.
[shader("compute")]
[numthreads(16, 16, 1)]
void heal_sweep(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint index : SV_GroupIndex)
{
    if (stateBuf[kDone] != 0)
        return;
    const uint w = params.width;
    const uint h = params.height;
    const uint y = tid.y;
    const uint x = 2 * tid.x + ((y & 1) ^ params.colour);
    float err = 0.0f;
    if (x < w && y < h)
    {
        const uint k = y * w + x;
        if (masked(k))
        {
            const float relax = asfloat(stateBuf[kRelaxation]);
            float a = 4.0f;
            if (y == 0)
                a -= 1.0f;
            if (y == h - 1)
                a -= 1.0f;
            if (x == 0)
                a -= 1.0f;
            // darktable reads a zero right neighbour at the last column of an odd width.
            if (x == w - 1 && (w & 1) == 0)
                a -= 1.0f;
            float diffs[3];
            for (uint c = 0; c < 4; ++c)
            {
                const float up = neighbour(y > 0, k - w, c);
                const float down = neighbour(y + 1 < h, k + w, c);
                const float left = neighbour(x > 0, k - 1, c);
                const float right = neighbour(x + 1 < w, k + 1, c);
                const float v = diffBuf[4 * k + c];
                precise float diff = relax * ((a * v) - (up + down + left + right));
                precise float updated = v - diff;
                diffBuf[4 * k + c] = updated;
                if (c < 3)
                    diffs[c] = diff;
            }
            precise float e = diffs[0] * diffs[0] + diffs[1] * diffs[1] + diffs[2] * diffs[2];
            err = e;
        }
    }
    sums[index] = err;
    reduceGroup(index);
    if (index == 0)
        partialBuf[params.colour * params.groups + gid.y * params.groupsX + gid.x] = sums[0];
}

// The iteration's error from both sweeps' group sums; sets the done flag once it is below the
// threshold.
[shader("compute")]
[numthreads(256, 1, 1)]
void heal_reduce(uint3 tid : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (stateBuf[kDone] != 0)
        return;
    float acc = 0.0f;
    for (uint i = index; i < 2 * params.groups; i += kSweepGroup)
    {
        precise float s = acc + partialBuf[i];
        acc = s;
    }
    sums[index] = acc;
    reduceGroup(index);
    if (index == 0 && sums[0] < asfloat(stateBuf[kErrExit]))
        stateBuf[kDone] = 1;
}

// The healed image: src plus the relaxed difference where both dest and src are finite, dest
// elsewhere.
[shader("compute")]
[numthreads(16, 16, 1)]
void heal_finish(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint k = tid.y * params.width + tid.x;
    for (uint c = 0; c < 4; ++c)
    {
        const float d = destBuf[4 * k + c];
        const float s = srcBuf[4 * k + c];
        if (isFiniteBits(d) && isFiniteBits(s))
        {
            precise float v = diffBuf[4 * k + c] + s;
            destBuf[4 * k + c] = v;
        }
    }
}
