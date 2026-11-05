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

// Difference: the result is B and the step's input A. Colour is B's. Alpha remaps the sum of the
// squared colour differences so that the black point gives 0 and the white point 1 (scale is
// 1 / (white - black)), clamped below at 0 and times the gain; the alpha is then limited to 0 to 1; a hard threshold gives the gain
// above the black point and 0 elsewhere. Absent channels read 0 in both, so they add nothing.
// `precise` keeps the arithmetic unfused, in the CPU step's order.
struct StepDifference : ICompositeStep
{
    float blackPoint;
    float scale;
    float gain;
    uint threshold;

    // Parameter block, in words: black point, scale, gain, threshold (0 or 1).
    [mutating] void load(StructuredBuffer<uint> params)
    {
        blackPoint = wrapParamFloat(params, 0);
        scale = wrapParamFloat(params, 1);
        gain = wrapParamFloat(params, 2);
        threshold = params[3];
    }

    float4 apply(float4 result, float4 input, int2 xy)
    {
        precise float3 d = result.rgb - input.rgb;
        precise float sum = 0.0;
        sum = sum + d.r * d.r;
        sum = sum + d.g * d.g;
        sum = sum + d.b * d.b;
        precise float matte;
        if (threshold != 0) {
            matte = (sum > blackPoint) ? gain : 0.0;
        } else {
            precise float remapped = (sum - blackPoint) * scale;
            matte = gain * max(0.0, remapped);
        }
        // A comparison rather than clamp(), so that a NaN gives 0 as on the CPU.
        matte = (matte > 0.0) ? min(matte, 1.0) : 0.0;
        return float4(result.rgb, matte);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void difference(uint3 tid : SV_DispatchThreadID)
{
    StepDifference op;
    runCompositeStep<StepDifference>(op, tid);
}
