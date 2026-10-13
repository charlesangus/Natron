import pixelwrap;

// 1 - v on all four channels; the wrapper keeps only the processed ones.
struct InvertOp : IPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params) {}

    float4 apply(float4 v, int2 xy)
    {
        return float4(1.0, 1.0, 1.0, 1.0) - v;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void invert_main(uint3 tid : SV_DispatchThreadID)
{
    InvertOp op;
    runWrapped<InvertOp>(op, tid);
}
