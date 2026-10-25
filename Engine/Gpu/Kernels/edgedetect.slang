// EdgeDetect's own passes, after the workers of Engine/Nodes/Filter/EdgeDetect.cpp: the source fill,
// the finite-difference stencil, the normalisation and channel combination, and non-maxima
// suppression. The scratch buffers cover the CPU's filter buffer, `channels` floats per pixel,
// rows of bufWidth pixels from (bufX1, bufY1). The CPU combines and suppresses in double; here
// those steps run in two floats, since a float rounding flips the suppression's comparisons on near
// ties. EdgeDetectGpuParams in Engine/Nodes/Filter/EdgeDetect.cpp mirrors EdgeParams.

// fill: the source image; stencil: the pre-blurred buffer; suppression: the edge magnitude.
StructuredBuffer<uint> inBuf;
RWStructuredBuffer<float> gxBuf;
RWStructuredBuffer<float> gyBuf;
// fill: the buffer; combination and suppression: the rect of outWidth x outHeight pixels at
// (outX1, outY1), whose first pixel is at dstOffset.
RWStructuredBuffer<uint> outBuf;

struct EdgeParams
{
    int bufX1;
    int bufY1;
    uint bufWidth;
    uint bufHeight;
    uint channels;
    // The processed components, one bit each.
    uint processed;
    // stencil: the scheme; combination: the multi-channel mode; suppression: 1 when the channels
    // are suppressed separately.
    uint mode;
    // combination: kNormalizeX and kNormalizeY; suppression: the component whose direction is
    // followed when the channels are not separate.
    uint flags;
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint dstOffset;
    uint dstStride;
    // fill: the source pixels that exist, the first at srcOffset; empty when there are none.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    // The double normalisation factors as (hi, lo) float pairs.
    float normXHi;
    float normXLo;
    float normYHi;
    float normYLo;
    // The rotation-invariant kernel's weights, as the CPU rounds them.
    float ra;
    float rb;
};
[[vk::push_constant]] ConstantBuffer<EdgeParams> params;

// Mirrored in Engine/Nodes/Filter/EdgeDetect.cpp.
static const uint kSchemeSimple = 0;
static const uint kSchemeSobel = 1;
static const uint kSchemeRotationInvariant = 2;
static const uint kCombineSeparate = 0;
static const uint kCombineRMS = 1;
static const uint kCombineMax = 2;
static const uint kCombineTensor = 3;
static const uint kNormalizeX = 1;
static const uint kNormalizeY = 2;

///////////////////////////////////////////////////////////////////////////////
// Two-float arithmetic: a value is hi + lo with |lo| at most half an ulp of hi.

struct FF
{
    float hi;
    float lo;
};

