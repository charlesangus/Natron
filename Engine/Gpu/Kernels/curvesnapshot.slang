import pixelwrap;

// A curve as CurveSnapshot::packGpu() lays it out in the op's parameter block, in words: the key
// time thresholds, then one cubic segment per gap between keys, with a segment before the first
// key and after the last.
static const uint CS_MAX_KEYS = 128;
static const uint CS_SEGMENT_WORDS = 12;
static const uint CS_CURVE_WORDS = CS_MAX_KEYS + (CS_MAX_KEYS + 1) * CS_SEGMENT_WORDS;
// Samples of a table taken from a curve over a range.
static const uint CS_TABLE_SIZE = 1024;

// Floats ordered as integers: devices may flush denormals to zero in a float comparison, while
// the CPU orders a denormal below or above zero. -0 and +0 share key 0.
int curveOrderKey(float f)
{
    const uint u = asuint(f);
    return (u & 0x80000000u) != 0 ? -int(u & 0x7fffffffu) : int(u);
}

bool curveIsNaN(float f)
{
    return (asuint(f) & 0x7fffffffu) > 0x7f800000u;
}

// a < b as IEEE orders them, false when either is a NaN.
bool curveLess(float a, float b)
{
    return !curveIsNaN(a) && !curveIsNaN(b) && curveOrderKey(a) < curveOrderKey(b);
}

// The value hi + lo, lo within half an ulp of hi.
struct CurveFF
{
    float hi;
    float lo;
}

