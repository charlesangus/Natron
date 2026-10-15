import curvesnapshot;
import pixelwrap;

// Parameter block, in words. Every offset is mirrored by ColorLookupGpu in ColorLookup.cpp.
static const uint CL_CURVES = 5;
static const uint CL_HEADER_WORDS = 32;
static const uint CL_Y_MIN = 16;
static const uint CL_Y_MAX = 21;
static const uint CL_KEY_COUNT = 8;
static const uint CL_TABLE_RANGE = 26;
static const uint CL_LUT_BASE = CL_HEADER_WORDS;
static const uint CL_CURVE_BASE = CL_LUT_BASE + CL_CURVES * CS_TABLE_SIZE;

static const uint CL_MASTER = 0;
static const uint CL_RED = 1;
static const uint CL_GREEN = 2;
static const uint CL_BLUE = 3;
static const uint CL_ALPHA = 4;

static const uint CL_MODE_STANDARD = 0;
static const uint CL_MODE_WEIGHTED = 1;
static const uint CL_MODE_FILMLIKE = 2;
static const uint CL_MODE_LUMINANCE = 3;

// The tables are read from the bound parameter buffer, wrapAux, since an op cannot hold a buffer.
struct ColorLookupOp : IPixelOp
{
    uint mode;
    uint luminanceMath;
    bool clampBlack;
    bool clampWhite;
    CurveTableRange range;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        mode = params[0];
        luminanceMath = params[1];
        clampBlack = (params[2] & 1) != 0;
        clampWhite = (params[2] & 2) != 0;
        range.load(params, CL_TABLE_RANGE);
    }

    bool isStandard()
    {
        return mode == CL_MODE_STANDARD || mode == CL_MODE_WEIGHTED;
    }

    float clampOut(float v)
    {
        if (clampBlack && v < 0.0)
            return 0.0;
        else if (clampWhite && v > 1.0)
            return 1.0;
        return v;
    }

    float curveAt(uint curve, float t)
    {
        return curveSnapshotAt(CL_CURVE_BASE + curve * CS_CURVE_WORDS, wrapAux[CL_KEY_COUNT + curve],
                               asfloat(wrapAux[CL_Y_MIN + curve]), asfloat(wrapAux[CL_Y_MAX + curve]), t);
    }

    float curveValue(uint curve, float pos)
    {
        float v = curveAt(curve, pos);
        if (isStandard() && curve >= CL_RED && curve <= CL_BLUE)
            v += curveAt(CL_MASTER, pos) - pos;
        return v;
    }

    float lookup(uint curve, float value)
    {
        if (range.outside(value))
            return clampOut(curveValue(curve, value));
        // The weighted mode divides the blend's error by 1 - value, which the table's two-float
        // blend holds to the CPU's double rounding.
        return range.at(CL_LUT_BASE + curve * CS_TABLE_SIZE, value);
    }

    // A denormal a is compared by its bits, and the CPU's b + 0 * x / a is b while a is not zero
    // and b finite: a device that flushes the denormal a would divide by zero instead.
    float triangle(float a, float a1, float b)
    {
        if (curveIsNaN(a) || curveIsNaN(b) || curveOrderKey(a) != curveOrderKey(b))
        {
            const float a2 = a1 - a;
            const bool bFinite = (asuint(b) & 0x7fffffffu) < 0x7f800000u;
            if (curveLess(b, a))
            {
                if (a2 == 0.0 && bFinite && curveOrderKey(a) != 0)
                    return b;
                return b + a2 * b / a;
            }
            if (a2 == 0.0 && bFinite && a != 1.0)
                return b;
            return b + a2 * (1.0 - b) / (1.0 - a);
        }
        return a1;
    }

    float luminance(float r, float g, float b)
    {
        switch (luminanceMath)
        {
        case 0:
            return 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
        case 1:
            return 0.2627002119 * r + 0.6779980711 * g + 0.0593017165 * b;
        case 2:
            return 0.3439664498 * r + 0.7281660966 * g + -0.0721325464 * b;
        case 3:
            return 0.2722287168 * r + 0.6740817658 * g + 0.0536895174 * b;
        case 4:
            return 0.2989 * r + 0.5866 * g + 0.1145 * b;
        case 5:
            return (r + g + b) / 3.0;
        default:
            return max(max(r, g), b);
        }
    }

    // The master curve on the largest and smallest of the three, the middle one placed between.
    float3 rgbTone(float r, float g, float b)
    {
        const float rold = r;
        const float gold = g;
        const float bold = b;
        r = lookup(CL_MASTER, rold);
        b = lookup(CL_MASTER, bold);
        g = b + ((r - b) * (gold - bold) / (rold - bold));
        return float3(r, g, b);
    }

    float3 filmLike(float r, float g, float b)
    {
        const float rcoef = r < 1e-8 ? 1.0 : (lookup(CL_RED, r) / r);
        const float gcoef = g < 1e-8 ? 1.0 : (lookup(CL_GREEN, g) / g);
        const float bcoef = b < 1e-8 ? 1.0 : (lookup(CL_BLUE, b) / b);
        float3 t;
        if (r >= g)
        {
            if (g > b)
                t = rgbTone(r, g, b);
            else if (b > r)
            {
                const float3 u = rgbTone(b, r, g);
                t = float3(u.y, u.z, u.x);
            }
            else if (b > g)
            {
                const float3 u = rgbTone(r, b, g);
                t = float3(u.x, u.z, u.y);
            }
            else
            {
                const float m0 = lookup(CL_MASTER, r);
                const float m1 = lookup(CL_MASTER, g);
                t = float3(m0, m1, m1);
            }
        }
        else
        {
            if (r >= b)
            {
                const float3 u = rgbTone(g, r, b);
                t = float3(u.y, u.x, u.z);
            }
            else if (b > g)
            {
                const float3 u = rgbTone(b, g, r);
                t = float3(u.z, u.y, u.x);
            }
            else
            {
                const float3 u = rgbTone(g, b, r);
                t = float3(u.z, u.x, u.y);
            }
        }
        return float3(clampOut(rcoef * t.x), clampOut(gcoef * t.y), clampOut(bcoef * t.z));
    }

    float4 apply(float4 v, int2 xy)
    {
        const float r = v.x;
        const float g = v.y;
        const float b = v.z;
        float3 o;
        switch (mode)
        {
        case CL_MODE_STANDARD:
            o = float3(lookup(CL_RED, r), lookup(CL_GREEN, g), lookup(CL_BLUE, b));
            break;
        case CL_MODE_WEIGHTED:
        {
            const float r1 = lookup(CL_RED, r);
            const float g1 = triangle(r, r1, g);
            const float b1 = triangle(r, r1, b);

            const float g2 = lookup(CL_GREEN, g);
            const float r2 = triangle(g, g2, r);
            const float b2 = triangle(g, g2, b);

            const float b3 = lookup(CL_BLUE, b);
            const float r3 = triangle(b, b3, r);
            const float g3 = triangle(b, b3, g);

            o = float3(clampOut(r1 * 0.50 + r2 * 0.25 + r3 * 0.25), clampOut(g1 * 0.25 + g2 * 0.50 + g3 * 0.25),
                       clampOut(b1 * 0.25 + b2 * 0.25 + b3 * 0.50));
            break;
        }
        case CL_MODE_FILMLIKE:
            o = filmLike(r, g, b);
            break;
        default:
        {
            const float l = max(luminance(r, g, b), 1e-8);
            const float coef = lookup(CL_MASTER, l) / l;
            o = float3(clampOut(coef * lookup(CL_RED, r)), clampOut(coef * lookup(CL_GREEN, g)),
                       clampOut(coef * lookup(CL_BLUE, b)));
            break;
        }
        }
        return float4(o, lookup(CL_ALPHA, v.w));
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void colorlookup_wrapped(uint3 tid : SV_DispatchThreadID)
{
    ColorLookupOp op;
    runWrapped<ColorLookupOp>(op, tid);
}