FF ff(float hi, float lo)
{
    FF r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

FF ff(float v)
{
    return ff(v, 0.0);
}

FF ffQuick(float a, float b)
{
    precise float s = a + b;
    precise float e = b - (s - a);
    return ff(s, e);
}

FF ffTwoSum(float a, float b)
{
    precise float s = a + b;
    precise float v = s - a;
    precise float e = (a - (s - v)) + (b - v);
    return ff(s, e);
}

FF ffAdd(FF a, FF b)
{
    const FF s = ffTwoSum(a.hi, b.hi);
    precise float lo = s.lo + (a.lo + b.lo);
    return ffQuick(s.hi, lo);
}

FF ffNeg(FF a)
{
    return ff(-a.hi, -a.lo);
}

FF ffSub(FF a, FF b)
{
    return ffAdd(a, ffNeg(b));
}

// Exact for a power of two.
FF ffScale(FF a, float s)
{
    precise float hi = a.hi * s;
    precise float lo = a.lo * s;
    return ff(hi, lo);
}

// Veltkamp's split: hi carries the top 12 bits of the significand, so products of halves are exact.
void ffSplit(float a, out float hi, out float lo)
{
    precise float t = a * 4097.0;
    precise float h = t - (t - a);
    precise float l = a - h;
    hi = h;
    lo = l;
}

// Dekker's exact product, as Vulkan does not require fma() to round once.
FF ffProduct(float a, float b)
{
    precise float p = a * b;
    float ah;
    float al;
    float bh;
    float bl;
    ffSplit(a, ah, al);
    ffSplit(b, bh, bl);
    precise float e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return ff(p, e);
}

FF ffMul(FF a, FF b)
{
    const FF p = ffProduct(a.hi, b.hi);
    precise float lo = p.lo + (a.hi * b.lo + a.lo * b.hi);
    return ffQuick(p.hi, lo);
}

// One correction step after the float quotient, whose rounding the device may not make exact.
FF ffDiv(FF a, FF b)
{
    precise float q1 = a.hi / b.hi;
    const FF r = ffSub(a, ffMul(ff(q1), b));
    precise float q2 = (r.hi + r.lo) / b.hi;
    return ffQuick(q1, q2);
}

// The square root of max(a, 0): one Newton step from the device's float root, which need not be
// correctly rounded.
FF ffSqrt(FF a)
{
    if (!(a.hi > 0.0))
        return ff(0.0);
    const float s = sqrt(a.hi);
    const FF r = ffSub(a, ffProduct(s, s));
    precise float c = (r.hi + r.lo) / (2.0 * s);
    return ffQuick(s, c);
}

float ffFloat(FF a)
{
    precise float v = a.hi + a.lo;
    return v;
}

bool ffLess(FF a, FF b)
{
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

bool ffGreaterEqual(FF a, FF b)
{
    return a.hi > b.hi || (a.hi == b.hi && a.lo >= b.lo);
}

// float(v * norm) for the double norm (hi, lo), as the CPU rounds the double product.
float scaled(float v, float hi, float lo)
{
    const FF p = ffProduct(v, hi);
    precise float l = p.lo + v * lo;
    return ffFloat(ffQuick(p.hi, l));
}

///////////////////////////////////////////////////////////////////////////////
// Addressing.

bool inBuffer(int x, int y)
{
    return x >= params.bufX1 && y >= params.bufY1 && x < params.bufX1 + int(params.bufWidth)
           && y < params.bufY1 + int(params.bufHeight);
}

uint bufIndex(int x, int y)
{
    return (uint(y - params.bufY1) * params.bufWidth + uint(x - params.bufX1)) * params.channels;
}

uint dstIndex(uint2 tid)
{
    return params.dstOffset + tid.y * params.dstStride + tid.x * params.channels;
}

bool isProcessed(uint c)
{
    return (params.processed & (1u << c)) != 0;
}

void writeZero(uint dst)
{
    for (uint c = 0; c < params.channels && c < 4; ++c)
        outBuf[dst + c] = 0u;
}

///////////////////////////////////////////////////////////////////////////////

// fillBuffer() with the nearest boundary: each buffer pixel is the source pixel nearest to it, or
// zero when the source has none.
[shader("compute")]
[numthreads(8, 8, 1)]
void edgedetect_fill(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.bufWidth || tid.y >= params.bufHeight)
        return;
    const uint n = params.channels;
    const uint dst = (tid.y * params.bufWidth + tid.x) * n;
    if (params.srcX1 >= params.srcX2 || params.srcY1 >= params.srcY2)
    {
        for (uint c = 0; c < n && c < 4; ++c)
            outBuf[dst + c] = 0u;
        return;
    }
    const int cx = clamp(params.bufX1 + int(tid.x), params.srcX1, params.srcX2 - 1);
    const int cy = clamp(params.bufY1 + int(tid.y), params.srcY1, params.srcY2 - 1);
    const uint src = params.srcOffset + uint(cy - params.srcY1) * params.srcStride + uint(cx - params.srcX1) * n;
    for (uint c = 0; c < n && c < 4; ++c)
        outBuf[dst + c] = inBuf[src + c];
}

float blurredAt(int i, int j, uint c)
{
    return asfloat(inBuf[(uint(j) * params.bufWidth + uint(i)) * params.channels + c]);
}

// stencilWorker(): the gradient components of the pre-blurred buffer, with its edges repeated.
// Dividing by 8 or 2 is multiplying by 0.125 or 0.5, which is exact where the device's division is not.
[shader("compute")]
[numthreads(8, 8, 1)]
void edgedetect_stencil(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.bufWidth || tid.y >= params.bufHeight)
        return;
    const int w = int(params.bufWidth);
    const int h = int(params.bufHeight);
    const int i = int(tid.x);
    const int row = int(tid.y);
    const int rowP = max(0, row - 1);
    const int rowN = min(h - 1, row + 1);
    const int iP = max(0, i - 1);
    const int iN = min(w - 1, i + 1);
    const float ra = params.ra;
    const float rb = params.rb;
    const uint base = (uint(row) * params.bufWidth + uint(i)) * params.channels;
    for (uint c = 0; c < params.channels && c < 4; ++c)
    {
        const float Ipp = blurredAt(iP, rowP, c);
        const float Icp = blurredAt(i, rowP, c);
        const float Inp = blurredAt(iN, rowP, c);
        const float Ipc = blurredAt(iP, row, c);
        const float Inc = blurredAt(iN, row, c);
        const float Ipn = blurredAt(iP, rowN, c);
        const float Icn = blurredAt(i, rowN, c);
        const float Inn = blurredAt(iN, rowN, c);
        float vx;
        float vy;
        if (params.mode == kSchemeSobel)
        {
            precise float sx = -Ipp + Inp - 2.0 * Ipc + 2.0 * Inc - Ipn + Inn;
            precise float sy = -Ipp - 2.0 * Icp - Inp + Ipn + 2.0 * Icn + Inn;
            precise float qx = sx * 0.125;
            precise float qy = sy * 0.125;
            vx = qx;
            vy = qy;
        }
        else if (params.mode == kSchemeRotationInvariant)
        {
            precise float sx = -ra * Ipp - rb * Ipc - ra * Ipn + ra * Inp + rb * Inc + ra * Inn;
            precise float sy = -ra * Ipp - rb * Icp - ra * Inp + ra * Ipn + rb * Icn + ra * Inn;
            vx = sx;
            vy = sy;
        }
        else
        {
            precise float sx = (Inc - Ipc) * 0.5;
            precise float sy = (Icn - Icp) * 0.5;
            vx = sx;
            vy = sy;
        }
        gxBuf[base + c] = w > 1 ? vx : 0.0;
        gyBuf[base + c] = h > 1 ? vy : 0.0;
    }
}

