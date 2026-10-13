import pixelwrap;

// Parameter block, in words. Every offset is mirrored by ColorLookupGpu in ColorLookup.cpp.
static const uint CL_CURVES = 5;
static const uint CL_LUT_SIZE = 1024;
static const uint CL_MAX_KEYS = 128;
static const uint CL_SEGMENT_WORDS = 6;
// Per curve: the key time thresholds, then one cubic segment per gap between keys, with a
// segment before the first key and after the last.
static const uint CL_CURVE_WORDS = CL_MAX_KEYS + (CL_MAX_KEYS + 1) * CL_SEGMENT_WORDS;
static const uint CL_HEADER_WORDS = 32;
static const uint CL_Y_MIN = 16;
static const uint CL_Y_MAX = 21;
static const uint CL_KEY_COUNT = 8;
static const uint CL_SPLIT_RANGE = 26;
static const uint CL_LUT_BASE = CL_HEADER_WORDS;
static const uint CL_CURVE_BASE = CL_LUT_BASE + CL_CURVES * CL_LUT_SIZE;

static const uint CL_MASTER = 0;
static const uint CL_RED = 1;
static const uint CL_GREEN = 2;
static const uint CL_BLUE = 3;
static const uint CL_ALPHA = 4;

static const uint CL_MODE_STANDARD = 0;
static const uint CL_MODE_WEIGHTED = 1;
static const uint CL_MODE_FILMLIKE = 2;
static const uint CL_MODE_LUMINANCE = 3;

// Floats ordered as integers: devices may flush denormals to zero in a float comparison, while
// the CPU orders a denormal below or above zero. -0 and +0 share key 0.
int clOrderKey(float f)
{
    const uint u = asuint(f);
    return (u & 0x80000000u) != 0 ? -int(u & 0x7fffffffu) : int(u);
}

bool clIsNaN(float f)
{
    return (asuint(f) & 0x7fffffffu) > 0x7f800000u;
}

// a < b as IEEE orders them, false when either is a NaN.
bool clLess(float a, float b)
{
    return !clIsNaN(a) && !clIsNaN(b) && clOrderKey(a) < clOrderKey(b);
}

// The tables are read from the bound parameter buffer, wrapAux, since an op cannot hold a buffer.
struct ColorLookupOp : IPixelOp
{
    uint mode;
    uint luminanceMath;
    bool clampBlack;
    bool clampWhite;
    // A float v is outside the table range exactly when v < lowOut or v > highOut; the bounds are
    // the range rounded so that this agrees with the double comparison on the CPU.
    float lowOut;
    float highOut;
    // The range start and kLutIntervals / span, each as a float and its remainder.
    float rangeMinHi;
    float rangeMinLo;
    float scaleHi;
    float scaleLo;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        mode = params[0];
        luminanceMath = params[1];
        clampBlack = (params[2] & 1) != 0;
        clampWhite = (params[2] & 2) != 0;
        lowOut = wrapParamFloat(params, 4);
        highOut = wrapParamFloat(params, 5);
        rangeMinHi = wrapParamFloat(params, CL_SPLIT_RANGE);
        rangeMinLo = wrapParamFloat(params, CL_SPLIT_RANGE + 1);
        scaleHi = wrapParamFloat(params, CL_SPLIT_RANGE + 2);
        scaleLo = wrapParamFloat(params, CL_SPLIT_RANGE + 3);
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

