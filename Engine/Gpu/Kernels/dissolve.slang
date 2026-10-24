import pixelwrap;
import compositestep;

struct StepDissolve : ICompositeStep
{
    float blend;

    // Parameter block, in words: the blend, the fractional part of Dissolve's "which".
    [mutating] void load(StructuredBuffer<uint> params)
    {
        blend = wrapParamFloat(params, 0);
    }

    // The result is the lower input and the step's input the upper one, as the CPU step reads
    // them. `precise` keeps the multiply and add apart, as the CPU's unfused arithmetic has them.
    float4 apply(float4 result, float4 input, int2 xy)
    {
        precise float4 r = (input - result) * blend + result;
        return r;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void dissolve(uint3 tid : SV_DispatchThreadID)
{
    StepDissolve op;
    runCompositeStep<StepDissolve>(op, tid);
}