FF squaredNorm(float gx, float gy)
{
    return ffAdd(ffProduct(gx, gx), ffProduct(gy, gy));
}

// combineWorker(): the components normalised in place, the edge magnitude written to the output
// rect, and for the combined modes the direction left in every component. Pixels outside the buffer
// are zero, as the CPU's write reads them.
[shader("compute")]
[numthreads(8, 8, 1)]
void edgedetect_combine(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint dst = dstIndex(tid.xy);
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    if (!inBuffer(x, y))
    {
        writeZero(dst);
        return;
    }
    const uint k = bufIndex(x, y);
    const uint n = min(params.channels, 4u);
    float gx[4];
    float gy[4];
    for (uint c = 0; c < n; ++c)
    {
        gx[c] = gxBuf[k + c];
        gy[c] = gyBuf[k + c];
        if ((params.flags & kNormalizeX) != 0)
            gx[c] = scaled(gx[c], params.normXHi, params.normXLo);
        if ((params.flags & kNormalizeY) != 0)
            gy[c] = scaled(gy[c], params.normYHi, params.normYLo);
    }

    if (params.mode == kCombineSeparate)
    {
        for (uint c = 0; c < n; ++c)
        {
            gxBuf[k + c] = gx[c];
            gyBuf[k + c] = gy[c];
            outBuf[dst + c] = asuint(ffFloat(ffSqrt(squaredNorm(gx[c], gy[c]))));
        }
        return;
    }

    float e = 0.0;
    float dirX = 0.0;
    float dirY = 0.0;
    if (params.mode == kCombineRMS)
    {
        // The direction is the sum of the channel gradients, each flipped to point the same way
        // as the strongest channel's.
        FF sumsq = ff(0.0);
        FF refsq = ff(-1.0);
        float refx = 0.0;
        float refy = 0.0;
        uint count = 0;
        for (uint c = 0; c < n; ++c)
        {
            if (!isProcessed(c))
                continue;
            const FF sq = squaredNorm(gx[c], gy[c]);
            sumsq = ffAdd(sumsq, sq);
            ++count;
            if (ffLess(refsq, sq))
            {
                refsq = sq;
                refx = gx[c];
                refy = gy[c];
            }
        }
        FF gradx = ff(0.0);
        FF grady = ff(0.0);
        for (uint c = 0; c < n; ++c)
        {
            if (!isProcessed(c))
                continue;
            const FF dot = ffAdd(ffProduct(gx[c], refx), ffProduct(gy[c], refy));
            const bool flip = dot.hi < 0.0 || (dot.hi == 0.0 && dot.lo < 0.0);
            gradx = ffAdd(gradx, ff(flip ? -gx[c] : gx[c]));
            grady = ffAdd(grady, ff(flip ? -gy[c] : gy[c]));
        }
        e = ffFloat(ffSqrt(ffDiv(sumsq, ff(float(count)))));
        dirX = ffFloat(gradx);
        dirY = ffFloat(grady);
    }
    else if (params.mode == kCombineMax)
    {
        FF maxsq = ff(0.0);
        for (uint c = 0; c < n; ++c)
        {
            if (!isProcessed(c))
                continue;
            const FF sq = squaredNorm(gx[c], gy[c]);
            if (ffLess(maxsq, sq))
            {
                maxsq = sq;
                dirX = gx[c];
                dirY = gy[c];
            }
        }
        e = ffFloat(ffSqrt(maxsq));
    }
    else
    {
        // Di Zenzo: the square root of the largest eigenvalue of the structure tensor, whose
        // eigenvector (Jxy, e1 - Jx) is the direction.
        FF Jx = ff(0.0);
        FF Jy = ff(0.0);
        FF Jxy = ff(0.0);
        for (uint c = 0; c < n; ++c)
        {
            if (!isProcessed(c))
                continue;
            Jx = ffAdd(Jx, ffProduct(gx[c], gx[c]));
            Jy = ffAdd(Jy, ffProduct(gy[c], gy[c]));
            Jxy = ffAdd(Jxy, ffProduct(gx[c], gy[c]));
        }
        const FF sqD = ffAdd(ffAdd(ffSub(ffMul(Jx, Jx), ffMul(ffScale(Jx, 2.0), Jy)), ffMul(Jy, Jy)),
                             ffMul(ffScale(Jxy, 4.0), Jxy));
        FF D = ffSqrt(sqD);
        // With Jy and Jxy zero the CPU's sqD is Jx * Jx rounded, whose double root is exactly Jx,
        // so its direction is exactly zero; a root one rounding off would leave a direction that
        // the suppression then follows.
        if (Jy.hi == 0.0 && Jxy.hi == 0.0)
            D = Jx;
        const FF e1 = ffScale(ffAdd(ffAdd(Jx, Jy), D), 0.5);
        e = ffFloat(ffSqrt(e1));
        dirX = ffFloat(Jxy);
        dirY = ffFloat(ffSub(e1, Jx));
    }
    for (uint c = 0; c < n; ++c)
    {
        outBuf[dst + c] = asuint(e);
        gxBuf[k + c] = dirX;
        gyBuf[k + c] = dirY;
    }
}

