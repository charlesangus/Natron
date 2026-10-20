import pixelwrap;

// One step of a Composite op's fold: the result so far, the source WrapParams describes, combined
// with the next input into the destination over the output rect. WrapParams' mask fields describe
// the input's view in stepInput, maskComps being its channel count; the mask itself, mix and the
// pass-through apply once to the final result, through wrap_finish. GpuWrapStep in
// Engine/Gpu/GpuWrap.h packs these.
//
// stepInput lives here rather than in pixelwrap.slang because every global of an imported module
// is reflected as a binding of each kernel that imports it.

[[vk::binding(4, 0)]] StructuredBuffer<float> stepInput;

// The op sees and returns pixels in channel-bit order, absent channels reading as zero. Every
// channel of its result is written; only those in channelMask reach the output.
interface ICompositeStep
{
    [mutating] void load(StructuredBuffer<uint> params);
    float4 apply(float4 result, float4 input, int2 xy);
}

// The component of an nComps pixel that holds channel bit, -1 for none.
int stepChannelIndex(uint nComps, uint bit)
{
    if (nComps == 1)
        return bit == 3 ? 0 : -1;
    return bit < nComps ? int(bit) : -1;
}

float4 stepReadInput(int2 p)
{
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    const uint comps = wrap.maskComps;
    if (comps == 0 || !wrapInside(p, wrap.maskX1, wrap.maskY1, wrap.maskX2, wrap.maskY2))
        return v;
    const uint base = wrap.maskOffset + uint(p.y - wrap.maskY1) * wrap.maskStride + uint(p.x - wrap.maskX1) * comps;
    const uint n = wrap.nComps;
    for (uint c = 0; c < n && c < 4; ++c)
    {
        const uint bit = wrapChannelBit(n, c);
        const int i = stepChannelIndex(comps, bit);
        if (i >= 0)
            v[bit] = stepInput[base + uint(i)];
    }
    return v;
}

void runCompositeStep<Op : ICompositeStep>(Op op, uint3 tid)
{
    if (wrapOutside(tid))
        return;
    const int2 p = wrapPixel(tid);
    Op o = op;
    o.load(wrapAux);
    const float4 r = o.apply(wrapReadSource(p), stepReadInput(p), p);
    const uint n = wrap.nComps;
    const uint base = wrap.dstOffset + tid.y * wrap.dstStride + tid.x * n;
    for (uint c = 0; c < n && c < 4; ++c)
        wrapDst[base + c] = r[wrapChannelBit(n, c)];
}

// Halves the result and adds the input, so the order of the fold shows in the result. Halving is
// exact, so a fused multiply-add rounds as the separate operations do.
struct StepHalveAdd : ICompositeStep
{
    [mutating] void load(StructuredBuffer<uint> params) {}

    float4 apply(float4 result, float4 input, int2 xy)
    {
        precise float4 r = result * 0.5 + input;
        return r;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositestep_halveadd(uint3 tid : SV_DispatchThreadID)
{
    StepHalveAdd op;
    runCompositeStep<StepHalveAdd>(op, tid);
}
