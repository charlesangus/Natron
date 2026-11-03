/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), data/kernels/denoiseprofile.cl
 * (denoiseprofile_decompose, denoiseprofile_reduce_first, denoiseprofile_reduce_second,
 * denoiseprofile_synthesize) and src/iop/denoiseprofile.c (process_wavelets_cl), Copyright (C)
 * 2011-2026 darktable developers, distributed under the GNU GPL version 3 or later, with the
 * arithmetic of the CPU port in Engine/Nodes/Filter/DenoiseProfiledWavelets.cpp.
 *
 * DenoiseProfiled's edge-aware a-trous wavelets over the variance-stabilised image, three floats a
 * pixel packed over a width x height buffer, rows bottom-up. A level decomposes buf1 into its coarse
 * part in buf2 (or the other way round), sums the squared details a group, reduces those sums into
 * the level's thresholds in paramBuf, then adds the shrunk details to outBuf. The chain stays on the
 * device: the thresholds are never read back.
 */

RWStructuredBuffer<float> buf1;
RWStructuredBuffer<float> buf2;
RWStructuredBuffer<float> outBuf;
// Three sums a decompose group.
RWStructuredBuffer<float> partialBuf;
// The constants of denoiseprofile_vst.slang, then a level's constants and thresholds below.
RWStructuredBuffer<float> paramBuf;

// DenoiseProfiledWaveletsGpuParams in Engine/Nodes/Filter/DenoiseProfiledGpu.cpp mirrors it.
struct WaveletParams
{
    uint width;
    uint height;
    // The level, and its tap spacing 1 << scale.
    uint scale;
    int mult;
    // Whether the level reads buf2 and writes buf1.
    uint swap;
    // Whether outBuf starts at 0 on this level, and whether the coarse part is added after it.
    uint first;
    uint last;
    // Decompose groups across, and in all.
    uint groupsX;
    uint groups;
};
[[vk::push_constant]] ConstantBuffer<WaveletParams> params;

// Offsets in paramBuf; kDenoiseProfiledGpu* in Engine/Nodes/Filter/DenoiseProfiledGpu.cpp mirror them.
// (float)npixels - 1, the variance's denominator.
static const uint kVarianceDenominator = 44;
// kLevelStride floats a level: 1 / sigmaBand^2, sigmaBand^2, then adjt * sigmaBand^2 a channel.
static const uint kLevels = 48;
static const uint kLevelStride = 8;
// Three thresholds a level, written by denoiseprofile_wav_reduce.
static const uint kThresholds = 104;

static const uint kGroup = 256;
groupshared float sums[3][kGroup];

float stdMax(float a, float b)
{
    return (a < b) ? b : a;
}

float stdMin(float a, float b)
{
    return (b < a) ? b : a;
}

// fast_mexp2f() with darktable's float arithmetic, on which the edge weights depend.
float fastMexp2fFloat(float x)
{
    if (!(x < 200.0f))
        return 0.0f;
    precise float k0 = 1065353216.0f + x * -8388608.0f;
    return asfloat(k0 >= 8388608.0f ? int(k0) : 0);
}

float3 load(bool second, uint k)
{
    return second ? float3(buf2[k], buf2[k + 1], buf2[k + 2]) : float3(buf1[k], buf1[k + 1], buf1[k + 2]);
}

