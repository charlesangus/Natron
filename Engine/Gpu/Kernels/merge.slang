import pixelwrap;
import compositestep;
import compositealpha;
import mergeops;

// Merge's step (NativeMergeOp in Engine/Nodes/Merge/Merge.cpp): the next A input, of alpha
// inputAlpha, merged over the result so far with one of mergeops' operators. Every input of a Merge
// has an alpha source, so the step carries the result's alpha (compositealpha.slang), which merges
// with the operator as a channel does, or with over for a non-separable operator, whose blend has
// no meaning on alphas alone. A per-channel alpha layer arrives through the alpha views, so the
// parameter block needs no stride of its own.
//
// A pixel with nothing in A, such as one outside every A input's region of definition, keeps the
// result unchanged when the operator leaves B unchanged under a transparent A, as the CPU step's
// copy does, without the operator's arithmetic.
//
// This entry lives apart from mergeops because each global of an imported module becomes a binding
// of every kernel that imports it, and mergeops is also built alone as the CPU oracle.

struct StepMerge : ICompositeAlphaStep
{
    uint op;
    uint identityForTransparentA;

    // Parameter block, in words: the operator's index in mergeops, then whether a transparent A
    // leaves B unchanged under it (0 or 1).
    [mutating] void load(StructuredBuffer<uint> params)
    {
        op = params[0];
        identityForTransparentA = params[1];
    }

    // The result and its alpha are B's side of the operator, the step's input and its alpha A's.
    void apply(inout float4 result, inout float4 resultAlpha, float4 input, float4 inputAlpha, int2 xy)
    {
        if (identityForTransparentA != 0 && all(input == float4(0.0)) && all(inputAlpha == float4(0.0)))
            return;
        const uint alphaOp = mergeIsSeparable(op) ? op : MERGE_OVER;
        const float4 merged = mergePixel(op, input, inputAlpha, result, resultAlpha);
        const float4 mergedAlpha = mergePixel(alphaOp, inputAlpha, inputAlpha, resultAlpha, resultAlpha);
        result = merged;
        resultAlpha = mergedAlpha;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void merge(uint3 tid : SV_DispatchThreadID)
{
    StepMerge op;
    runCompositeAlphaStep<StepMerge>(op, tid);
}
