import mapremap;

// One iteration of LoopEnd's Map Blend (LoopMapBlend::accumulate in Engine/Nodes/Loop/LoopMapBlend.cpp):
// iteration's weighted values added to the accumulator in the destination over the output rect, the
// weight being the one LoopMapBlend::tapsAt gives the iteration at the pixel's Map value. Iterations are
// dispatched in increasing order, so that each pixel sums its taps in the CPU's order. A pixel outside
// the source's valid window, or a component the source does not have, adds nothing, as on the CPU.
//
// The weights are the CPU's float operations in the same order, precise so that none contracts, and
// mapremap.slang computes the CPU's Map remap. LoopMapBlendGpuParams in
// Engine/Nodes/Loop/LoopMapBlend.cpp mirrors LoopMapBlendParams.

struct LoopMapBlendParams
{
    uint width;
    uint height;
    uint dstOffset;
    uint dstStride;
    uint nComps;
    // kBlend* flags.
    uint flags;
    uint srcOffset;
    uint srcStride;
    uint srcComps;
    uint validX1;
    uint validY1;
    uint validX2;
    uint validY2;
    // The Map's channel, of mapChannels floats per pixel, its pixels the output rect's
    // (mapX1, mapY1)-(mapX2, mapY2), the first at mapOffset, rows mapStride floats apart.
    uint mapChannel;
    uint mapChannels;
    uint mapOffset;
    uint mapStride;
    uint mapX1;
    uint mapY1;
    uint mapX2;
    uint mapY2;
    // MapInputParams::remap()'s exponent; kBlendMapSmooth in flags is its smooth.
    float exponent;
    // The Map value where the Map holds no pixel, as the CPU's MapPlane reads it.
    float outside;
    int iteration;
    int count;
    // LoopMapBlend::InterpolationEnum.
    uint interpolation;
};

[[vk::binding(0, 0)]] StructuredBuffer<float> blendMap;
[[vk::binding(1, 0)]] StructuredBuffer<float> blendSrc;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> blendDst;
[[vk::push_constant]] ConstantBuffer<LoopMapBlendParams> blendParams;

// The accumulator starts at zero rather than at the destination's values.
static const uint kBlendFirst = 1;
// The source is bound; without it the dispatch only writes the accumulator.
static const uint kBlendSource = 2;
static const uint kBlendMapRead = 4;
static const uint kBlendMapInvert = 8;
static const uint kBlendMapSmooth = 16;

static const uint kInterpolationLinear = 0;
static const uint kInterpolationCubic = 1;
static const uint kInterpolationNearest = 2;

float clampedMap(float m)
{
    if (!(m > 0.0))
        return 0.0;
    return (m < 1.0) ? m : 1.0;
}

// The weight LoopMapBlend::weightOf(tapsAt(m, count, interpolation), iteration) gives.
float tapWeight(float m, int count, int iteration, uint interpolation)
{
    if (count <= 1)
        return (iteration == 0) ? 1.0 : 0.0;
    const int last = count - 1;
    precise float u = clampedMap(m) * float(last);
    if (interpolation == kInterpolationNearest) {
        precise float rounded = u + 0.5;
        return (min(int(floor(rounded)), last) == iteration) ? 1.0 : 0.0;
    }
    const int k = min(int(floor(u)), last);
    precise float w = u - float(k);
    if (w == 0.0)
        return (k == iteration) ? 1.0 : 0.0;
    if (interpolation != kInterpolationCubic) {
        if (iteration == k) {
            precise float r = 1.0 - w;
            return r;
        }
        return (iteration == k + 1) ? w : 0.0;
    }
    precise float w2 = w * w;
    precise float w3 = w2 * w;
    precise float a0 = -w3 + 2.0 * w2;
    precise float b0 = a0 - w;
    precise float a1 = 3.0 * w3 - 5.0 * w2;
    precise float b1 = a1 + 2.0;
    precise float a2 = -3.0 * w3 + 4.0 * w2;
    precise float b2 = a2 + w;
    precise float b3 = w3 - w2;
    precise float c0 = 0.5 * b0;
    precise float c1 = 0.5 * b1;
    precise float c2 = 0.5 * b2;
    precise float c3 = 0.5 * b3;
    const float weights[4] = { c0, c1, c2, c3 };
    // Taps clamped onto the same end iteration merge, summed in tap order.
    precise float weight = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (clamp(k - 1 + i, 0, last) == iteration && weights[i] != 0.0) {
            precise float sum = weight + weights[i];
            weight = sum;
        }
    }
    return weight;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void loopmapblend_accumulate(uint3 tid : SV_DispatchThreadID)
{
    const LoopMapBlendParams p = blendParams;
    if (tid.x >= p.width || tid.y >= p.height)
        return;
    const uint n = min(p.nComps, 4u);
    const uint dst = p.dstOffset + tid.y * p.dstStride + tid.x * p.nComps;
    const bool first = (p.flags & kBlendFirst) != 0u;

    float weight = 0.0;
    uint src = 0;
    const bool inside = (p.flags & kBlendSource) != 0u
                        && tid.x >= p.validX1 && tid.x < p.validX2 && tid.y >= p.validY1 && tid.y < p.validY2;
    if (inside) {
        src = p.srcOffset + (tid.y - p.validY1) * p.srcStride + (tid.x - p.validX1) * p.srcComps;
        float m = p.outside;
        if ((p.flags & kBlendMapRead) != 0u && tid.x >= p.mapX1 && tid.x < p.mapX2 && tid.y >= p.mapY1 && tid.y < p.mapY2) {
            const float raw = blendMap[p.mapOffset + (tid.y - p.mapY1) * p.mapStride + (tid.x - p.mapX1) * p.mapChannels + p.mapChannel];
            m = mapRemap(raw, (p.flags & kBlendMapInvert) != 0u, (p.flags & kBlendMapSmooth) != 0u, p.exponent);
        }
        weight = tapWeight(m, p.count, p.iteration, p.interpolation);
    }

    for (uint c = 0; c < n; ++c) {
        float acc = first ? 0.0 : blendDst[dst + c];
        if (weight != 0.0 && c < p.srcComps) {
            precise float term = weight * blendSrc[src + c];
            precise float sum = acc + term;
            acc = sum;
        }
        blendDst[dst + c] = acc;
    }
}
