// The noise node's sampling core on the GPU: a line-for-line mirror of libs/noise/Noise4D.h (the
// basis generators, ported from Auburn/FastNoise2 721400f9560831be2fc9790be9b72c44bee7d33f, MIT,
// see libs/noise/LICENSE) followed by Engine/Nodes/Generator/NoiseCore.h (fractals and domain
// warp). Keep the three in step, including the order of every floating-point operation.
//
// Compile with -fp-mode precise so nothing is contracted or reassociated: the C++ uses no fused
// multiply-add either. Integer, compare, select, add, sub and mul are then bit-equal to the CPU
// (denormals aside, which a device may flush); sqrt and division are not required to be correctly
// rounded in Vulkan, so the cellular distances and the fractal normalisation can differ by a few
// ulps. Float constants are spelled as bit patterns; Noise4D.h static_asserts the same values.

// ---- libs/noise/Noise4D.h ----

static const uint kPrimeX = 0xF797C5C7u;
static const uint kPrimeY = 0x6C060C89u;
static const uint kPrimeZ = 0x465FD04Fu;
static const uint kPrimeW = 0xF7A62279u;
static const uint kHashMultiplierA = 0xB7E0A5F5u;
static const uint kSignBit = 0x80000000u;
static const float kInfinity = asfloat(0x7F800000u);

static const float kRangeMin = -1.0f;
static const float kTwoPow31 = 2147483648.0f;
static const float kTwoPowMinus31 = asfloat(0x30000000u); // 2^-31
static const float kTwoPow32 = 4294967296.0f;
static const float kTwoPowMinus32 = asfloat(0x2F800000u); // 2^-32

// The cellular metrics.
static const int kNoiseMetricEuclidean = 0;
static const int kNoiseMetricManhattan = 1;
static const int kNoiseMetricChebyshev = 2;

// The cellular outputs.
static const int kNoiseCellularF1 = 0;
static const int kNoiseCellularF2 = 1;
static const int kNoiseCellularF2MinusF1 = 2;
static const int kNoiseCellularCellValue = 3;
static const int kNoiseCellularDistanceToEdge = 4;
static const int kNoiseCellularSmoothF1 = 5;

// A point in noise space: per axis an integer cell (two's complement, wrapping) and a fraction in
// [0, 1).
struct NoiseCoord {
    uint cx;
    uint cy;
    uint cz;
    uint cw;
    float fx;
    float fy;
    float fz;
    float fw;
};

// One axis of a NoiseCoord.
struct NoiseAxis {
    uint c;
    float f;
};

struct NoiseVec4 {
    float x;
    float y;
    float z;
    float w;
};

float
selectF(bool m, float a, float b)
{
    return m ? a : b;
}
uint
selectU(bool m, uint a, uint b)
{
    return m ? a : b;
}

// SSE semantics: max_ps(a, b) is a > b ? a : b, min_ps(a, b) is a < b ? a : b.
float
vmax(float a, float b)
{
    return a > b ? a : b;
}
float
vmin(float a, float b)
{
    return a < b ? a : b;
}
float
vabs(float a)
{
    return asfloat(asuint(a) & 0x7FFFFFFFu);
}
float
vsqrt(float a)
{
    return sqrt(a);
}

float
muladd(float a, float b, float c)
{
    return a * b + c;
}
float
nmuladd(float a, float b, float c)
{
    return c - a * b;
}

// float32v ^ float32v where the right side is a sign mask.
float
xorBits(float a, uint bits)
{
    return asfloat(asuint(a) ^ bits);
}

uint
sra(uint a, int n)
{
    return uint(int(a) >> n);
}

float
masked(bool m, float a)
{
    return m ? a : 0.0f;
}
float
maskedDecrement(bool m, float a)
{
    return a + masked(m, -1.0f);
}

// Round half to even, exact for every float. Adding and removing 2^23 leaves no fraction bits;
// magnitudes from 2^23 up are already integers and get a zero constant instead.
float
roundEven(float a)
{
    uint magnitude = vabs(a) < 8388608.0f ? 0x4B000000u : 0u;
    float c = asfloat((asuint(a) & kSignBit) | magnitude);
    return (a + c) - c;
}

// Exact floor without a library call: round, then decrement where it rounded up.
float
floorV(float a)
{
    float r = roundEven(a);
    return maskedDecrement(r > a, r);
}

// An integral float in the int range, as its two's complement bits.
uint
toUint(float a)
{
    return uint(int(a));
}
float
toFloat(uint a)
{
    return float(int(a));
}

float
invSqrt(float a)
{
    return 1.0f / vsqrt(a);
}

// An integral float of any magnitude, modulo 2^32. Every step is exact: the magnitude less a
// multiple of 2^32 has no more significant bits than the magnitude.
uint
wrapIntegral(float v)
{
    bool negative = v < 0.0f;
    float a = vabs(v);
    float turns = floorV(a * kTwoPowMinus32);
    float r = a - turns * kTwoPow32;
    bool high = r >= kTwoPow31;
    r = selectF(high, r - kTwoPow31, r);
    uint u = toUint(r) + selectU(high, kSignBit, 0u);
    return selectU(negative, 0u - u, u);
}

// Brings the fraction back into [0, 1), moving whole cells into the cell. |f| < 2^31.
NoiseAxis
axisNormalize(uint c, float f)
{
    float k = floorV(f);
    NoiseAxis r;
    r.c = c + toUint(k);
    r.f = f - k;
    // f - k rounds up to 1 when f is a tiny negative number.
    bool wrap = r.f >= 1.0f;
    r.c = r.c + selectU(wrap, 1u, 0u);
    r.f = selectF(wrap, r.f - 1.0f, r.f);
    return r;
}

