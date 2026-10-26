import pixelwrap;
import compositestep;

// One step of a Composite op's fold that carries the result's alpha, as CompositeOp in
// Engine/Nodes/Image/Op.h describes it for an op whose inputs have alpha sources: the step reads
// the result's alpha and the step input's alpha, and writes the new result's alpha beside the new
// result. Everything else is as runCompositeStep (compositestep.slang) has it. GpuWrapStep in
// Engine/Gpu/GpuWrap.h packs these.
//
// stepAlphaGeometry holds, in words, a view of stepResultAlpha then one of stepInputAlpha, each
// its offset, row stride, x1, y1, x2, y2, channel count and channel, then the destination alpha's
// offset and row stride, from the output's origin, in the destination's layout. A view reads zero
// outside its bounds and everywhere when it has no channels. Its channel is the alpha of every
// channel, kStepAlphaPerChannel to read the view per channel by channel bit, as stepInput is, or
// kStepAlphaPerWrittenChannel to read its channel i as the alpha of the i-th channel the wrap's
// channelMask writes, the others reading zero.
//
// These live in their own module so that a step without alpha keeps compositestep.slang's bindings.

[[vk::binding(5, 0)]] StructuredBuffer<uint> stepAlphaGeometry;
[[vk::binding(6, 0)]] StructuredBuffer<float> stepResultAlpha;
[[vk::binding(7, 0)]] StructuredBuffer<float> stepInputAlpha;
[[vk::binding(8, 0)]] RWStructuredBuffer<float> stepDstAlpha;

static const uint kStepAlphaViewWords = 8;
static const uint kStepAlphaPerChannel = 0xFFFFFFFF;
static const uint kStepAlphaPerWrittenChannel = 0xFFFFFFFE;

// The op sees and returns pixels and their alphas in channel-bit order, absent channels reading as
// zero. It updates result and resultAlpha, the result so far and its alpha, with input, of alpha
// inputAlpha. Every channel of both is written; only those in channelMask reach the output.
interface ICompositeAlphaStep
{
    [mutating] void load(StructuredBuffer<uint> params);
    void apply(inout float4 result, inout float4 resultAlpha, float4 input, float4 inputAlpha, int2 xy);
}

// The alpha of the view at word `view` of stepAlphaGeometry in buffer `alpha`, at p.
float4 stepReadAlpha(StructuredBuffer<float> alpha, uint view, int2 p)
{
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    const uint comps = stepAlphaGeometry[view + 6];
    const int x1 = int(stepAlphaGeometry[view + 2]);
    const int y1 = int(stepAlphaGeometry[view + 3]);
    if (comps == 0 || !wrapInside(p, x1, y1, int(stepAlphaGeometry[view + 4]), int(stepAlphaGeometry[view + 5])))
        return v;
    const uint base = stepAlphaGeometry[view] + uint(p.y - y1) * stepAlphaGeometry[view + 1] + uint(p.x - x1) * comps;
    const uint channel = stepAlphaGeometry[view + 7];
    const uint n = wrap.nComps;
    if (channel == kStepAlphaPerWrittenChannel)
    {
        // The channels channelMask leaves out pass through unprocessed and need no alpha.
        uint next = 0;
        for (uint c = 0; c < n && c < 4; ++c)
        {
            const uint bit = wrapChannelBit(n, c);
            if ((wrap.channelMask & (1u << bit)) == 0)
                continue;
            if (next < comps)
                v[bit] = alpha[base + next];
            ++next;
        }
        return v;
    }
    if (channel != kStepAlphaPerChannel)
    {
        const float a = alpha[base + channel];
        return float4(a, a, a, a);
    }
    for (uint c = 0; c < n && c < 4; ++c)
    {
        const uint bit = wrapChannelBit(n, c);
        const int i = stepChannelIndex(comps, bit);
        if (i >= 0)
            v[bit] = alpha[base + uint(i)];
    }
    return v;
}

void runCompositeAlphaStep<Op : ICompositeAlphaStep>(Op op, uint3 tid)
{
    if (wrapOutside(tid))
        return;
    const int2 p = wrapPixel(tid);
    Op o = op;
    o.load(wrapAux);
    float4 r = wrapReadSource(p);
    float4 a = stepReadAlpha(stepResultAlpha, 0, p);
    o.apply(r, a, stepReadInput(p), stepReadAlpha(stepInputAlpha, kStepAlphaViewWords, p), p);
    const uint n = wrap.nComps;
    const uint base = wrap.dstOffset + tid.y * wrap.dstStride + tid.x * n;
    const uint alphaBase = stepAlphaGeometry[2 * kStepAlphaViewWords] + tid.y * stepAlphaGeometry[2 * kStepAlphaViewWords + 1] + tid.x * n;
    for (uint c = 0; c < n && c < 4; ++c)
    {
        const uint bit = wrapChannelBit(n, c);
        wrapDst[base + c] = r[bit];
        stepDstAlpha[alphaBase + c] = a[bit];
    }
}

// Porter-Duff xor of the input over the result, alpha by alpha: the test op of
// Tests/Native/StagedImageEffectGpu_Test.cpp. `precise` keeps the multiplies and the add apart, as
// the CPU's unfused arithmetic has them.
struct StepAlphaXor : ICompositeAlphaStep
{
    [mutating] void load(StructuredBuffer<uint> params) {}

    void apply(inout float4 result, inout float4 resultAlpha, float4 input, float4 inputAlpha, int2 xy)
    {
        precise float4 r = input * (1.0 - resultAlpha) + result * (1.0 - inputAlpha);
        precise float4 a = inputAlpha * (1.0 - resultAlpha) + resultAlpha * (1.0 - inputAlpha);
        result = r;
        resultAlpha = a;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositealpha_xor(uint3 tid : SV_DispatchThreadID)
{
    StepAlphaXor op;
    runCompositeAlphaStep<StepAlphaXor>(op, tid);
}