float magnitudeAt(int x, int y, uint c)
{
    return asfloat(inBuf[bufIndex(x, y) + c]);
}

// parabola(): the parabola through (-1, Ip), (0, Ic), (1, In), at alpha.
FF parabola(float Ip, float Ic, float In, FF alpha)
{
    const FF a = ffScale(ffAdd(ffTwoSum(In, -Ic), ffTwoSum(Ip, -Ic)), 0.5);
    const FF b = ffScale(ffTwoSum(In, -Ip), 0.5);
    return ffAdd(ffAdd(ffMul(ffMul(a, alpha), alpha), ffMul(b, alpha)), ff(Ic));
}

// parabolaMaxValue(): that parabola's extremum.
FF parabolaMax(FF Ip, float Ic, FF In)
{
    const FF c = ff(Ic);
    const FF a = ffScale(ffAdd(ffSub(In, c), ffSub(Ip, c)), 0.5);
    const FF b = ffScale(ffSub(In, Ip), 0.5);
    return ffSub(c, ffDiv(ffMul(b, b), ffScale(a, 4.0)));
}

float suppressed(int x, int y, uint c, uint dirC)
{
    const uint k = bufIndex(x, y);
    float gradx = gxBuf[k + dirC];
    float grady = gyBuf[k + dirC];
    if (gradx == 0.0 && grady == 0.0)
        return 0.0;
    const bool horiz = abs(gradx) >= abs(grady);
    float Ipp;
    float Ipc;
    float Ipn;
    float Inp;
    float Inc;
    float Inn;
    if (horiz)
    {
        Ipp = magnitudeAt(x - 1, y - 1, c);
        Ipc = magnitudeAt(x - 1, y, c);
        Ipn = magnitudeAt(x - 1, y + 1, c);
        Inp = magnitudeAt(x + 1, y - 1, c);
        Inc = magnitudeAt(x + 1, y, c);
        Inn = magnitudeAt(x + 1, y + 1, c);
    }
    else
    {
        const float t = gradx;
        gradx = grady;
        grady = t;
        Ipp = magnitudeAt(x - 1, y - 1, c);
        Ipc = magnitudeAt(x, y - 1, c);
        Ipn = magnitudeAt(x + 1, y - 1, c);
        Inp = magnitudeAt(x - 1, y + 1, c);
        Inc = magnitudeAt(x, y + 1, c);
        Inn = magnitudeAt(x + 1, y + 1, c);
    }
    const FF alpha = ffDiv(ff(grady), ff(gradx));
    const FF Ip = parabola(Ipp, Ipc, Ipn, ffNeg(alpha));
    const FF In = parabola(Inp, Inc, Inn, alpha);
    const float Icv = magnitudeAt(x, y, c);
    if (ffLess(Ip, ff(Icv)) && ffGreaterEqual(ff(Icv), In))
        return ffFloat(parabolaMax(Ip, Icv, In));
    return 0.0;
}

// suppressionWorker() over the output rect: the buffer's border, and pixels outside it, are zero.
[shader("compute")]
[numthreads(8, 8, 1)]
void edgedetect_nms(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint dst = dstIndex(tid.xy);
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    const int x2 = params.bufX1 + int(params.bufWidth);
    const int y2 = params.bufY1 + int(params.bufHeight);
    if (!inBuffer(x, y) || x == params.bufX1 || y == params.bufY1 || x == x2 - 1 || y == y2 - 1)
    {
        writeZero(dst);
        return;
    }
    const uint n = min(params.channels, 4u);
    if (params.mode != 0)
    {
        for (uint c = 0; c < n; ++c)
            outBuf[dst + c] = asuint(suppressed(x, y, c, c));
        return;
    }
    const float value = suppressed(x, y, params.flags, params.flags);
    for (uint c = 0; c < n; ++c)
        outBuf[dst + c] = asuint(value);
}