void reduceGroup(uint index)
{
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kGroup / 2; stride > 0; stride /= 2)
    {
        if (index < stride)
        {
            for (uint c = 0; c < 3; ++c)
            {
                precise float s = sums[c][index] + sums[c][index + stride];
                sums[c][index] = s;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

// eaw_dn_decompose(): the coarse part of one level, with the edges clamped.
[shader("compute")]
[numthreads(16, 16, 1)]
void denoiseprofile_wav_decompose(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint index : SV_GroupIndex)
{
    const bool inside = (tid.x < params.width) && (tid.y < params.height);
    float3 squares = float3(0.0f, 0.0f, 0.0f);
    if (inside)
    {
        const bool second = (params.swap != 0);
        const int width = int(params.width);
        const int height = int(params.height);
        const int i = int(tid.x);
        const int j = int(tid.y);
        const float invSigma2 = paramBuf[kLevels + kLevelStride * params.scale];
        const float kFilter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
        const uint k = 3 * (uint(j) * params.width + uint(i));
        const float3 px = load(second, k);
        precise float3 sum = float3(0.0f, 0.0f, 0.0f);
        precise float wgt = 0.0f;
        for (int jj = 0; jj < 5; ++jj)
        {
            const int row = clamp(j + params.mult * (jj - 2), 0, height - 1);
            for (int ii = 0; ii < 5; ++ii)
            {
                const int col = clamp(i + params.mult * (ii - 2), 0, width - 1);
                const float3 px2 = load(second, 3 * (uint(row) * params.width + uint(col)));
                precise float d0 = px.x - px2.x;
                precise float d1 = px.y - px2.y;
                precise float d2 = px.z - px2.z;
                precise float dot = (d0 * d0 + d1 * d1 + d2 * d2) * invSigma2;
                precise float filter = kFilter[jj] * kFilter[ii];
                precise float w = filter * fastMexp2fFloat(stdMax(0.0f, dot * 0.02f - 9.0f));
                wgt += w;
                sum.x += w * px2.x;
                sum.y += w * px2.y;
                sum.z += w * px2.z;
            }
        }
        precise float3 coarse = sum / wgt;
        precise float3 detail = px - coarse;
        squares = detail * detail;
        if (second)
        {
            buf1[k] = coarse.x;
            buf1[k + 1] = coarse.y;
            buf1[k + 2] = coarse.z;
        }
        else
        {
            buf2[k] = coarse.x;
            buf2[k + 1] = coarse.y;
            buf2[k + 2] = coarse.z;
        }
    }
    sums[0][index] = squares.x;
    sums[1][index] = squares.y;
    sums[2][index] = squares.z;
    reduceGroup(index);
    if (index == 0)
    {
        const uint g = gid.y * params.groupsX + gid.x;
        for (uint c = 0; c < 3; ++c)
            partialBuf[3 * g + c] = sums[c][0];
    }
}

// variance_stabilizing_xform(): the level's BayesShrink thresholds from the groups' sums.
[shader("compute")]
[numthreads(256, 1, 1)]
void denoiseprofile_wav_reduce(uint index : SV_GroupIndex)
{
    for (uint c = 0; c < 3; ++c)
    {
        // Compensated, so that a large frame's sum loses no more than the tree does.
        precise float s = 0.0f;
        precise float carry = 0.0f;
        for (uint g = index; g < params.groups; g += kGroup)
        {
            precise float y = partialBuf[3 * g + c] - carry;
            precise float t = s + y;
            carry = (t - s) - y;
            s = t;
        }
        sums[c][index] = s;
    }
    reduceGroup(index);
    if (index == 0)
    {
        const uint level = kLevels + kLevelStride * params.scale;
        const float sb2 = paramBuf[level + 1];
        for (uint c = 0; c < 3; ++c)
        {
            precise float varY = sums[c][0] / paramBuf[kVarianceDenominator];
            precise float stdX = sqrt(stdMax(1e-6f, varY - sb2));
            precise float threshold = paramBuf[level + 2 + c] / stdX;
            paramBuf[kThresholds + 3 * params.scale + c] = threshold;
        }
    }
}

// The level's shrunk details added to outBuf, and after the last level its coarse part.
[shader("compute")]
[numthreads(16, 16, 1)]
void denoiseprofile_wav_shrink(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const bool second = (params.swap != 0);
    const uint k = 3 * (tid.y * params.width + tid.x);
    const float3 px = load(second, k);
    const float3 coarse = load(!second, k);
    const uint thresholds = kThresholds + 3 * params.scale;
    for (uint c = 0; c < 3; ++c)
    {
        const float thr = paramBuf[thresholds + c];
        precise float d = px[c] - coarse[c];
        precise float shrunk = stdMax(d - thr, 0.0f) + stdMin(d + thr, 0.0f);
        precise float o = ((params.first != 0) ? 0.0f : outBuf[k + c]) + shrunk;
        if (params.last != 0)
            o = o + coarse[c];
        outBuf[k + c] = o;
    }
}