// A float of any magnitude as a cell and a fraction.
NoiseAxis
axisFromFloat(float t)
{
    float k = floorV(t);
    NoiseAxis r;
    r.c = wrapIntegral(k);
    r.f = t - k;
    return r;
}

// (c + f) * m for the signed cell c, as a cell and a fraction. m splits into two halves of 12
// significant bits and c into two 12-bit pieces and a signed top byte, so each of the six
// products has at most 24 significant bits and is exact; only f * m and the sum of the fractions
// round. A cell in [-4096, 4096) needs only the two products of c itself.
NoiseAxis
axisScale(uint c, float f, float m)
{
    float mHi = asfloat(asuint(m) & 0xFFFFF000u);
    float mLo = m - mHi;
    NoiseAxis tf = axisFromFloat(f * m);
    if (c + 4096u < 8192u) {
        float cs = toFloat(c);
        NoiseAxis s0 = axisFromFloat(cs * mHi);
        NoiseAxis s1 = axisFromFloat(cs * mLo);
        return axisNormalize(s0.c + s1.c + tf.c, (s0.f + s1.f) + tf.f);
    }
    float c0 = toFloat(c & 0xFFFu);
    float c1 = toFloat((c >> 12) & 0xFFFu);
    float c2 = toFloat(sra(c, 24));
    NoiseAxis t0 = axisFromFloat(c0 * mHi);
    NoiseAxis t1 = axisFromFloat(c0 * mLo);
    NoiseAxis t2 = axisFromFloat((c1 * mHi) * 4096.0f);
    NoiseAxis t3 = axisFromFloat((c1 * mLo) * 4096.0f);
    NoiseAxis t4 = axisFromFloat((c2 * mHi) * 16777216.0f);
    NoiseAxis t5 = axisFromFloat((c2 * mLo) * 16777216.0f);
    uint cell = t0.c + t1.c + t2.c + t3.c + t4.c + t5.c + tf.c;
    float fraction = (((((t0.f + t1.f) + t2.f) + t3.f) + t4.f) + t5.f) + tf.f;
    return axisNormalize(cell, fraction);
}

// a + b.
NoiseAxis
axisAdd(NoiseAxis a, NoiseAxis b)
{
    return axisNormalize(a.c + b.c, a.f + b.f);
}

NoiseCoord
coordFromAxes(NoiseAxis x, NoiseAxis y, NoiseAxis z, NoiseAxis w)
{
    NoiseCoord p;
    p.cx = x.c;
    p.cy = y.c;
    p.cz = z.c;
    p.cw = w.c;
    p.fx = x.f;
    p.fy = y.f;
    p.fz = z.f;
    p.fw = w.f;
    return p;
}

// A point given as plain floats. Their precision is the float's; use the split forms for points
// far from the origin.
NoiseCoord
coordFromFloats(float x, float y, float z, float w)
{
    NoiseAxis ax = axisFromFloat(x);
    NoiseAxis ay = axisFromFloat(y);
    NoiseAxis az = axisFromFloat(z);
    NoiseAxis aw = axisFromFloat(w);
    return coordFromAxes(axisNormalize(ax.c, ax.f), axisNormalize(ay.c, ay.f), axisNormalize(az.c, az.f), axisNormalize(aw.c, aw.f));
}

// Every axis of p times m.
NoiseCoord
coordScale(NoiseCoord p, float m)
{
    return coordFromAxes(axisScale(p.cx, p.fx, m), axisScale(p.cy, p.fy, m), axisScale(p.cz, p.fz, m), axisScale(p.cw, p.fw, m));
}

// p moved by (x, y, z, w), each of magnitude below 2^31.
NoiseCoord
coordOffset(NoiseCoord p, float x, float y, float z, float w)
{
    return coordFromAxes(axisNormalize(p.cx, p.fx + x), axisNormalize(p.cy, p.fy + y), axisNormalize(p.cz, p.fz + z), axisNormalize(p.cw, p.fw + w));
}

uint
hashPrimes(uint seed, uint x, uint y, uint z, uint w)
{
    uint hash = seed;
    hash ^= x ^ (y ^ (z ^ w));

    hash *= kHashMultiplierA;

    return sra(hash, 15) ^ hash;
}

uint
hashPrimesHB(uint seed, uint x, uint y, uint z, uint w)
{
    uint hash = seed;
    hash ^= x ^ (y ^ (z ^ w));

    hash *= kHashMultiplierA;
    return hash;
}

float
getValueCoord(uint seed, uint x, uint y, uint z, uint w)
{
    uint hash = seed;
    hash ^= x ^ (y ^ (z ^ w));

    hash *= hash * kHashMultiplierA;
    return toFloat(hash);
}

float
lerpV(float a, float b, float t)
{
    return muladd(t, b - a, a);
}

float
interpHermite(float t)
{
    return t * t * nmuladd(t, 2.0f, 3.0f);
}

float
interpQuintic(float t)
{
    return t * t * t * muladd(t, muladd(t, 6.0f, -15.0f), 10.0f);
}

float
getGradientDotPerlin(uint hash, float fX, float fY, float fZ, float fW)
{
    uint p = hash & (3u << 3);

    float a = selectF(int(p) > 0, fX, fY);
    float b = selectF(int(hash << 27) < 0, fY, fZ);
    float c = selectF(int(p) > (2 << 3), fZ, fW);

    uint aSign = hash << 31;
    uint bSign = (hash >> 1) << 31;
    uint cSign = (hash >> 2) << 31;

    return xorBits(a, aSign) + xorBits(b, bSign) + xorBits(c, cSign);
}

