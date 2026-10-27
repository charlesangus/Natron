import mapremap;

// IBlur's passes, after the workers of Engine/Nodes/Filter/IBlur.cpp and the box of
// Engine/Nodes/Filter/VariableBlurKernels.cpp. The work buffer covers the CPU's scratch rect, the
// domain of domWidth x domHeight pixels from (domX1, domY1): two slots of `channels` floats per
// pixel at fromOffset and toOffset, and the radii, two floats per pixel at radiiOffset.
//
// The CPU sums each plane in a double summed-area table and differences its entries, so its
// error grows with the sum of the whole domain; a float or two-float table loses too many digits
// on a large domain. Here each plane is quantised to integers of a power-of-two step and summed
// exactly in 64 bits, two uints: the step brings the plane's largest finite |value| below
// 2^valueBits, which leaves room for the sum of every pixel. The box differences the entries in
// integers, which gives exact sums over rectangles no larger than the box, and only weighs those
// in two floats, so its error is at most half a step: the plane's largest |value| times
// 2^-(valueBits + 1), plus the final rounding to float. Samples that are not finite read as 0,
// where the CPU's table spreads them.
//
// IBlurGpuParams and IBlurGpuConstants in Engine/Nodes/Filter/IBlur.cpp mirror IBlurParams and
// MapConstants.

struct MapConstants
{
    // kMap* flags.
    uint mapFlags;
    // The Map's channel, of mapChannels floats per pixel, its pixels (mapX1, mapY1)-(mapX2, mapY2)
    // from mapOffset, rows mapStride floats apart.
    uint mapChannel;
    uint mapChannels;
    uint mapOffset;
    uint mapStride;
    int mapX1;
    int mapY1;
    int mapX2;
    int mapY2;
    // MapInputParams::remap()'s exponent, 1 / Map Gamma; kMapSmooth in mapFlags is its smooth.
    float exponent;
    uint remapPad;
    // MapInputParams::sizeAt()'s sizes, and the factor taking a size to the standard deviation of
    // the Gaussian: the render scale over 2.4, horizontally over the pixel aspect ratio too.
    float sizeAtBlackX;
    float sizeAtBlackY;
    float sizeX;
    float sizeY;
    float sigmaScaleX;
    float sigmaScaleY;
    // The radii of a Map of 0, of 1, and outside the Map image, as the CPU computes them.
    float blackRx;
    float blackRy;
    float whiteRx;
    float whiteRy;
    float outsideRx;
    float outsideRy;
    uint pad;
};

// load: the source image.
StructuredBuffer<uint> srcBuf;
// radii: the Map image.
StructuredBuffer<uint> mapBuf;
StructuredBuffer<MapConstants> constBuf;
RWStructuredBuffer<float> workBuf;
// Row maxima at 0, the step's exponent at domHeight, then the table from satOffset: (domWidth + 1)
// x (domHeight + 1) entries of one plane, each two uints, low word first, row 0 and column 0 zero.
RWStructuredBuffer<uint> satBuf;
// The last box: outWidth x outHeight pixels at (outX1, outY1), the first at dstOffset.
RWStructuredBuffer<float> outBuf;

struct IBlurParams
{
    int domX1;
    int domY1;
    uint domWidth;
    uint domHeight;
    uint channels;
    // The component the table and the box work on.
    uint comp;
    uint fromOffset;
    uint toOffset;
    uint radiiOffset;
    uint valueBits;
    // kFinal when the box writes the result into outBuf.
    uint flags;
    uint satOffset;
    // load: the source pixels that exist, the first at srcOffset; empty when there are none.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint dstOffset;
    uint dstStride;
};
[[vk::push_constant]] ConstantBuffer<IBlurParams> params;

// Mirrored in Engine/Nodes/Filter/IBlur.cpp.
static const uint kFinal = 1;
static const uint kMapConnected = 1;
static const uint kMapRead = 2;
static const uint kMapInvert = 4;
static const uint kMapSmooth = 8;

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

float ffFloat(FF a)
{
    precise float v = a.hi + a.lo;
    return v;
}

// 2^n for n in [-126, 127], built exactly, as exp2() need not be.
float pow2(int n)
{
    return asfloat(uint(n + 127) << 23);
}

// v * 2^n for n in [-252, 254], in two exact products; only an underflow rounds.
float scaled(float v, int n)
{
    const int half = n / 2;
    precise float a = v * pow2(half);
    precise float b = a * pow2(n - half);
    return b;
}

