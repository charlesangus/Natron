import pixelwrap;

// RawInputColor: red, green and blue through the camera to working space matrix, in the order
// RawInputColorOp::row() sums them; a pixel without three channels passes through.
struct InputColorOp : IPixelOp
{
    float m[9];

    // Parameter block, in words: the row-major matrix (9).
    [mutating] void load(StructuredBuffer<uint> params)
    {
        [ForceUnroll]
        for (uint i = 0; i < 9; ++i)
            m[i] = asfloat(params[i]);
    }

    float4 apply(float4 v, int2 xy)
    {
        if (wrap.nComps < 3)
            return v;
        const float r = v.x;
        const float g = v.y;
        const float b = v.z;
        precise float outR = m[0] * r + m[1] * g + m[2] * b;
        precise float outG = m[3] * r + m[4] * g + m[5] * b;
        precise float outB = m[6] * r + m[7] * g + m[8] * b;
        return float4(outR, outG, outB, v.w);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rawinputcolor_wrapped(uint3 tid : SV_DispatchThreadID)
{
    InputColorOp op;
    runWrapped<InputColorOp>(op, tid);
}