// VariableRange::ScaleOutput with the default range [-1, 1]; invRangeTimesScale is
// float(1.0f / (nativeMax - nativeMin)) * 2.
float
scaleOutput(float value, float nativeMin, float invRangeTimesScale)
{
    return muladd(invRangeTimesScale, value - nativeMin, kRangeMin);
}

static const float kPerlinBounding = asfloat(0x3fab56cbu); // 1.33858621
static const float kPerlinInvRange = asfloat(0x3f3f3f1eu); // 0.747056842

// Perlin noise in [-1, 1].
float
perlin4(uint seed, NoiseCoord p)
{
    uint x0 = p.cx * kPrimeX;
    uint y0 = p.cy * kPrimeY;
    uint z0 = p.cz * kPrimeZ;
    uint w0 = p.cw * kPrimeW;
    uint x1 = x0 + kPrimeX;
    uint y1 = y0 + kPrimeY;
    uint z1 = z0 + kPrimeZ;
    uint w1 = w0 + kPrimeW;

    float xf0 = p.fx;
    float yf0 = p.fy;
    float zf0 = p.fz;
    float wf0 = p.fw;
    float xf1 = xf0 - 1.0f;
    float yf1 = yf0 - 1.0f;
    float zf1 = zf0 - 1.0f;
    float wf1 = wf0 - 1.0f;

    float xs = interpQuintic(xf0);
    float ys = interpQuintic(yf0);
    float zs = interpQuintic(zf0);
    float ws = interpQuintic(wf0);

    float value = lerpV(lerpV(lerpV(lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y0, z0, w0), xf0, yf0, zf0, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y0, z0, w0), xf1, yf0, zf0, wf0), xs),
                                    lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y1, z0, w0), xf0, yf1, zf0, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y1, z0, w0), xf1, yf1, zf0, wf0), xs), ys),
                              lerpV(lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y0, z1, w0), xf0, yf0, zf1, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y0, z1, w0), xf1, yf0, zf1, wf0), xs),
                                    lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y1, z1, w0), xf0, yf1, zf1, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y1, z1, w0), xf1, yf1, zf1, wf0), xs), ys),
                              zs),
                        lerpV(lerpV(lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y0, z0, w1), xf0, yf0, zf0, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y0, z0, w1), xf1, yf0, zf0, wf1), xs),
                                    lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y1, z0, w1), xf0, yf1, zf0, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y1, z0, w1), xf1, yf1, zf0, wf1), xs), ys),
                              lerpV(lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y0, z1, w1), xf0, yf0, zf1, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y0, z1, w1), xf1, yf0, zf1, wf1), xs),
                                    lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y1, z1, w1), xf0, yf1, zf1, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y1, z1, w1), xf1, yf1, zf1, wf1), xs), ys),
                              zs),
                        ws);

    return scaleOutput(value, -kPerlinBounding, kPerlinInvRange);
}

static const float kValueBounds = 2147483648.0f;
static const float kValueInvRange = kTwoPowMinus31;

// Value noise in [-1, 1].
float
value4(uint seed, NoiseCoord p)
{
    uint x0 = p.cx * kPrimeX;
    uint y0 = p.cy * kPrimeY;
    uint z0 = p.cz * kPrimeZ;
    uint w0 = p.cw * kPrimeW;
    uint x1 = x0 + kPrimeX;
    uint y1 = y0 + kPrimeY;
    uint z1 = z0 + kPrimeZ;
    uint w1 = w0 + kPrimeW;

    float xs = interpHermite(p.fx);
    float ys = interpHermite(p.fy);
    float zs = interpHermite(p.fz);
    float ws = interpHermite(p.fw);

    float value = lerpV(lerpV(lerpV(lerpV(getValueCoord(seed, x0, y0, z0, w0), getValueCoord(seed, x1, y0, z0, w0), xs),
                                    lerpV(getValueCoord(seed, x0, y1, z0, w0), getValueCoord(seed, x1, y1, z0, w0), xs), ys),
                              lerpV(lerpV(getValueCoord(seed, x0, y0, z1, w0), getValueCoord(seed, x1, y0, z1, w0), xs),
                                    lerpV(getValueCoord(seed, x0, y1, z1, w0), getValueCoord(seed, x1, y1, z1, w0), xs), ys),
                              zs),
                        lerpV(lerpV(lerpV(getValueCoord(seed, x0, y0, z0, w1), getValueCoord(seed, x1, y0, z0, w1), xs),
                                    lerpV(getValueCoord(seed, x0, y1, z0, w1), getValueCoord(seed, x1, y1, z0, w1), xs), ys),
                              lerpV(lerpV(getValueCoord(seed, x0, y0, z1, w1), getValueCoord(seed, x1, y0, z1, w1), xs),
                                    lerpV(getValueCoord(seed, x0, y1, z1, w1), getValueCoord(seed, x1, y1, z1, w1), xs), ys),
                              zs),
                        ws);

    return scaleOutput(value, -kValueBounds, kValueInvRange);
}

// White noise in [-1, 1): one hash of the cell, so every point of a cell has the same value. The
// integer hash and the power-of-two scale are exact, so the GPU matches bit for bit.
float
white4(uint seed, NoiseCoord p)
{
    return getValueCoord(seed, p.cx * kPrimeX, p.cy * kPrimeY, p.cz * kPrimeZ, p.cw * kPrimeW) * kTwoPowMinus31;
}