bool isFinite(float v)
{
    return (asuint(v) & 0x7f800000u) != 0x7f800000u;
}

///////////////////////////////////////////////////////////////////////////////
// Two's complement 64-bit integers as (lo, hi) uints.

struct I64
{
    uint lo;
    uint hi;
};

I64 i64(uint lo, uint hi)
{
    I64 r;
    r.lo = lo;
    r.hi = hi;
    return r;
}

I64 i64Add(I64 a, I64 b)
{
    const uint lo = a.lo + b.lo;
    return i64(lo, a.hi + b.hi + ((lo < a.lo) ? 1u : 0u));
}

I64 i64Neg(I64 a)
{
    const uint lo = ~a.lo + 1u;
    return i64(lo, ~a.hi + ((lo == 0u) ? 1u : 0u));
}

I64 i64Sub(I64 a, I64 b)
{
    return i64Add(a, i64Neg(b));
}

// x is a whole number below 2^62 in magnitude: the high word and the remainder are exact floats.
I64 i64FromFloat(float x)
{
    const float ax = abs(x);
    const float h = floor(ax * 2.3283064365386963e-10);
    precise float l = ax - h * 4294967296.0;
    const I64 r = i64(uint(l), uint(h));
    return (x < 0.0) ? i64Neg(r) : r;
}

// In three pieces of at most 24 bits, each an exact float, summed in two floats.
FF i64ToFF(I64 a)
{
    const bool negative = (a.hi & 0x80000000u) != 0u;
    const I64 m = negative ? i64Neg(a) : a;
    const float p0 = float(m.hi >> 8) * 1099511627776.0;
    const float p1 = float(((m.hi & 0xffu) << 16) | (m.lo >> 16)) * 65536.0;
    const float p2 = float(m.lo & 0xffffu);
    const FF r = ffAdd(ffTwoSum(p0, p1), ff(p2));
    return negative ? ffNeg(r) : r;
}

///////////////////////////////////////////////////////////////////////////////
// Addressing.

uint workIndex(uint offset, uint x, uint y)
{
    return offset + (y * params.domWidth + x) * params.channels + params.comp;
}

uint satIndex(uint i, uint j)
{
    return params.satOffset + (j * (params.domWidth + 1) + i) * 2;
}

I64 satAt(uint i, uint j)
{
    const uint k = satIndex(i, j);
    return i64(satBuf[k], satBuf[k + 1]);
}

void satStore(uint i, uint j, I64 v)
{
    const uint k = satIndex(i, j);
    satBuf[k] = v.lo;
    satBuf[k + 1] = v.hi;
}

///////////////////////////////////////////////////////////////////////////////

[shader("compute")]
[numthreads(8, 8, 1)]
void iblur_load(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.domWidth || tid.y >= params.domHeight)
        return;
    const uint n = params.channels;
    const uint dst = params.fromOffset + (tid.y * params.domWidth + tid.x) * n;
    const int x = params.domX1 + int(tid.x);
    const int y = params.domY1 + int(tid.y);
    if (x < params.srcX1 || x >= params.srcX2 || y < params.srcY1 || y >= params.srcY2) {
        for (uint c = 0; c < n && c < 4; ++c)
            workBuf[dst + c] = 0.0;
        return;
    }
    const uint src = params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * n;
    for (uint c = 0; c < n && c < 4; ++c)
        workBuf[dst + c] = asfloat(srcBuf[src + c]);
}

// MapInputParams::remap(), by mapremap.slang.
float remapMap(MapConstants k, float raw)
{
    return mapRemap(raw, (k.mapFlags & kMapInvert) != 0u, (k.mapFlags & kMapSmooth) != 0u, k.exponent);
}

// VariableBlurKernels::boxRadius() in float; each loop moves n by one step at most in practice.
float boxRadius(float sigma)
{
    if (!(sigma > 0.0))
        return 0.0;
    const float target = sigma * sigma / 3.0;
    float n = floor((sqrt(1.0 + 12.0 * target) - 1.0) / 2.0);
    for (int k = 0; k < 8 && n > 0.0 && n * (n + 1.0) / 3.0 > target; ++k)
        n -= 1.0;
    for (int k = 0; k < 8 && (n + 1.0) * (n + 2.0) / 3.0 <= target; ++k)
        n += 1.0;
    const float f = (2.0 * n + 1.0) * (target - n * (n + 1.0) / 3.0) / (2.0 * ((n + 1.0) * (n + 1.0) - target));
    return n + clamp(f, 0.0, 1.0);
}

