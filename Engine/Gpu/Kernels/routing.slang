import pixelwrap;
import compositestep;

// KeyMix: the step's input is A, which replaces B; the wrap blends by mask and mix and keeps
// B's unprocessed channels.
struct StepKeyMix : ICompositeStep
{
    // Parameter block, in words: one reserved word.
    [mutating] void load(StructuredBuffer<uint> params) {}

    float4 apply(float4 result, float4 input, int2 xy)
    {
        return input;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void keymix(uint3 tid : SV_DispatchThreadID)
{
    StepKeyMix op;
    runCompositeStep<StepKeyMix>(op, tid);
}

// Difference: the result is B and the step's input A. Colour is B's and alpha the gained sum of
// the squared colour differences less the offset, clamped below at 0. Absent channels read 0 in
// both, so they add nothing. `precise` keeps the arithmetic unfused, in the CPU step's order.
struct StepDifference : ICompositeStep
{
    float gain;
    float offset;

    // Parameter block, in words: gain, offset.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        gain = wrapParamFloat(params, 0);
        offset = wrapParamFloat(params, 1);
    }

    float4 apply(float4 result, float4 input, int2 xy)
    {
        precise float3 d = result.rgb - input.rgb;
        precise float sum = 0.0;
        sum = sum + d.r * d.r;
        sum = sum + d.g * d.g;
        sum = sum + d.b * d.b;
        precise float matte = gain * sum - offset;
        return float4(result.rgb, max(0.0, matte));
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void difference(uint3 tid : SV_DispatchThreadID)
{
    StepDifference op;
    runCompositeStep<StepDifference>(op, tid);
}
