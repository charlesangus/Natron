// BlurKernels::LineFilter's recursive filters, one thread per line: Deriche, van Vliet and the box
// filter iterated 0 to 3 times, at orders 0 and 1, with the CPU's recursions and rounding points, and
// black or the end samples (Neumann) outside the line. The CPU keeps the recursion state in double;
// here it is kept in two floats, as plain float state drifts by more than 1e-3 for van Vliet at large
// sigma. GpuLineBlurParams in Engine/Gpu/GpuBlur.h mirrors LinesParams.

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;
// Whole lines, which the filters run in place along when they read their input more than once.
RWStructuredBuffer<float> lineBuf;
// The box filter's window of each line: 2 * half + 1 slots of lineCount pixels.
RWStructuredBuffer<float4> ringBuf;
// Coefficients as (hi, lo) float pairs, laid out by Engine/Gpu/GpuBlur.cpp.
StructuredBuffer<float> coefBuf;
// The rect (x1, y1, x2, y2) the CPU filters, which bounds every line.
StructuredBuffer<int> windowBuf;

// Positions run along the blur axis and lines across it, both in image pixels; li is the line's
// index, lineOrigin + li its position. A line spans the filtered rect along the axis, and writes the
// positions in [outLo, outHi) to dstBuf. The first pass of a filter reads the source image when
// fromSource is set (the horizontal pass), and lineBuf otherwise (the vertical pass, in place).
struct LinesParams
{
    uint kind;
    uint vertical;
    uint channels;
    uint lineCount;
    int lineOrigin;
    int outLo;
    int outHi;
    uint dstOffset;
    uint dstPosStep;
    uint dstLineStep;
    int storeLo;
    uint storePosStep;
    uint storeLineStep;
    uint fromSource;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    int half;
    uint iterations;
    uint order;
    uint neumann;
};
[[vk::push_constant]] ConstantBuffer<LinesParams> params;

// Mirrored in Engine/Gpu/GpuBlur.cpp.
static const uint kKindIdentity = 0;
static const uint kKindDeriche = 1;
static const uint kKindVanVliet = 2;
static const uint kKindBox = 3;

///////////////////////////////////////////////////////////////////////////////
// Two-float arithmetic per channel: a value is hi + lo with |lo| at most half an ulp of hi.

struct FF
{
    float hi;
    float lo;
};

struct FF4
{
    float4 hi;
    float4 lo;
};