static const float kSkew4 = asfloat(0x3e9e377au); // 1 / (sqrt(5) + 1) = 0.309017003
static const float kTwiceUnskew4 = asfloat(0xbe4ccccdu); // -1 / 5 = -0.200000003
static const float kUnskew4 = asfloat(0xbe0d8369u); // -1 / (sqrt(5) + 5) = -0.138196602
static const float kFalloffRadiusSquared = asfloat(0x3f4ccccdu); // 4 / 5 = 0.800000012
static const float kDistanceSquaredAHalf = asfloat(0x3ecccccdu); // 4 / 5 / 2 = 0.400000006
static const float kDistanceSquaredBHalf = asfloat(0x3f19999au); // 6 / 5 / 2 = 0.600000024
static const float kUnskew4x4p1 = asfloat(0x3ee4f92eu); // 4u + 1 = 0.44721359
static const float kUnskew4x3p1 = asfloat(0x3f15dd71u); // 3u + 1 = 0.585410178
static const float kUnskew4x3 = asfloat(0xbed4451du); // 3u = -0.414589792
static const float kUnskew4x2p1 = asfloat(0x3f393e4cu); // 2u + 1 = 0.723606825
static const float kUnskew4x2 = asfloat(0xbe8d8369u); // 2u = -0.276393205
static const float kUnskew4p1 = asfloat(0x3f5c9f26u); // u + 1 = 0.861803412
static const float kSimplexBounding = asfloat(0x3c394da3u); // 0.0113100139
static const float kSimplexInvRange = asfloat(0x42b0d59eu); // 88.4172211

struct SimplexScore {
    float maxScore;
    uint moveMaskBits;
};

SimplexScore
considerVertex(SimplexScore s, float score, uint bits)
{
    s.moveMaskBits = selectU(score > s.maxScore, bits, s.moveMaskBits);
    s.maxScore = vmax(s.maxScore, score);
    return s;
}

// One SuperSimplex vertex's contribution added to value.
float
simplexVertex(float value, uint seed, uint xPrimed, uint yPrimed, uint zPrimed, uint wPrimed, float dx, float dy, float dz, float dw, float falloffBase)
{
    float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimed, zPrimed, wPrimed), dx, dy, dz, dw);
    falloffBase = vmax(falloffBase, 0.0f);
    return muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
}

