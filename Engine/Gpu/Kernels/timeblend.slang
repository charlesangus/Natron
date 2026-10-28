import pixelwrap;
import compositestep;

// One step of a fold of one input read at several times, as TimeBlend::foldPixel() takes it on
// the CPU.
struct StepTimeBlend : ICompositeStep
{
    float blend;
    uint op;
    float factor;

    // Parameter block, in words: the step's weight over the sum of the weights up to it, the
    // operation (TimeBlend::Operation), and the decay applied to the result so far.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        blend = wrapParamFloat(params, 0);
        op = params[1];
        factor = wrapParamFloat(params, 2);
    }

    // `precise` keeps each multiply and add apart, as the CPU's unfused arithmetic has them.
    float4 apply(float4 result, float4 input, int2 xy)
    {
        if (op == 0)
        {
            precise float4 r = (input - result) * blend + result;
            return r;
        }
        precise float4 decayed = result * factor;
        if (op == 5)
        {
            const uint n = wrap.nComps;
            const float a = (n == 4 || n == 1) ? input.w : 1.0;
            precise float keep = 1.0 - a;
            precise float4 under = decayed * keep;
            precise float4 over = input + under;
            return over;
        }
        float4 r;
        for (int c = 0; c < 4; ++c)
        {
            const float i = input[c];
            const float d = decayed[c];
            if (op == 1)
                r[c] = i < d ? i : d;
            else if (op == 2)
                r[c] = i > d ? i : d;
            else if (op == 3)
            {
                precise float s = i + d;
                r[c] = s;
            }
            else
            {
                precise float p = i * d;
                r[c] = p;
            }
        }
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
