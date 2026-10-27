/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), data/kernels/nlmeans.cl
 * and src/common/nlmeans_core.c, Copyright (C) 2011-2026 darktable developers, distributed under the
 * GNU GPL version 3 or later, with the arithmetic of the CPU port in
 * Engine/Nodes/Filter/DenoiseProfiledNlm.cpp.
 *
 * DenoiseProfiled's non-local means over the variance-stabilised image, three floats a pixel packed
 * over a width x height buffer, rows bottom-up. Each group takes a 16 x 16 tile of the window and
 * walks the patches in the CPU's order, so that every pixel's weighted sums add up in that order;
 * only a patch distance, summed directly here and by running sums on the CPU, rounds differently.
 */

RWStructuredBuffer<float> preBuf;
// acc / weight over the window, three floats a pixel packed over it.
RWStructuredBuffer<float> outBuf;
// The scattered shifts, rows then columns, a pair a patch.
RWStructuredBuffer<int> patchBuf;

// DenoiseProfiledNlmGpuParams in Engine/Nodes/Filter/DenoiseProfiledGpu.cpp mirrors it.
struct NlmParams
{
    int width;
    int height;
    // The window, in the buffer's pixels.
    int windowX;
    int windowY;
    int windowWidth;
    int windowHeight;
    uint nPatches;
    int radius;
    // centerWeight * (2 radius + 1)^2.
    float cpNorm;
    // 1 + centerWeight.
    float onePlusCenterWeight;
    float sharpness;
};
[[vk::push_constant]] ConstantBuffer<NlmParams> params;

static const int kTile = 16;
static const int kMaxRadius = 12;
static const int kSpan = kTile + 2 * kMaxRadius;

// A patch's pixel distances over the tile and its border, then their sums down 2 radius + 1 rows.
groupshared float distances[kSpan * kSpan];
groupshared float columns[kTile * kSpan];

float stdMax(float a, float b)
{
    return (a < b) ? b : a;
}

// fast_mexp2f() with darktable's integer arithmetic; the cut at 200 keeps the conversion in range.
float fastMexp2fInt(float x)
{
    if (!(x < 200.0f))
        return 0.0f;
    const int k0 = 0x3f800000 + int(x * -8388608.0f);
    return asfloat(k0 >= 0x800000 ? k0 : 0);
}

float3 pixelAt(int x, int y)
{
    const uint k = 3 * (uint(y) * uint(params.width) + uint(x));
    return float3(preBuf[k], preBuf[k + 1], preBuf[k + 2]);
}

float pixelDifference(float3 a, float3 b)
{
    precise float d0 = a.x - b.x;
    precise float d1 = a.y - b.y;
    precise float d2 = a.z - b.z;
    precise float d = d0 * d0 + d1 * d1 + d2 * d2;
    return d;
}

bool inBuffer(int x, int y)
{
    return (x >= 0) && (x < params.width) && (y >= 0) && (y < params.height);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void denoiseprofile_nlm(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    const int radius = params.radius;
    const int span = kTile + 2 * radius;
    const int x0 = params.windowX + int(gid.x) * kTile;
    const int y0 = params.windowY + int(gid.y) * kTile;
    const int x = x0 + int(lid.x);
    const int y = y0 + int(lid.y);
    const bool inWindow = (x < params.windowX + params.windowWidth) && (y < params.windowY + params.windowHeight);
    const float3 px = inWindow ? pixelAt(x, y) : float3(0.0f, 0.0f, 0.0f);
    precise float3 acc = float3(0.0f, 0.0f, 0.0f);
    precise float weight = 0.0f;

    // Every thread reaches every barrier: the loop bounds are uniform over the group.
    for (uint p = 0; p < params.nPatches; ++p)
    {
        const int dr = patchBuf[2 * p];
        const int dc = patchBuf[2 * p + 1];
        // A pixel and its shifted pixel both in the buffer contribute, others are 0.
        for (int k = int(index); k < span * span; k += kTile * kTile)
        {
            const int r = y0 - radius + k / span;
            const int c = x0 - radius + k % span;
            float d = 0.0f;
            if (inBuffer(c, r) && inBuffer(c + dc, r + dr))
                d = pixelDifference(pixelAt(c, r), pixelAt(c + dc, r + dr));
            distances[k] = d;
        }
        GroupMemoryBarrierWithGroupSync();
        for (int k = int(index); k < kTile * span; k += kTile * kTile)
        {
            const int i = k / span;
            const int j = k % span;
            precise float s = 0.0f;
            for (int t = 0; t <= 2 * radius; ++t)
                s += distances[(i + t) * span + j];
            columns[k] = s;
        }
        GroupMemoryBarrierWithGroupSync();
        if (inWindow && inBuffer(x + dc, y + dr))
        {
            precise float distortion = 0.0f;
            for (int t = 0; t <= 2 * radius; ++t)
                distortion += columns[int(lid.y) * span + int(lid.x) + t];
            const float3 shifted = pixelAt(x + dc, y + dr);
            precise float dissimilarity = (distortion + params.cpNorm * pixelDifference(px, shifted)) / params.onePlusCenterWeight;
            const float wt = fastMexp2fInt(stdMax(0.0f, dissimilarity * params.sharpness - 2.0f));
            acc.x += shifted.x * wt;
            acc.y += shifted.y * wt;
            acc.z += shifted.z * wt;
            weight += wt;
        }
    }
    if (!inWindow)
        return;
    const uint o = 3 * (uint(y - params.windowY) * uint(params.windowWidth) + uint(x - params.windowX));
    precise float3 result = acc / weight;
    outBuf[o] = result.x;
    outBuf[o + 1] = result.y;
    outBuf[o + 2] = result.z;
}