// SuperSimplex (OpenSimplex2S) noise in [-1, 1]. The skew is computed on the split coordinate, so
// the skewed cell is exact far from the origin.
float
superSimplex4(uint seed, NoiseCoord p)
{
    NoiseAxis skew = axisScale(p.cx + p.cy + p.cz + p.cw, ((p.fx + p.fy) + p.fz) + p.fw, kSkew4);
    NoiseAxis xk = axisNormalize(p.cx + skew.c, p.fx + skew.f);
    NoiseAxis yk = axisNormalize(p.cy + skew.c, p.fy + skew.f);
    NoiseAxis zk = axisNormalize(p.cz + skew.c, p.fz + skew.f);
    NoiseAxis wk = axisNormalize(p.cw + skew.c, p.fw + skew.f);
    uint xSkewedBase = xk.c;
    uint ySkewedBase = yk.c;
    uint zSkewedBase = zk.c;
    uint wSkewedBase = wk.c;
    float dxSkewed = xk.f;
    float dySkewed = yk.f;
    float dzSkewed = zk.f;
    float dwSkewed = wk.f;

    // From the unit cell base, find the closest vertex.
    {
        // A double unskew gives the vector whose dot product with skewed vectors is the unskewed result.
        float twiceUnskewDelta = kTwiceUnskew4 * (((dxSkewed + dySkewed) + dzSkewed) + dwSkewed);
        float xNormal = dxSkewed + twiceUnskewDelta;
        float yNormal = dySkewed + twiceUnskewDelta;
        float zNormal = dzSkewed + twiceUnskewDelta;
        float wNormal = dwSkewed + twiceUnskewDelta;
        float xyzwNormal = -twiceUnskewDelta; // xNormal + yNormal + zNormal + wNormal

        // Compare scores to find the closest vertex.
        SimplexScore s;
        s.maxScore = 0.6f - xyzwNormal;
        s.moveMaskBits = selectU(0.2f > s.maxScore, 0xFFFFFFFFu, 0u);
        s.maxScore = vmax(s.maxScore, 0.2f);
        s = considerVertex(s, -wNormal, 7u);
        s = considerVertex(s, -zNormal, 11u);
        s = considerVertex(s, -yNormal, 13u);
        s = considerVertex(s, -xNormal, 14u);
        s.maxScore += xyzwNormal - 0.2f;
        s = considerVertex(s, xNormal, 1u);
        s = considerVertex(s, yNormal, 2u);
        s = considerVertex(s, zNormal, 4u);
        s = considerVertex(s, wNormal, 8u);
        s.maxScore += 0.2f - xNormal;
        s = considerVertex(s, yNormal, 3u);
        s = considerVertex(s, zNormal, 5u);
        s = considerVertex(s, wNormal, 9u);
        s.maxScore += xNormal;
        s = considerVertex(s, yNormal + zNormal, 6u);
        s.maxScore -= wNormal;
        s = considerVertex(s, yNormal, 10u);
        s = considerVertex(s, zNormal, 12u);

        bool moveX = (s.moveMaskBits & 1u) != 0u;
        bool moveY = (s.moveMaskBits & 2u) != 0u;
        bool moveZ = (s.moveMaskBits & 4u) != 0u;
        bool moveW = (s.moveMaskBits & 8u) != 0u;

        xSkewedBase += selectU(moveX, 1u, 0u);
        ySkewedBase += selectU(moveY, 1u, 0u);
        zSkewedBase += selectU(moveZ, 1u, 0u);
        wSkewedBase += selectU(moveW, 1u, 0u);

        dxSkewed = maskedDecrement(moveX, dxSkewed);
        dySkewed = maskedDecrement(moveY, dySkewed);
        dzSkewed = maskedDecrement(moveZ, dzSkewed);
        dwSkewed = maskedDecrement(moveW, dwSkewed);
    }

    uint xPrimedBase = xSkewedBase * kPrimeX;
    uint yPrimedBase = ySkewedBase * kPrimeY;
    uint zPrimedBase = zSkewedBase * kPrimeZ;
    uint wPrimedBase = wSkewedBase * kPrimeW;

    float skewedCoordinateSum = ((dxSkewed + dySkewed) + dzSkewed) + dwSkewed;
    float twiceUnskewDelta = kTwiceUnskew4 * skewedCoordinateSum;
    float xNormal = dxSkewed + twiceUnskewDelta;
    float yNormal = dySkewed + twiceUnskewDelta;
    float zNormal = dzSkewed + twiceUnskewDelta;
    float wNormal = dwSkewed + twiceUnskewDelta;
    float xyzwNormal = -twiceUnskewDelta; // xNormal + yNormal + zNormal + wNormal

    float unskewDelta = kUnskew4 * skewedCoordinateSum;
    float dxBase = dxSkewed + unskewDelta;
    float dyBase = dySkewed + unskewDelta;
    float dzBase = dzSkewed + unskewDelta;
    float dwBase = dwSkewed + unskewDelta;

    float coordinateSum = kUnskew4x4p1 * skewedCoordinateSum; // dxBase + dyBase + dzBase + dwBase

    // Vertex <0, 0, 0, 0>
    float value;
    float falloffBaseStemA;
    float falloffBaseStemB;
    {
        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimedBase, zPrimedBase, wPrimedBase), dxBase, dyBase, dzBase, dwBase);
        float falloffBase = nmuladd(dwBase, dwBase, nmuladd(dzBase, dzBase, nmuladd(dyBase, dyBase, nmuladd(dxBase, dxBase, kFalloffRadiusSquared)))) * 0.5f;
        falloffBaseStemA = falloffBase - kDistanceSquaredAHalf;
        falloffBaseStemB = falloffBase - kDistanceSquaredBHalf;
        value = (falloffBase * falloffBase) * (falloffBase * falloffBase) * gradientRampValue;
    }

    // Vertex <1, 1, 1, 1> or <-1, -1, -1, -1>
    {
        bool signMask = xyzwNormal < 0.0f;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset = xorBits(kUnskew4x4p1, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset, dyBase - offset, dzBase - offset, dwBase - offset, muladd(offset, coordinateSum, falloffBaseStemA));
    }

    // Vertex <1, 1, 1, 0> or <-1, -1, -1, 0>
    {
        bool signMask = xyzwNormal < wNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase,
                              dxBase - offset1, dyBase - offset1, dzBase - offset1, dwBase - offset0, muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dwBase, sign));
    }

    // Vertex <1, 1, 0, 1> or <-1, -1, 0, -1>
    {
        bool signMask = xyzwNormal < zNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase, wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset1, dyBase - offset1, dzBase - offset0, dwBase - offset1, muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dzBase, sign));
    }

    // Vertex <1, 0, 1, 1> or <-1, 0, -1, -1>
    {
        bool signMask = xyzwNormal < yNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase, zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset1, dyBase - offset0, dzBase - offset1, dwBase - offset1, muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dyBase, sign));
    }

    // Vertex <0, 1, 1, 1> or <0, -1, -1, -1>
    {
        bool signMask = xyzwNormal < xNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset0, dyBase - offset1, dzBase - offset1, dwBase - offset1, muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dxBase, sign));
    }

    // Vertex <1, 0, 0, 0> or <-1, 0, 0, 0>
    {
        bool signMask = xNormal < 0.0f;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase, zPrimedBase, wPrimedBase,
                              dxBase - offset1, dyBase - offset0, dzBase - offset0, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dxBase, sign));
    }

    // Vertex <1, 1, 0, 0> or <-1, -1, 0, 0>
    {
        bool signMask = xNormal < -yNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase, wPrimedBase,
                              dxBase - offset1, dyBase - offset1, dzBase - offset0, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dyBase, sign));
    }

    // Vertex <1, 0, 1, 0> or <-1, 0, -1, 0>
    {
        bool signMask = xNormal < -zNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase, zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase,
                              dxBase - offset1, dyBase - offset0, dzBase - offset1, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dzBase, sign));
    }

    // Vertex <1, 0, 0, 1> or <-1, 0, 0, -1>
    {
        bool signMask = xNormal < -wNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase, zPrimedBase, wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset1, dyBase - offset0, dzBase - offset0, dwBase - offset1, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dwBase, sign));
    }

    // Vertex <0, 1, 0, 0> or <0, -1, 0, 0>
    {
        bool signMask = yNormal < 0.0f;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase, wPrimedBase,
                              dxBase - offset0, dyBase - offset1, dzBase - offset0, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dyBase, sign));
    }

    // Vertex <0, 1, 1, 0> or <0, -1, -1, 0>
    {
        bool signMask = yNormal < -zNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase,
                              dxBase - offset0, dyBase - offset1, dzBase - offset1, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dyBase + dzBase, sign));
    }

    // Vertex <0, 1, 0, 1> or <0, -1, 0, -1>
    {
        bool signMask = yNormal < -wNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase, wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset0, dyBase - offset1, dzBase - offset0, dwBase - offset1, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dyBase + dwBase, sign));
    }

    // Vertex <0, 0, 1, 0> or <0, 0, -1, 0>
    {
        bool signMask = zNormal < 0.0f;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase, zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase,
                              dxBase - offset0, dyBase - offset0, dzBase - offset1, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dzBase, sign));
    }

    // Vertex <0, 0, 1, 1> or <0, 0, -1, -1>
    {
        bool signMask = zNormal < -wNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase, zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset0, dyBase - offset0, dzBase - offset1, dwBase - offset1, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dzBase + dwBase, sign));
    }

    // Vertex <0, 0, 0, 1> or <0, 0, 0, -1>
    {
        bool signMask = wNormal < 0.0f;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase, zPrimedBase, wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset0, dyBase - offset0, dzBase - offset0, dwBase - offset1, muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dwBase, sign));
    }

    return scaleOutput(value, -kSimplexBounding, kSimplexInvRange);
}

