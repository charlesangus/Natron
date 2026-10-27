import pixelwrap;
import compositestep;

// One step of a running weighted mean of one input read at several times.
struct StepTimeBlend : ICompositeStep
{
    float blend;

    // Parameter block, in words: the step's weight over the sum of the weights up to it.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        blend = wrapParamFloat(params, 0);
    }

    // `precise` keeps the multiply and add apart, as the CPU's unfused arithmetic has them.
    float4 apply(float4 result, float4 input, int2 xy)
    {
        precise float4 r = (input - result) * blend + result;
        return r;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void timeblend(uint3 tid : SV_DispatchThreadID)
{
    StepTimeBlend op;
    runCompositeStep<StepTimeBlend>(op, tid);
}
