import pixelwrap;

// runWrapped() for an op that reads several inputs at the output pixel: the op's result is blended
// with the blend source in wrapSrc by mask x mix, and unprocessed channels pass through from it, as
// for a one-source op. No input is divided, since no one input's divisor applies to the others.
// GpuWrapSourceBlock in Engine/Gpu/GpuWrap.h mirrors the view block.
//
// These bindings live here rather than in pixelwrap.slang because every global of an imported
// module is reflected as a binding of each kernel that imports it.

[[vk::binding(4, 0)]] StructuredBuffer<float> wrapSource0;
[[vk::binding(5, 0)]] StructuredBuffer<float> wrapSource1;
[[vk::binding(6, 0)]] StructuredBuffer<float> wrapSource2;
[[vk::binding(7, 0)]] StructuredBuffer<float> wrapSource3;
// Words: the source count, then per source its offset, stride, x1, y1, x2, y2, comps and channel.
[[vk::binding(8, 0)]] StructuredBuffer<uint> wrapSourceViews;

static const uint kWrapMaxSources = 4;
static const uint kWrapSourceViewWords = 8;

// The inputs at one pixel, count of them, each in channel-bit order with absent channels reading
// zero. An input read in colour has the output's channels. A Mask-role input is its one channel,
// in alpha like any one-channel pixel. An input reads zero wherever it has no pixel.
struct WrapSources
{
    float4 pixel[4];
    uint count;
}

// As IPixelOp, with every input the host lists instead of the one source.
interface ISourcesPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params);
    float4 apply(WrapSources sources, int2 xy);
}

float wrapSourceAt(uint s, uint index)
{
    switch (s)
    {
    case 0:
        return wrapSource0[index];
    case 1:
        return wrapSource1[index];
    case 2:
        return wrapSource2[index];
    default:
        return wrapSource3[index];
    }
}

// The component of an nComps pixel that holds channel bit, -1 for none.
int wrapChannelIndex(uint nComps, uint bit)
{
    if (nComps == 1)
        return bit == 3 ? 0 : -1;
    return bit < nComps ? int(bit) : -1;
}

float4 wrapReadSourceAt(uint s, int2 p)
{
    const uint w = 1 + s * kWrapSourceViewWords;
    const uint offset = wrapSourceViews[w];
    const uint stride = wrapSourceViews[w + 1];
    const int x1 = int(wrapSourceViews[w + 2]);
    const int y1 = int(wrapSourceViews[w + 3]);
    const int x2 = int(wrapSourceViews[w + 4]);
    const int y2 = int(wrapSourceViews[w + 5]);
    const uint comps = wrapSourceViews[w + 6];
    const int channel = int(wrapSourceViews[w + 7]);
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    if (comps == 0 || !wrapInside(p, x1, y1, x2, y2))
        return v;
    const uint base = offset + uint(p.y - y1) * stride + uint(p.x - x1) * comps;
    if (channel >= 0)
    {
        v.a = wrapSourceAt(s, base + uint(channel));
        return v;
    }
    const uint n = wrap.nComps;
    for (uint c = 0; c < n && c < 4; ++c)
    {
        const uint bit = wrapChannelBit(n, c);
        const int i = wrapChannelIndex(comps, bit);
        if (i >= 0)
            v[bit] = wrapSourceAt(s, base + uint(i));
    }
    return v;
}

void runWrappedSources<Op : ISourcesPixelOp>(Op op, uint3 tid)
{
    if (wrapOutside(tid))
        return;
    const int2 p = wrapPixel(tid);
    WrapSources sources;
    sources.count = min(wrapSourceViews[0], kWrapMaxSources);
    for (uint s = 0; s < kWrapMaxSources; ++s)
        sources.pixel[s] = s < sources.count ? wrapReadSourceAt(s, p) : float4(0.0, 0.0, 0.0, 0.0);
    Op o = op;
    o.load(wrapAux);
    wrapWrite(tid.xy, p, wrapReadSource(p), o.apply(sources, p), 1.0);
}

struct WrapTwoSources : ISourcesPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params) {}

    float4 apply(WrapSources sources, int2 xy)
    {
        return sources.pixel[0] + sources.pixel[1] * 0.5;
    }
}

// Two colour inputs weighted by two Mask-role inputs, the shape of a keyer's inputs.
struct WrapFourSources : ISourcesPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params) {}

    float4 apply(WrapSources sources, int2 xy)
    {
        precise float4 r = sources.pixel[0] * sources.pixel[1].a + sources.pixel[3] * sources.pixel[2].a;
        return r;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositewrap_two(uint3 tid : SV_DispatchThreadID)
{
    WrapTwoSources op;
    runWrappedSources<WrapTwoSources>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositewrap_four(uint3 tid : SV_DispatchThreadID)
{
    WrapFourSources op;
    runWrappedSources<WrapFourSources>(op, tid);
}