static const float kJitter4D = asfloat(0x3ebb67a1u); // 0.366025
static const float kHalfByte = 127.5f;

// Bounds on F2 under each metric, so the outputs map [0, bound] to [-1, 1]. Two of the searched
// points are the jittered lattice point L nearest the sample and its neighbour one step along the
// sample's largest offset from L; without jitter both lie within distance 1 (Euclidean and
// Chebyshev) or 2 (Manhattan), and the jitter moves a point by at most kJitter4D in Euclidean and
// Chebyshev distance and twice that in Manhattan distance.
static const float kCellularEuclideanRange = 1.0f + kJitter4D;
static const float kCellularManhattanRange = 2.0f + 2.0f * kJitter4D;
static const float kCellularChebyshevRange = 1.0f + kJitter4D;
static const float kCellularEuclideanScale = asfloat(0x3fbb67b2u); // 2 / kCellularEuclideanRange
static const float kCellularManhattanScale = asfloat(0x3f3b67b2u); // 2 / kCellularManhattanRange
static const float kCellularChebyshevScale = asfloat(0x3fbb67b2u); // 2 / kCellularChebyshevRange

// A feature point relative to the sample: the cell's hash gives a direction, the jitter its length
// (FastNoise2's CellularDistance inner step). offset is the cell centre less the sample.
NoiseVec4
cellPoint(uint hash, float jitter, float xOffset, float yOffset, float zOffset, float wOffset)
{
    float xd = toFloat(hash & 0xffu) - kHalfByte;
    float yd = toFloat((hash >> 8) & 0xffu) - kHalfByte;
    float zd = toFloat((hash >> 16) & 0xffu) - kHalfByte;
    float wd = toFloat(hash >> 24) - kHalfByte;

    float invMag = jitter * invSqrt(muladd(xd, xd, muladd(yd, yd, muladd(zd, zd, wd * wd))));
    NoiseVec4 r;
    r.x = muladd(xd, invMag, xOffset);
    r.y = muladd(yd, invMag, yOffset);
    r.z = muladd(zd, invMag, zOffset);
    r.w = muladd(wd, invMag, wOffset);
    return r;
}

// The distance of a feature point under the metric; Euclidean is left squared.
float
cellDistance(int metric, NoiseVec4 d)
{
    if (metric == kNoiseMetricManhattan) {
        return ((vabs(d.x) + vabs(d.y)) + vabs(d.z)) + vabs(d.w);
    }
    if (metric == kNoiseMetricChebyshev) {
        return vmax(vmax(vmax(vabs(d.x), vabs(d.y)), vabs(d.z)), vabs(d.w));
    }
    float distSqr = d.x * d.x;
    distSqr = muladd(d.y, d.y, distSqr);
    distSqr = muladd(d.z, d.z, distSqr);
    distSqr = muladd(d.w, d.w, distSqr);
    return distSqr;
}

// Polynomial smooth minimum: min(a, b) less a quadratic blend where they are within k.
float
smoothMin(float a, float b, float k)
{
    float e = vmax(k - vabs(a - b), 0.0f);
    return vmin(a, b) - e * e * (0.25f / k);
}

// The distance from the sample to the bisector of the nearest point n and another point q, both
// relative to the sample.
float
bisectorDistance(NoiseVec4 n, NoiseVec4 q)
{
    float dx = q.x - n.x;
    float dy = q.y - n.y;
    float dz = q.z - n.z;
    float dw = q.w - n.w;
    float lengthSqr = muladd(dw, dw, muladd(dz, dz, muladd(dy, dy, dx * dx)));
    float along = muladd((n.w + q.w) * 0.5f, dw, muladd((n.z + q.z) * 0.5f, dz, muladd((n.y + q.y) * 0.5f, dy, ((n.x + q.x) * 0.5f) * dx)));
    return selectF(lengthSqr > 0.0f, along * invSqrt(lengthSqr), kInfinity);
}