FF4 ff4(float4 hi, float4 lo)
{
    FF4 r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

FF4 ff4(float4 v)
{
    return ff4(v, float4(0.0));
}

FF4 ffQuick(float4 a, float4 b)
{
    precise float4 s = a + b;
    precise float4 e = b - (s - a);
    return ff4(s, e);
}

FF4 ffTwoSum(float4 a, float4 b)
{
    precise float4 s = a + b;
    precise float4 v = s - a;
    precise float4 e = (a - (s - v)) + (b - v);
    return ff4(s, e);
}

FF4 ffAdd(FF4 a, FF4 b)
{
    const FF4 s = ffTwoSum(a.hi, b.hi);
    precise float4 lo = s.lo + (a.lo + b.lo);
    return ffQuick(s.hi, lo);
}

FF4 ffAddF(FF4 a, float4 b)
{
    const FF4 s = ffTwoSum(a.hi, b);
    precise float4 lo = s.lo + a.lo;
    return ffQuick(s.hi, lo);
}

FF4 ffNeg(FF4 a)
{
    return ff4(-a.hi, -a.lo);
}

FF4 ffSub(FF4 a, FF4 b)
{
    return ffAdd(a, ffNeg(b));
}

// Veltkamp's split: hi carries the top 12 bits of the significand, so products of halves are exact.
void ffSplit(float4 a, out float4 hi, out float4 lo)
{
    precise float4 t = a * 4097.0;
    precise float4 h = t - (t - a);
    precise float4 l = a - h;
    hi = h;
    lo = l;
}

// Dekker's exact product error, as Vulkan does not require fma() to round once (lavapipe does not).
float4 ffProductError(float4 a, float b, float4 p)
{
    float4 ah;
    float4 al;
    float4 bh;
    float4 bl;
    ffSplit(a, ah, al);
    ffSplit(float4(b), bh, bl);
    precise float4 e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return e;
}

FF4 ffMul(FF4 a, FF b)
{
    precise float4 p = a.hi * b.hi;
    const float4 e = ffProductError(a.hi, b.hi, p);
    precise float4 lo = e + (a.hi * b.lo + a.lo * b.hi);
    return ffQuick(p, lo);
}

// One correction step after the float quotient, whose rounding the device may not make exact.
FF4 ffDiv(FF4 a, FF b)
{
    precise float4 q1 = a.hi / b.hi;
    const FF4 r = ffSub(a, ffMul(ff4(q1), b));
    precise float4 q2 = (r.hi + r.lo) / b.hi;
    return ffQuick(q1, q2);
}

float4 ffFloat(FF4 a)
{
    precise float4 v = a.hi + a.lo;
    return v;
}

FF coef(uint i)
{
    FF r;
    r.hi = coefBuf[2 * i];
    r.lo = coefBuf[2 * i + 1];
    return r;
}

///////////////////////////////////////////////////////////////////////////////
// Line access. Channels beyond `channels` read as zero and are never written.

float4 loadAt(uint idx)
{
    float4 v = float4(0.0);
    [ForceUnroll]
    for (uint c = 0; c < 4; ++c)
    {
        if (c < params.channels)
            v[c] = lineBuf[idx + c];
    }
    return v;
}

uint lineIndex(int pos, uint li)
{
    return uint(pos - params.storeLo) * params.storePosStep + li * params.storeLineStep;
}

float4 loadLine(int pos, uint li)
{
    return loadAt(lineIndex(pos, li));
}

void storeLine(int pos, uint li, float4 v)
{
    const uint idx = lineIndex(pos, li);
    [ForceUnroll]
    for (uint c = 0; c < 4; ++c)
    {
        if (c < params.channels)
            lineBuf[idx + c] = v[c];
    }
}

uint outIndex(int pos, uint li)
{
    return params.dstOffset + uint(pos - params.outLo) * params.dstPosStep + li * params.dstLineStep;
}

bool isOut(int pos)
{
    return pos >= params.outLo && pos < params.outHi;
}

float4 loadOut(int pos, uint li)
{
    const uint idx = outIndex(pos, li);
    float4 v = float4(0.0);
    [ForceUnroll]
    for (uint c = 0; c < 4; ++c)
    {
        if (c < params.channels)
            v[c] = dstBuf[idx + c];
    }
    return v;
}

void storeOut(int pos, uint li, float4 v)
{
    const uint idx = outIndex(pos, li);
    [ForceUnroll]
    for (uint c = 0; c < 4; ++c)
    {
        if (c < params.channels)
            dstBuf[idx + c] = v[c];
    }
}

// The source pixel at (x, y), as the CPU's buffer fill reads its image: zero outside the source rect.
float4 sourceAt(int pos, int line)
{
    const int x = params.vertical != 0 ? line : pos;
    const int y = params.vertical != 0 ? pos : line;
    if (params.srcX1 >= params.srcX2 || params.srcY1 >= params.srcY2)
        return float4(0.0);
    if (x < params.srcX1 || x >= params.srcX2 || y < params.srcY1 || y >= params.srcY2)
        return float4(0.0);
    const uint idx = params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.channels;
    float4 v = float4(0.0);
    [ForceUnroll]
    for (uint c = 0; c < 4; ++c)
    {
        if (c < params.channels)
            v[c] = srcBuf[idx + c];
    }
    return v;
}

// The line before the filter's first pass changes it.
float4 inputAt(int pos, int line, uint li)
{
    return params.fromSource != 0 ? sourceAt(pos, line) : loadLine(pos, li);
}

///////////////////////////////////////////////////////////////////////////////
// The filters, over positions [lo, hi) of line li.

void identityLine(int lo, int hi, int line, uint li)
{
    for (int pos = max(lo, params.outLo); pos < min(hi, params.outHi); ++pos)
        storeOut(pos, li, inputAt(pos, line, li));
}

// LineFilter::applyDeriche. The causal sums go to the output positions and the anticausal pass
// adds to them, so neither pass needs the whole line stored.
void dericheLine(int lo, int hi, int line, uint li)
{
    const FF a0 = coef(0);
    const FF a1 = coef(1);
    const FF a2 = coef(2);
    const FF a3 = coef(3);
    const FF b1 = coef(4);
    const FF b2 = coef(5);

    FF4 yb = ff4(float4(0.0));
    FF4 yp = yb;
    float4 xp = float4(0.0);
    if (params.neumann != 0)
    {
        xp = inputAt(lo, line, li);
        yp = ffMul(ff4(xp), coef(6));
        yb = yp;
    }
    const int causalEnd = min(hi, params.outHi);
    for (int pos = lo; pos < causalEnd; ++pos)
    {
        const float4 xc = inputAt(pos, line, li);
        const FF4 yc = ffSub(ffSub(ffAdd(ffMul(ff4(xc), a0), ffMul(ff4(xp), a1)), ffMul(yp, b1)), ffMul(yb, b2));
        if (isOut(pos))
            storeOut(pos, li, ffFloat(yc));
        xp = xc;
        yb = yp;
        yp = yc;
    }

    float4 xn = float4(0.0);
    float4 xa = float4(0.0);
    FF4 yn = ff4(float4(0.0));
    FF4 ya = yn;
    if (params.neumann != 0)
    {
        xn = inputAt(hi - 1, line, li);
        xa = xn;
        yn = ffMul(ff4(xn), coef(7));
        ya = yn;
    }
    const int anticausalEnd = max(lo, params.outLo);
    for (int pos = hi - 1; pos >= anticausalEnd; --pos)
    {
        const float4 xc = inputAt(pos, line, li);
        const FF4 yc = ffSub(ffSub(ffAdd(ffMul(ff4(xn), a2), ffMul(ff4(xa), a3)), ffMul(yn, b1)), ffMul(ya, b2));
        xa = xn;
        xn = xc;
        ya = yn;
        yn = yc;
        if (isOut(pos))
            storeOut(pos, li, ffFloat(ffAddF(yc, loadOut(pos, li))));
    }
}

// LineFilter::applyVanVliet at order 0: the causal pass stores its outputs along the line, rounded
// to float as the CPU stores them in place, and the anticausal pass reads them back.
void vanVlietLine(int lo, int hi, int line, uint li)
{
    const FF sum = coef(1);
    const FF f1 = coef(2);
    const FF f2 = coef(3);
    const FF f3 = coef(4);
    const FF denom = coef(5);

    FF4 v1 = ff4(float4(0.0));
    FF4 v2 = v1;
    FF4 v3 = v1;
    // Read before the causal pass, which overwrites the line in place.
    float4 iplus = float4(0.0);
    if (params.neumann != 0)
    {
        iplus = inputAt(hi - 1, line, li);
        v1 = ffDiv(ff4(inputAt(lo, line, li)), coef(0));
        v2 = v1;
        v3 = v1;
    }
    for (int pos = lo; pos < hi; ++pos)
    {
        FF4 v0 = ff4(inputAt(pos, line, li));
        v0 = ffAdd(v0, ffMul(v1, f1));
        v0 = ffAdd(v0, ffMul(v2, f2));
        v0 = ffAdd(v0, ffMul(v3, f3));
        storeLine(pos, li, ffFloat(v0));
        v3 = v2;
        v2 = v1;
        v1 = v0;
    }

    const FF4 uplus = ffDiv(ff4(iplus), denom);
    const FF4 vplus = ffDiv(uplus, denom);
    const FF4 unp = ffSub(v1, uplus);
    const FF4 unp1 = ffSub(v2, uplus);
    const FF4 unp2 = ffSub(v3, uplus);
    FF4 w[3];
    [ForceUnroll]
    for (uint r = 0; r < 3; ++r)
    {
        const FF4 m = ffAdd(ffAdd(ffAdd(ffMul(unp, coef(6 + 3 * r)), ffMul(unp1, coef(7 + 3 * r))), ffMul(unp2, coef(8 + 3 * r))), vplus);
        w[r] = ffMul(m, sum);
    }
    if (isOut(hi - 1))
        storeOut(hi - 1, li, ffFloat(w[0]));
    v1 = w[0];
    v2 = w[1];
    v3 = w[2];
    const int anticausalEnd = max(lo, params.outLo);
    for (int pos = hi - 2; pos >= anticausalEnd; --pos)
    {
        FF4 v0 = ffMul(ff4(loadLine(pos, li)), sum);
        v0 = ffAdd(v0, ffMul(v1, f1));
        v0 = ffAdd(v0, ffMul(v2, f2));
        v0 = ffAdd(v0, ffMul(v3, f3));
        if (isOut(pos))
            storeOut(pos, li, ffFloat(v0));
        v3 = v2;
        v2 = v1;
        v1 = v0;
    }
}

// LineFilter::applyVanVliet at order 1. As on the CPU, the causal input at a position is half the
// difference of its neighbours, except that the first two positions take the boundary value in place
// of the previous sample; the first position's output is zero.
void vanVlietDerivativeLine(int lo, int hi, int line, uint li)
{
    const FF sum = coef(1);
    const FF f1 = coef(2);
    const FF f2 = coef(3);
    const FF f3 = coef(4);

    float4 x1 = float4(0.0);
    float4 x2 = float4(0.0);
    if (params.neumann != 0)
    {
        x1 = inputAt(lo, line, li);
        x2 = x1;
    }
    FF4 v1 = ff4(float4(0.0));
    FF4 v2 = v1;
    FF4 v3 = v1;
    for (int pos = lo; pos < hi - 1; ++pos)
    {
        const float4 x0 = inputAt(pos + 1, line, li);
        const FF4 d = ffTwoSum(x0, -x2);
        FF4 v0 = ff4(d.hi * 0.5, d.lo * 0.5);
        v0 = ffAdd(v0, ffMul(v1, f1));
        v0 = ffAdd(v0, ffMul(v2, f2));
        v0 = ffAdd(v0, ffMul(v3, f3));
        storeLine(pos, li, ffFloat(v0));
        x2 = x1;
        x1 = x0;
        v3 = v2;
        v2 = v1;
        v1 = v0;
    }

    FF4 w[3];
    [ForceUnroll]
    for (uint r = 0; r < 3; ++r)
    {
        const FF4 m = ffAdd(ffAdd(ffMul(v1, coef(6 + 3 * r)), ffMul(v2, coef(7 + 3 * r))), ffMul(v3, coef(8 + 3 * r)));
        w[r] = ffMul(m, sum);
    }
    if (isOut(hi - 1))
        storeOut(hi - 1, li, ffFloat(w[0]));
    v1 = w[0];
    v2 = w[1];
    v3 = w[2];
    const int anticausalEnd = max(lo + 1, params.outLo);
    for (int pos = hi - 2; pos >= anticausalEnd; --pos)
    {
        FF4 v0 = ffMul(ff4(loadLine(pos, li)), sum);
        v0 = ffAdd(v0, ffMul(v1, f1));
        v0 = ffAdd(v0, ffMul(v2, f2));
        v0 = ffAdd(v0, ffMul(v3, f3));
        if (isOut(pos))
            storeOut(pos, li, ffFloat(v0));
        v3 = v2;
        v2 = v1;
        v1 = v0;
    }
    if (hi - lo > 1 && isOut(lo))
        storeOut(lo, li, float4(0.0));
}

// boxSample() of the line as the pass reads it: positions outside the line read zero, or the end
// sample for a Neumann filter.
float4 boxSample(int rel, int lo, int n, bool first, int line, uint li)
{
    if (rel < 0 || rel >= n)
    {
        if (params.neumann == 0)
            return float4(0.0);
        rel = rel < 0 ? 0 : n - 1;
    }
    return first ? inputAt(lo + rel, line, li) : loadLine(lo + rel, li);
}

// centredDifference() at order 1, from the line the box passes left (or the input, after none) to
// the output positions. The difference is rounded to float before it is halved, as on the CPU.
void boxDifferenceLine(int lo, int hi, int line, uint li)
{
    const bool first = params.iterations == 0;
    const int n = hi - lo;
    const int start = max(0, params.outLo - lo);
    const int end = min(n, params.outHi - lo);
    for (int x = start; x < end; ++x)
    {
        precise float4 d = boxSample(x + 1, lo, n, first, line, li) - boxSample(x - 1, lo, n, first, line, li);
        precise float4 v = d * 0.5;
        storeOut(lo + x, li, v);
    }
}

// LineFilter::applyBox. Every pass but the last writes the line in place, the window keeping the
// samples it overwrites; the last writes the output positions at order 0, and the line at order 1,
// where the difference then writes the output positions.
void boxLine(int lo, int hi, int line, uint li)
{
    const bool derivative = params.order != 0;
    const FF frac = coef(0);
    const FF boxSize = coef(1);
    const int w2 = params.half;
    const uint winsize = uint(2 * w2 + 1);
    const int n = hi - lo;
    const uint lines = params.lineCount;

    for (uint iter = 0; iter < params.iterations; ++iter)
    {
        const bool first = iter == 0;
        const bool last = !derivative && iter + 1 == params.iterations;
        FF4 sum = ff4(float4(0.0));
        for (int x = -w2; x <= w2; ++x)
        {
            const float4 s = boxSample(x, lo, n, first, line, li);
            ringBuf[uint(x + w2) * lines + li] = s;
            sum = ffAddF(sum, s);
        }
        uint ifirst = 0;
        uint ilast = uint(2 * w2);
        float4 prev = boxSample(-w2 - 1, lo, n, first, line, li);
        float4 next = boxSample(w2 + 1, lo, n, first, line, li);
        const int end = last ? min(n, params.outHi - lo) : n;
        for (int x = 0; x < end; ++x)
        {
            precise float4 edges = prev + next;
            const float4 v = ffFloat(ffDiv(ffAdd(sum, ffMul(ff4(edges), frac)), boxSize));
            if (last)
            {
                if (isOut(lo + x))
                    storeOut(lo + x, li, v);
            }
            else
                storeLine(lo + x, li, v);
            if (x == n - 1)
                break;
            prev = ringBuf[ifirst * lines + li];
            sum = ffAddF(sum, -prev);
            ifirst = (ifirst + 1) % winsize;
            ilast = (ilast + 1) % winsize;
            ringBuf[ilast * lines + li] = next;
            sum = ffAddF(sum, next);
            next = boxSample(x + w2 + 2, lo, n, first, line, li);
        }
    }
    if (derivative)
        boxDifferenceLine(lo, hi, line, li);
}

// One thread per line. The vertical pass also writes zero to the output positions its line does
// not filter, as the CPU's buffer holds none there.
[shader("compute")]
[numthreads(64, 1, 1)]
void blur_lines(uint3 dtid : SV_DispatchThreadID)
{
    const uint li = dtid.x;
    if (li >= params.lineCount)
        return;
    const int line = params.lineOrigin + int(li);
    const bool vertical = params.vertical != 0;
    const int lo = vertical ? windowBuf[1] : windowBuf[0];
    const int hi = vertical ? windowBuf[3] : windowBuf[2];
    const int crossLo = vertical ? windowBuf[0] : windowBuf[1];
    const int crossHi = vertical ? windowBuf[2] : windowBuf[3];
    const bool filtered = lo < hi && line >= crossLo && line < crossHi;

    if (vertical)
    {
        for (int pos = params.outLo; pos < params.outHi; ++pos)
        {
            if (!filtered || pos < lo || pos >= hi)
                storeOut(pos, li, float4(0.0));
        }
    }
    if (!filtered)
        return;

    switch (params.kind)
    {
    case kKindDeriche:
        dericheLine(lo, hi, line, li);
        break;
    case kKindVanVliet:
        if (params.order != 0)
            vanVlietDerivativeLine(lo, hi, line, li);
        else
            vanVlietLine(lo, hi, line, li);
        break;
    case kKindBox:
        boxLine(lo, hi, line, li);
        break;
    default:
        identityLine(lo, hi, line, li);
        break;
    }
}
