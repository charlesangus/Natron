import pixelwrap;

// The colour in every pixel; the wrapper keeps only the processed channels and passes the rest
// through from the source, which reads zero when none is connected.
struct ConstantOp : IPixelOp
{
    float4 color;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        color = wrapParamFloat4(params, 0);
    }

    float4 apply(float4 v, int2 xy)
    {
        return color;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void constant_main(uint3 tid : SV_DispatchThreadID)
{
    ConstantOp op;
    runWrapped<ConstantOp>(op, tid);
}