    // The curve at t, as CurveSnapshot::valueAt: the segment after the last key not above t, a
    // Hermite cubic, clamped to the curve's value range.
    float curveAt(uint curve, float t)
    {
        const uint base = CL_CURVE_BASE + curve * CL_CURVE_WORDS;
        const uint count = wrapAux[CL_KEY_COUNT + curve];
        uint lo = 0;
        uint hi = count;
        while (lo < hi)
        {
            const uint mid = (lo + hi) >> 1;
            if (t < asfloat(wrapAux[base + mid]))
                hi = mid;
            else
                lo = mid + 1;
        }
        const uint s = base + CL_MAX_KEYS + lo * CL_SEGMENT_WORDS;
        const float u = (t - asfloat(wrapAux[s])) / asfloat(wrapAux[s + 1]);
        const float c1 = asfloat(wrapAux[s + 3]);
        const float c2 = asfloat(wrapAux[s + 4]);
        const float c3 = asfloat(wrapAux[s + 5]);
        const float u2 = u * u;
        const float u3 = u2 * u;
        float v = asfloat(wrapAux[s + 2]) + (c1 != 0.0 ? c1 * u : 0.0) + (c2 != 0.0 ? c2 * u2 : 0.0) +
                  (c3 != 0.0 ? c3 * u3 : 0.0);
        const float yMax = asfloat(wrapAux[CL_Y_MAX + curve]);
        const float yMin = asfloat(wrapAux[CL_Y_MIN + curve]);
        if (v > yMax)
            return yMax;
        else if (v < yMin)
            return yMin;
        return v;
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
        if (clLess(value, lowOut) || clLess(highOut, value))
            return clampOut(curveValue(curve, value));
        // The CPU finds the table position and blends in double, and the weighted mode divides the
        // blend's error by 1 - value: the position and the blend are carried as two floats each, so
        // that the result rounds as the double one does.
        const uint lut = CL_LUT_BASE + curve * CL_LUT_SIZE;
        precise float dh = value - rangeMinHi;
        precise float dv = dh - value;
        precise float dl = ((value - (dh - dv)) + (-rangeMinHi - dv)) - rangeMinLo;
        precise float d = dh + dl;
        precise float dRest = dl - (d - dh);
        precise float ph = d * scaleHi;
        precise float pl = fma(d, scaleHi, -ph) + (d * scaleLo + dRest * scaleHi);
        precise float sh = ph + pl;
        precise float sl = pl - (sh - ph);
        const float last = float(CL_LUT_SIZE - 1);
        if (isnan(sh) || sh < 0.0 || (sh == 0.0 && !(sl > 0.0)))
            return asfloat(wrapAux[lut]);
        else if (sh > last || (sh == last && !(sl < 0.0)))
            return asfloat(wrapAux[lut + CL_LUT_SIZE - 1]);
        int i = int(sh);
        if (float(i) == sh && sl < 0.0)
            i -= 1;
        i = clamp(i, 0, int(CL_LUT_SIZE) - 2);
        precise float ah = sh - float(i);
        precise float al = sl;
        if (ah + al <= 0.0)
        {
            ah = 0.0;
            al = 0.0;
        }
        else if (ah + al >= 1.0)
        {
            ah = 1.0;
            al = 0.0;
        }
        const float a = asfloat(wrapAux[lut + uint(i)]);
        const float b = asfloat(wrapAux[lut + uint(i) + 1]);
        precise float eh = b - a;
        precise float ev = eh - b;
        precise float el = (b - (eh - ev)) + (-a - ev);
        precise float qh = ah * eh;
        precise float ql = fma(ah, eh, -qh) + (ah * el + al * eh);
        precise float r = a + qh;
        precise float rv = r - a;
        precise float rErr = (a - (r - rv)) + (qh - rv);
        return r + (rErr + ql);
    }

    // A denormal a is compared by its bits, and the CPU's b + 0 * x / a is b while a is not zero
    // and b finite: a device that flushes the denormal a would divide by zero instead.
    float triangle(float a, float a1, float b)
    {
        if (clIsNaN(a) || clIsNaN(b) || clOrderKey(a) != clOrderKey(b))
        {
            const float a2 = a1 - a;
            const bool bFinite = (asuint(b) & 0x7fffffffu) < 0x7f800000u;
            if (clLess(b, a))
            {
                if (a2 == 0.0 && bFinite && clOrderKey(a) != 0)
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
