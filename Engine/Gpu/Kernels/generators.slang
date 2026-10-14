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

// The host classifies every column and row of the region of definition with the CPU's double
// comparisons and packs the classes two bits each; the device only looks them up, so it matches
// the CPU wherever double rounding decides a pixel. Parameter words: 0..23 the six colours (box
// 0..3, line, centreline), 24 and 25 the first column and row, 26 and 27 their counts, 28 and
// 29 the word offsets of the column and row tables.
// Column classes: 0 centreline, 1 line, 2 even box, 3 odd box. Row classes are the same.
struct CheckerBoardOp : IPixelOp
{
    int baseX;
    int baseY;
    int countX;
    int countY;
    uint offsetX;
    uint offsetY;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        baseX = int(params[24]);
        baseY = int(params[25]);
        countX = int(params[26]);
        countY = int(params[27]);
        offsetX = params[28];
        offsetY = params[29];
    }

    uint classOf(uint offset, int i, int count)
    {
        const uint n = uint(clamp(i, 0, count - 1));
        return (wrapAux[offset + (n >> 4)] >> ((n & 15u) * 2u)) & 3u;
    }

    float4 apply(float4 v, int2 xy)
    {
        const uint rc = classOf(offsetY, xy.y - baseY, countY);
        uint color;
        if (rc < 2u)
        {
            color = rc == 0u ? 5u : 4u;
        }
        else
        {
            const uint cc = classOf(offsetX, xy.x - baseX, countX);
            if (cc < 2u)
                color = cc == 0u ? 5u : 4u;
            else if (rc == 3u)
                color = cc == 3u ? 2u : 3u;
            else
                color = cc == 3u ? 1u : 0u;
        }
        return wrapParamFloat4(wrapAux, color * 4u);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void checkerboard_main(uint3 tid : SV_DispatchThreadID)
{
    CheckerBoardOp op;
    runWrapped<CheckerBoardOp>(op, tid);
}