// Cellular (Worley) noise over the 3^4 cells around the sample's nearest lattice point
// (FastNoise2's CellularDistance and CellularValue search), in [-1, 1].
//
// - F1, F2, F2 - F1: the nearest and second-nearest feature distances under the metric.
// - Cell value: the nearest cell's hash.
// - Distance to edge: Euclidean only, the distance to the nearest bisector between the nearest
//   point and any other, found in a second pass. Manhattan and Chebyshev cells are not bounded by
//   planes, so those metrics give (F2 - F1) / 2 instead.
// - Smooth F1: every distance folded through smoothMin with width smoothness.
//
// gridJitter is clamped to [0, 1], so the feature points stay inside their cells.
float
cellular4(uint seed, NoiseCoord p, int metric, int output, float gridJitter, float smoothness)
{
    float jitter = kJitter4D * vmin(vmax(gridJitter, 0.0f), 1.0f);
    float smoothWidth = vmax(smoothness, 0.000001f);
    bool euclidean = metric != kNoiseMetricManhattan && metric != kNoiseMetricChebyshev;

    // The 3^4 block starts one cell below the lattice point nearest the sample.
    uint xcBase = p.cx + selectU(p.fx >= 0.5f, 1u, 0u) - 1u;
    uint ycBase = p.cy + selectU(p.fy >= 0.5f, 1u, 0u) - 1u;
    uint zcBase = p.cz + selectU(p.fz >= 0.5f, 1u, 0u) - 1u;
    uint wcBase = p.cw + selectU(p.fw >= 0.5f, 1u, 0u) - 1u;
    float xcfBase = toFloat(xcBase - p.cx);
    float ycfBase = toFloat(ycBase - p.cy);
    float zcfBase = toFloat(zcBase - p.cz);
    float wcfBase = toFloat(wcBase - p.cw);
    uint xpBase = xcBase * kPrimeX;
    uint ypBase = ycBase * kPrimeY;
    uint zpBase = zcBase * kPrimeZ;
    uint wpBase = wcBase * kPrimeW;

    float distance0 = kInfinity;
    float distance1 = kInfinity;
    float smooth = kInfinity;
    uint valueHash = 0u;
    int nearestIndex = 0;
    NoiseVec4 nearest;
    nearest.x = 0.0f;
    nearest.y = 0.0f;
    nearest.z = 0.0f;
    nearest.w = 0.0f;

    int index = 0;
    float xcf = xcfBase;
    uint xc = xpBase;
    for (int xi = 0; xi < 3; xi++) {
        float ycf = ycfBase;
        uint yc = ypBase;
        for (int yi = 0; yi < 3; yi++) {
            float zcf = zcfBase;
            uint zc = zpBase;
            for (int zi = 0; zi < 3; zi++) {
                float wcf = wcfBase;
                uint wc = wpBase;
                for (int wi = 0; wi < 3; wi++) {
                    uint hash = hashPrimesHB(seed, xc, yc, zc, wc);
                    NoiseVec4 d = cellPoint(hash, jitter, xcf - p.fx, ycf - p.fy, zcf - p.fz, wcf - p.fw);
                    float newDistance = cellDistance(metric, d);

                    bool closer0 = newDistance < distance0;
                    bool closer1 = newDistance < distance1;
                    distance1 = selectF(closer0, distance0, selectF(closer1, newDistance, distance1));
                    distance0 = selectF(closer0, newDistance, distance0);
                    valueHash = selectU(closer0, hash, valueHash);
                    nearestIndex = closer0 ? index : nearestIndex;
                    nearest.x = selectF(closer0, d.x, nearest.x);
                    nearest.y = selectF(closer0, d.y, nearest.y);
                    nearest.z = selectF(closer0, d.z, nearest.z);
                    nearest.w = selectF(closer0, d.w, nearest.w);

                    if (output == kNoiseCellularSmoothF1) {
                        smooth = smoothMin(smooth, euclidean ? vsqrt(newDistance) : newDistance, smoothWidth);
                    }

                    index++;
                    wcf += 1.0f;
                    wc += kPrimeW;
                }
                zcf += 1.0f;
                zc += kPrimeZ;
            }
            ycf += 1.0f;
            yc += kPrimeY;
        }
        xcf += 1.0f;
        xc += kPrimeX;
    }

    if (output == kNoiseCellularCellValue) {
        return toFloat(valueHash) * kTwoPowMinus31;
    }

    if (euclidean) {
        distance0 = vsqrt(distance0);
        distance1 = vsqrt(distance1);
    }
    float scale = euclidean ? kCellularEuclideanScale : (metric == kNoiseMetricManhattan ? kCellularManhattanScale : kCellularChebyshevScale);

    float v = distance0;
    if (output == kNoiseCellularF2) {
        v = distance1;
    } else if (output == kNoiseCellularF2MinusF1) {
        v = distance1 - distance0;
    } else if (output == kNoiseCellularSmoothF1) {
        v = vmax(smooth, 0.0f);
    } else if (output == kNoiseCellularDistanceToEdge) {
        v = (distance1 - distance0) * 0.5f;
        if (euclidean) {
            float edge = kInfinity;
            index = 0;
            xcf = xcfBase;
            xc = xpBase;
            for (int xi = 0; xi < 3; xi++) {
                float ycf = ycfBase;
                uint yc = ypBase;
                for (int yi = 0; yi < 3; yi++) {
                    float zcf = zcfBase;
                    uint zc = zpBase;
                    for (int zi = 0; zi < 3; zi++) {
                        float wcf = wcfBase;
                        uint wc = wpBase;
                        for (int wi = 0; wi < 3; wi++) {
                            uint hash = hashPrimesHB(seed, xc, yc, zc, wc);
                            NoiseVec4 d = cellPoint(hash, jitter, xcf - p.fx, ycf - p.fy, zcf - p.fz, wcf - p.fw);
                            float b = bisectorDistance(nearest, d);
                            edge = selectF(index != nearestIndex, vmin(edge, b), edge);

                            index++;
                            wcf += 1.0f;
                            wc += kPrimeW;
                        }
                        zcf += 1.0f;
                        zc += kPrimeZ;
                    }
                    ycf += 1.0f;
                    yc += kPrimeY;
                }
                xcf += 1.0f;
                xc += kPrimeX;
            }
            v = edge;
        }
    }

    // The square roots may round past the bounds by an ulp.
    return vmin(muladd(v, scale, kRangeMin), 1.0f);
}

// ---- Engine/Nodes/Generator/NoiseCore.h ----

static const int kNoiseTypePerlin = 0;
static const int kNoiseTypeSimplex = 1; // SuperSimplex (OpenSimplex2S)
static const int kNoiseTypeValue = 2;
static const int kNoiseTypeCellular = 3;
static const int kNoiseTypeWhite = 4;