// radiusWorker(): the radii of every domain pixel. Map values of exactly 0 and 1, and the Map
// outside its image, take the radii the CPU computed.
[shader("compute")]
[numthreads(8, 8, 1)]
void iblur_radii(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.domWidth || tid.y >= params.domHeight)
        return;
    const MapConstants k = constBuf[0];
    const uint dst = params.radiiOffset + (tid.y * params.domWidth + tid.x) * 2;
    float rx = k.whiteRx;
    float ry = k.whiteRy;
    if ((k.mapFlags & kMapConnected) != 0u) {
        rx = k.outsideRx;
        ry = k.outsideRy;
        const int x = params.domX1 + int(tid.x);
        const int y = params.domY1 + int(tid.y);
        if ((k.mapFlags & kMapRead) != 0u && x >= k.mapX1 && x < k.mapX2 && y >= k.mapY1 && y < k.mapY2) {
            const float raw = asfloat(mapBuf[k.mapOffset + uint(y - k.mapY1) * k.mapStride + uint(x - k.mapX1) * k.mapChannels + k.mapChannel]);
            const float m = remapMap(k, raw);
            if (m == 0.0) {
                rx = k.blackRx;
                ry = k.blackRy;
            } else if (m == 1.0) {
                rx = k.whiteRx;
                ry = k.whiteRy;
            } else {
                rx = boxRadius(max(0.0, (1.0 - m) * k.sizeAtBlackX + m * k.sizeX) * k.sigmaScaleX);
                ry = boxRadius(max(0.0, (1.0 - m) * k.sizeAtBlackY + m * k.sizeY) * k.sigmaScaleY);
            }
        }
    }
    workBuf[dst] = rx;
    workBuf[dst + 1] = ry;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void iblur_rowmax(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.domHeight)
        return;
    uint best = 0u;
    for (uint x = 0; x < params.domWidth; ++x) {
        const uint bits = asuint(workBuf[workIndex(params.fromOffset, x, tid.x)]) & 0x7fffffffu;
        if (bits < 0x7f800000u)
            best = max(best, bits);
    }
    satBuf[tid.x] = best;
}

// The step's exponent: values times 2^shift lie below 2^valueBits.
[shader("compute")]
[numthreads(1, 1, 1)]
void iblur_shift(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x != 0u)
        return;
    uint best = 0u;
    for (uint y = 0; y < params.domHeight; ++y)
        best = max(best, satBuf[y]);
    int shift = 0;
    if (best != 0u) {
        // A normal value of exponent field e is below 2^(e - 126); a subnormal one below 2^-126.
        const int field = int(best >> 23);
        const int above = (field == 0) ? -126 : field - 126;
        shift = int(params.valueBits) - above;
    }
    satBuf[params.domHeight] = asuint(shift);
}

// prefixRow() of component comp of the slot at fromOffset, quantised, into row tid.x + 1 of the
// table; row 0 too for the first row. A NaN or an infinity counts as 0, as prefixRow()'s.
[shader("compute")]
[numthreads(64, 1, 1)]
void iblur_rows(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.domHeight)
        return;
    const int shift = asint(satBuf[params.domHeight]);
    const uint j = tid.x + 1;
    if (tid.x == 0u) {
        for (uint i = 0; i <= params.domWidth; ++i)
            satStore(i, 0, i64(0u, 0u));
    }
    I64 sum = i64(0u, 0u);
    satStore(0, j, sum);
    for (uint x = 0; x < params.domWidth; ++x) {
        const float v = workBuf[workIndex(params.fromOffset, x, tid.x)];
        if (isFinite(v))
            sum = i64Add(sum, i64FromFloat(round(scaled(v, shift))));
        satStore(x + 1, j, sum);
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void iblur_columns(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.domWidth)
        return;
    const uint i = tid.x + 1;
    I64 sum = satAt(i, 0);
    for (uint j = 1; j <= params.domHeight; ++j) {
        sum = i64Add(sum, satAt(i, j));
        satStore(i, j, sum);
    }
}

// One end of a box along an axis: the table index integral() interpolates from, and the weight
// of the next index, fu = u - i, as two floats. u is x - a for the lower end and x + 1 + a for the
// upper one, which split exactly into whole and fractional parts as a = n + f does.
struct BoxEnd
{
    int i;
    FF f;
};