CurveFF curveFF(float hi, float lo)
{
    CurveFF r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

CurveFF curveQuickSum(float a, float b)
{
    precise float s = a + b;
    precise float e = b - (s - a);
    return curveFF(s, e);
}

CurveFF curveTwoSum(float a, float b)
{
    precise float s = a + b;
    precise float v = s - a;
    precise float e = (a - (s - v)) + (b - v);
    return curveFF(s, e);
}

CurveFF curveAdd(CurveFF a, CurveFF b)
{
    const CurveFF s = curveTwoSum(a.hi, b.hi);
    precise float lo = s.lo + (a.lo + b.lo);
    return curveQuickSum(s.hi, lo);
}

// Veltkamp's split: hi carries the top 12 bits of the significand, so products of halves are
// exact. The split is not finite once |a| passes about 8.3e34, where a * 4097 overflows.
void curveSplit(float a, out float hi, out float lo)
{
    precise float t = a * 4097.0;
    precise float h = t - (t - a);
    hi = h;
    lo = a - h;
}

// The error of the float product p of a and b, by Dekker's split: fma() need not round once on
// every device (lavapipe does not), and a fused error term is then wrong or lost. An operand too
// large to split gets 0 rather than a NaN beside a finite p, and so does a product under 1e-30,
// whose error is denormal and which a device may flush on one side of the split but not the other.
float curveProductError(float a, float b, float p)
{
    if (!(abs(p) >= 1e-30))
        return 0.0;
    float ah;
    float al;
    float bh;
    float bl;
    curveSplit(a, ah, al);
    curveSplit(b, bh, bl);
    precise float e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return isfinite(e) ? e : 0.0;
}

CurveFF curveMul(CurveFF a, CurveFF b)
{
    precise float p = a.hi * b.hi;
    precise float e = curveProductError(a.hi, b.hi, p);
    precise float lo = e + (a.hi * b.lo + a.lo * b.hi);
    return curveQuickSum(p, lo);
}

CurveFF curvePair(uint index)
{
    return curveFF(asfloat(wrapAux[index]), asfloat(wrapAux[index + 1]));
}

// The curve at t, as CurveSnapshot::valueAt: the segment after the last key not above t, a
// Hermite cubic, clamped to the curve's value range. The curve is read from wrapAux at `base`.
// A key time lies between floats and a slope may be steep, so t - origin and the cubic are
// carried in two floats as the CPU's double carries them.
float curveSnapshotAt(uint base, uint count, float yMin, float yMax, float t)
{
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
    const uint s = base + CS_MAX_KEYS + lo * CS_SEGMENT_WORDS;
    const CurveFF origin = curvePair(s);
    const CurveFF invSpan = curvePair(s + 2);
    const CurveFF c0 = curvePair(s + 4);
    const CurveFF c1 = curvePair(s + 6);
    const CurveFF c2 = curvePair(s + 8);
    const CurveFF c3 = curvePair(s + 10);
    const CurveFF dt = curveAdd(curveTwoSum(t, -origin.hi), curveFF(-origin.lo, 0.0));
    const CurveFF u = curveMul(dt, invSpan);
    CurveFF r = curveAdd(curveMul(c3, u), c2);
    r = curveAdd(curveMul(r, u), c1);
    r = curveAdd(curveMul(r, u), c0);
    float v = r.hi + r.lo;
    if (!isfinite(r.hi))
    {
        // An infinite t, or a cubic past the float range: the CPU's terms one by one, a zero
        // coefficient contributing nothing rather than a NaN from 0 times infinity.
        const float uf = (t - origin.hi - origin.lo) * invSpan.hi;
        const float u2 = uf * uf;
        const float u3 = u2 * uf;
        v = c0.hi + (c1.hi != 0.0 ? c1.hi * uf : 0.0) + (c2.hi != 0.0 ? c2.hi * u2 : 0.0) + (c3.hi != 0.0 ? c3.hi * u3 : 0.0);
    }
    if (v > yMax)
        return yMax;
    else if (v < yMin)
        return yMin;
    return v;
}

// The range of a table of CS_TABLE_SIZE samples, as CurveSnapshot::packGpuTableRange() writes it.
struct CurveTableRange
{
    // A float v is outside the table range exactly when v < lowOut or v > highOut; the bounds are
    // the range rounded so that this agrees with the double comparison on the CPU.
    float lowOut;
    float highOut;
    // The range start and (CS_TABLE_SIZE - 1) / span, each as a float and its remainder.
    float rangeMinHi;
    float rangeMinLo;
    float scaleHi;
    float scaleLo;

    [mutating] void load(StructuredBuffer<uint> params, uint index)
    {
        lowOut = wrapParamFloat(params, index);
        highOut = wrapParamFloat(params, index + 1);
        rangeMinHi = wrapParamFloat(params, index + 2);
        rangeMinLo = wrapParamFloat(params, index + 3);
        scaleHi = wrapParamFloat(params, index + 4);
        scaleLo = wrapParamFloat(params, index + 5);
    }

    bool outside(float value)
    {
        return curveLess(value, lowOut) || curveLess(highOut, value);
    }

    // The table at `table` in wrapAux, interpolated at a value inside the range. The CPU finds the
    // table position and blends in double, and a caller may divide the blend's error by a small
    // quantity: the position and the blend are carried as two floats each, so that the result
    // rounds as the double one does.
    float at(uint table, float value)
    {
        precise float dh = value - rangeMinHi;
        precise float dv = dh - value;
        precise float dl = ((value - (dh - dv)) + (-rangeMinHi - dv)) - rangeMinLo;
        precise float d = dh + dl;
        precise float dRest = dl - (d - dh);
        precise float ph = d * scaleHi;
        precise float pl = curveProductError(d, scaleHi, ph) + (d * scaleLo + dRest * scaleHi);
        precise float sh = ph + pl;
        precise float sl = pl - (sh - ph);
        const float last = float(CS_TABLE_SIZE - 1);
        if (isnan(sh) || sh < 0.0 || (sh == 0.0 && !(sl > 0.0)))
            return asfloat(wrapAux[table]);
        else if (sh > last || (sh == last && !(sl < 0.0)))
            return asfloat(wrapAux[table + CS_TABLE_SIZE - 1]);
        int i = int(sh);
        if (float(i) == sh && sl < 0.0)
            i -= 1;
        i = clamp(i, 0, int(CS_TABLE_SIZE) - 2);
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
        const float a = asfloat(wrapAux[table + uint(i)]);
        const float b = asfloat(wrapAux[table + uint(i) + 1]);
        precise float eh = b - a;
        precise float ev = eh - b;
        precise float el = (b - (eh - ev)) + (-a - ev);
        precise float qh = ah * eh;
        precise float ql = curveProductError(ah, eh, qh) + (ah * el + al * eh);
        precise float r = a + qh;
        precise float rv = r - a;
        precise float rErr = (a - (r - rv)) + (qh - rv);
        return r + (rErr + ql);
    }
}