static const int kNoiseFractalNone = 0;
static const int kNoiseFractalFBm = 1;
static const int kNoiseFractalBillow = 2;
static const int kNoiseFractalRidged = 3;
static const int kNoiseFractalHybrid = 4;

static const int kNoiseWarpNone = 0;
static const int kNoiseWarpPerlin = 1;
static const int kNoiseWarpSimplex = 2;

static const int kNoiseMaxOctaves = 16;

static const uint kNoiseWarpSeed = 0x9E3779B9u;

// NoiseCore::NoiseParams, field for field (56 bytes).
struct NoiseParams {
    int type;
    uint seed;
    int fractal;
    int octaves;
    float lacunarity;
    float gain;
    float weightedStrength;
    int cellularMetric;
    int cellularOutput;
    float jitter;
    float smoothness;
    int warpType;
    float warpAmount;
    float warpFrequency;
};

float noiseBasis(NoiseParams prm,
                 int type,
                 uint seed,
                 NoiseCoord p)
{
    if (type == kNoiseTypeSimplex) {
        return superSimplex4(seed, p);
    }
    if (type == kNoiseTypeValue) {
        return value4(seed, p);
    }
    if (type == kNoiseTypeCellular) {
        return cellular4(seed, p, prm.cellularMetric, prm.cellularOutput, prm.jitter, prm.smoothness);
    }
    if (type == kNoiseTypeWhite) {
        return white4(seed, p);
    }
    return perlin4(seed, p);
}

NoiseCoord noiseWarp(NoiseParams prm,
                     NoiseCoord p)
{
    if ( (prm.warpType != kNoiseWarpPerlin) && (prm.warpType != kNoiseWarpSimplex) ) {
        return p;
    }
    NoiseCoord q = coordScale(p, prm.warpFrequency);
    float d[4];
    for (int i = 0; i < 4; i++) {
        uint seed = prm.seed + kNoiseWarpSeed * uint(i + 1);
        float n;
        if (prm.warpType == kNoiseWarpSimplex) {
            n = superSimplex4(seed, q);
        } else {
            n = perlin4(seed, q);
        }
        d[i] = n * prm.warpAmount;
    }

    return coordOffset(p, d[0], d[1], d[2], d[3]);
}

float noise4(NoiseParams prm,
             NoiseCoord p)
{
    p = noiseWarp(prm, p);
    if ( (prm.fractal != kNoiseFractalFBm) && (prm.fractal != kNoiseFractalBillow) &&
         (prm.fractal != kNoiseFractalRidged) && (prm.fractal != kNoiseFractalHybrid) ) {
        return noiseBasis(prm, prm.type, prm.seed, p);
    }

    int octaves = prm.octaves < 1 ? 1 : (prm.octaves > kNoiseMaxOctaves ? kNoiseMaxOctaves : prm.octaves);
    float gainBound = vabs(prm.gain);
    uint seed = prm.seed;
    float frequency = 1.0f;
    float amp = 1.0f;
    float bound = 0.0f;
    float boundAmp = 1.0f;
    float weight = 1.0f;
    float sum = 0.0f;
    for (int k = 0; k < octaves; k++) {
        NoiseCoord q = p;
        if (k > 0) {
            q = coordScale(p, frequency);
        }
        float n = noiseBasis(prm, prm.type, seed, q);
        if (prm.fractal == kNoiseFractalRidged) {
            float r = vabs(n);
            sum += (1.0f - 2.0f * r) * amp;
            amp *= lerpV(1.0f, 1.0f - r, prm.weightedStrength) * prm.gain;
        } else if (prm.fractal == kNoiseFractalHybrid) {
            float s = (n + 1.0f) * 0.5f;
            sum += amp * weight * s;
            weight = vmin(1.0f, weight * s * 2.0f);
            amp *= prm.gain;
        } else {
            if (prm.fractal == kNoiseFractalBillow) {
                n = vabs(n) * 2.0f - 1.0f;
            }
            sum += n * amp;
            amp *= lerpV(1.0f, (n + 1.0f) * 0.5f, prm.weightedStrength) * prm.gain;
        }
        bound += boundAmp;
        boundAmp *= gainBound;
        seed += 1u;
        frequency *= prm.lacunarity;
    }

    float v = sum / bound;
    if (prm.fractal == kNoiseFractalHybrid) {
        v = v * 2.0f - 1.0f;
    }

    return vmax(-1.0f, vmin(1.0f, v));
}

// ---- Test entry: noise4 at a list of points ----

// NoiseParams followed by the point count (60 bytes).
struct NoiseSampleParams {
    NoiseParams noise;
    uint count;
};

// Point i is eight words at 8 i: the cells cx, cy, cz, cw, then the fractions' bits.
[[vk::binding(0, 0)]] StructuredBuffer<uint> noiseSamplePoints;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> noiseSampleOut;
[[vk::push_constant]] ConstantBuffer<NoiseSampleParams> noiseSample;

[shader("compute")]
[numthreads(64, 1, 1)]
void noisecore_sample(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= noiseSample.count) {
        return;
    }
    uint base = i * 8u;
    NoiseCoord p;
    p.cx = noiseSamplePoints[base + 0u];
    p.cy = noiseSamplePoints[base + 1u];
    p.cz = noiseSamplePoints[base + 2u];
    p.cw = noiseSamplePoints[base + 3u];
    p.fx = asfloat(noiseSamplePoints[base + 4u]);
    p.fy = asfloat(noiseSamplePoints[base + 5u]);
    p.fz = asfloat(noiseSamplePoints[base + 6u]);
    p.fw = asfloat(noiseSamplePoints[base + 7u]);
    noiseSampleOut[i] = noise4(noiseSample.noise, p);
}