BoxEnd boxEnd(int x, float a, bool lower, int size)
{
    const float n = floor(a);
    const float f = a - n;
    BoxEnd e;
    if (lower) {
        e.i = (f > 0.0) ? x - int(n) - 1 : x - int(n);
        e.f = (f > 0.0) ? ffTwoSum(1.0, -f) : ff(0.0);
    } else {
        e.i = x + 1 + int(n);
        e.f = ff(f);
    }
    // integral() clamps u to [0, size], and reads u = size as the last index with a weight of 1.
    if (e.i < 0) {
        e.i = 0;
        e.f = ff(0.0);
    } else if (e.i >= size) {
        e.i = size - 1;
        e.f = ff(1.0);
    }
    return e;
}

// The sum over the box of half-widths a and b around pixel (x, y), times 2^shift, as boxRow()
// reads it from the table: integral() at the four corners, each bilinear between four entries.
// Along x the weights of entries i0, i0 + 1, i1, i1 + 1 sum to zero, as they do along y, so
// differencing every entry with those of row j0 and column i0 changes nothing, and turns each into
// the exact sum over a rectangle within the box.
FF boxSum(int x, int y, float a, float b)
{
    const int w = int(params.domWidth);
    const int h = int(params.domHeight);
    const BoxEnd u0 = boxEnd(x, a, true, w);
    const BoxEnd u1 = boxEnd(x, a, false, w);
    const BoxEnd v0 = boxEnd(y, b, true, h);
    const BoxEnd v1 = boxEnd(y, b, false, h);

    int ix[3];
    FF wx[3];
    ix[0] = u0.i + 1;
    wx[0] = ffNeg(u0.f);
    ix[1] = u1.i;
    wx[1] = ffSub(ff(1.0), u1.f);
    ix[2] = u1.i + 1;
    wx[2] = u1.f;
    int iy[3];
    FF wy[3];
    iy[0] = v0.i + 1;
    wy[0] = ffNeg(v0.f);
    iy[1] = v1.i;
    wy[1] = ffSub(ff(1.0), v1.f);
    iy[2] = v1.i + 1;
    wy[2] = v1.f;

    const uint i0 = uint(u0.i);
    const uint j0 = uint(v0.i);
    const I64 corner = satAt(i0, j0);
    FF sum = ff(0.0);
    for (int q = 0; q < 3; ++q) {
        if (wy[q].hi == 0.0)
            continue;
        const uint j = uint(iy[q]);
        const I64 rowStart = i64Sub(satAt(i0, j), corner);
        for (int p = 0; p < 3; ++p) {
            if (wx[p].hi == 0.0)
                continue;
            const uint i = uint(ix[p]);
            const I64 d = i64Sub(i64Sub(satAt(i, j), satAt(i, j0)), rowStart);
            sum = ffAdd(sum, ffMul(ffMul(wx[p], wy[q]), i64ToFF(d)));
        }
    }
    return sum;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void iblur_box(uint3 tid : SV_DispatchThreadID)
{
    const bool last = (params.flags & kFinal) != 0u;
    int x;
    int y;
    uint dst;
    if (last) {
        if (tid.x >= params.outWidth || tid.y >= params.outHeight)
            return;
        dst = params.dstOffset + tid.y * params.dstStride + tid.x * params.channels + params.comp;
        x = params.outX1 + int(tid.x) - params.domX1;
        y = params.outY1 + int(tid.y) - params.domY1;
        if (x < 0 || y < 0 || x >= int(params.domWidth) || y >= int(params.domHeight)) {
            outBuf[dst] = 0.0;
            return;
        }
    } else {
        if (tid.x >= params.domWidth || tid.y >= params.domHeight)
            return;
        x = int(tid.x);
        y = int(tid.y);
        dst = workIndex(params.toOffset, tid.x, tid.y);
    }
    const uint r = params.radiiOffset + (uint(y) * params.domWidth + uint(x)) * 2;
    const float a = workBuf[r];
    const float b = workBuf[r + 1];
    float value;
    if (a <= 0.0 && b <= 0.0) {
        // No filter at all, so that a radius of 0 leaves the pixel exactly as it was.
        value = workBuf[workIndex(params.fromOffset, uint(x), uint(y))];
    } else {
        const int shift = asint(satBuf[params.domHeight]);
        FF sum = boxSum(x, y, a, b);
        sum = ff(scaled(sum.hi, -shift), scaled(sum.lo, -shift));
        // 2a + 1 and 2b + 1 are exact as two floats.
        const FF area = ffMul(ffTwoSum(2.0 * a, 1.0), ffTwoSum(2.0 * b, 1.0));
        value = ffFloat(ffDiv(sum, area));
    }
    if (last)
        outBuf[dst] = value;
    else
        workBuf[dst] = value;
}
