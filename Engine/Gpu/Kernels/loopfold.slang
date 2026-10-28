// LoopEnd's fold step (LoopFold::foldRow and finishRow in Engine/Nodes/Loop/LoopFold.cpp): the
// accumulator so far, step k - 1's output or the operation's initial value, with iteration k's image
// folded in, written to the destination over the output rect. Components are folded by index, as the
// CPU does, an image component the source does not have and a pixel outside its valid window reading
// as zero. LoopFoldGpuParams in Engine/Nodes/Loop/LoopFold.cpp mirrors LoopFoldParams.

struct LoopFoldParams
{
    uint width;
    uint height;
    uint dstOffset;
    uint dstStride;
    uint nComps;
    uint hasPrev;
    uint prevOffset;
    uint prevStride;
    uint srcOffset;
    uint srcStride;
    uint srcComps;
    uint validX1;
    uint validY1;
    uint validX2;
    uint validY2;
    // -1 for a layer with no alpha.
    int alphaIndex;
    // The accumulator where there is no previous step.
    float initial;
    // Mean's last step divides by it; 0 for no division.
    float divisor;
};

[[vk::binding(0, 0)]] StructuredBuffer<float> foldPrev;
[[vk::binding(1, 0)]] StructuredBuffer<float> foldSrc;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> foldDst;
[[vk::push_constant]] ConstantBuffer<LoopFoldParams> foldParams;

interface ILoopFoldOp
{
    float fold(float acc, float value, float srcAlpha, float accAlpha);
}

// `precise` keeps the multiplies and adds apart, as the CPU's unfused arithmetic has them.
struct FoldSum : ILoopFoldOp
{
    float fold(float acc, float value, float srcAlpha, float accAlpha)
    {
        precise float r = acc + value;
        return r;
    }
}

// std::min and std::max, comparison for comparison, so that signed zeros and NaNs pick the same side.
struct FoldMin : ILoopFoldOp
{
    float fold(float acc, float value, float srcAlpha, float accAlpha)
    {
        return (value < acc) ? value : acc;
    }
}

struct FoldMax : ILoopFoldOp
{
    float fold(float acc, float value, float srcAlpha, float accAlpha)
    {
        return (acc < value) ? value : acc;
    }
}

struct FoldOver : ILoopFoldOp
{
    float fold(float acc, float value, float srcAlpha, float accAlpha)
    {
        precise float r = value + acc * (1.0 - srcAlpha);
        return r;
    }
}

struct FoldUnder : ILoopFoldOp
{
    float fold(float acc, float value, float srcAlpha, float accAlpha)
    {
        precise float r = acc + value * (1.0 - accAlpha);
        return r;
    }
}

// The source as it is, zero outside its valid window: LoopBegin's feedback forward.
struct FoldCopy : ILoopFoldOp
{
    float fold(float acc, float value, float srcAlpha, float accAlpha)
    {
        return value;
    }
}

void runLoopFold<Op : ILoopFoldOp>(Op op, uint3 tid)
{
    if (tid.x >= foldParams.width || tid.y >= foldParams.height)
        return;
    const uint n = min(foldParams.nComps, 4u);
    const uint dst = foldParams.dstOffset + tid.y * foldParams.dstStride + tid.x * foldParams.nComps;

    float acc[4] = { foldParams.initial, foldParams.initial, foldParams.initial, foldParams.initial };
    if (foldParams.hasPrev != 0)
    {
        const uint prev = foldParams.prevOffset + tid.y * foldParams.prevStride + tid.x * foldParams.nComps;
        for (uint c = 0; c < n; ++c)
            acc[c] = foldPrev[prev + c];
    }

    const bool inside = tid.x >= foldParams.validX1 && tid.x < foldParams.validX2
                        && tid.y >= foldParams.validY1 && tid.y < foldParams.validY2;
    uint src = 0;
    if (inside)
        src = foldParams.srcOffset + (tid.y - foldParams.validY1) * foldParams.srcStride
              + (tid.x - foldParams.validX1) * foldParams.srcComps;
    const int alpha = foldParams.alphaIndex;
    const float srcAlpha = (inside && alpha >= 0 && uint(alpha) < foldParams.srcComps) ? foldSrc[src + uint(alpha)] : 0.0;
    // Read before the alpha component itself is folded.
    const float accAlpha = (alpha >= 0 && uint(alpha) < n) ? acc[alpha] : 0.0;

    for (uint c = 0; c < n; ++c)
    {
        const float value = (inside && c < foldParams.srcComps) ? foldSrc[src + c] : 0.0;
        float r = op.fold(acc[c], value, srcAlpha, accAlpha);
        if (foldParams.divisor != 0.0)
            r = r / foldParams.divisor;
        foldDst[dst + c] = r;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void loopfold_mean(uint3 tid : SV_DispatchThreadID)
{
    FoldSum op;
    runLoopFold<FoldSum>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void loopfold_sum(uint3 tid : SV_DispatchThreadID)
{
    FoldSum op;
    runLoopFold<FoldSum>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void loopfold_min(uint3 tid : SV_DispatchThreadID)
{
    FoldMin op;
    runLoopFold<FoldMin>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void loopfold_max(uint3 tid : SV_DispatchThreadID)
{
    FoldMax op;
    runLoopFold<FoldMax>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void loopfold_over(uint3 tid : SV_DispatchThreadID)
{
    FoldOver op;
    runLoopFold<FoldOver>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void loopfold_under(uint3 tid : SV_DispatchThreadID)
{
    FoldUnder op;
    runLoopFold<FoldUnder>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void loopfold_copy(uint3 tid : SV_DispatchThreadID)
{
    FoldCopy op;
    runLoopFold<FoldCopy>(op, tid);
}
