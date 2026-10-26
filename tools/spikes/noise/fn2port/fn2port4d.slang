// Slang mirror of fn2port4d.h: FastNoise2's 4D Perlin, SuperSimplex, CellularDistance (Euclidean,
// F1) and FractalFBm, ported from Auburn/FastNoise2 721400f9560831be2fc9790be9b72c44bee7d33f.
// MIT, see LICENSE in this directory.
//
// Compile with -fp-mode precise so nothing is contracted or reassociated. FN2P_FUSED=1 maps
// FS::FMulAdd/FNMulAdd to fma(), 0 (the default) to a separate multiply and add. Float constants are
// spelled as bit patterns: each is the float32 rounding of the double expression the C++ uses, and
// fn2port4d.h static_asserts the same values.
//
// Exactness against the CPU: integer, compare, select, add, sub and mul are exact in Vulkan.
// sqrt and division are not required to be correctly rounded, and fma() may lower to an unfused
// multiply-add on some devices, so the cellular (1/sqrt, sqrt) path and the fused variant can differ
// from the CPU by a few ulps.

#ifndef FN2P_FUSED
#define FN2P_FUSED 0
#endif

RWStructuredBuffer<float> dstBuf;

struct NoiseParams
{
    uint width;
    uint height;
    float originX;
    float originY;
    float frequency;
    float z;
    float w;
    uint seed;
    int octaves;
    float lacunarity;
    float gain;
    float weightedStrength;
    float gridJitter;
    int type; // 0 Perlin, 1 SuperSimplex, 2 Cellular F1
};
[[vk::push_constant]] ConstantBuffer<NoiseParams> params;

static const uint kPrimeX = 0xF797C5C7u;
static const uint kPrimeY = 0x6C060C89u;
static const uint kPrimeZ = 0x465FD04Fu;
static const uint kPrimeW = 0xF7A62279u;
static const uint kHashMultiplierA = 0xB7E0A5F5u;
static const uint kSignBit = 0x80000000u;

static const float kRangeMin = -1.0f;

float muladd(float a, float b, float c)
{
#if FN2P_FUSED
    return fma(a, b, c);
#else
    return a * b + c;
#endif
}

float nmuladd(float a, float b, float c)
{
#if FN2P_FUSED
    return fma(-a, b, c);
#else
    return c - a * b;
#endif
}

float vmax(float a, float b) { return a > b ? a : b; }
float vmin(float a, float b) { return a < b ? a : b; }

float xorBits(float a, uint bits) { return asfloat(asuint(a) ^ bits); }

uint sra(uint a, int n) { return uint(int(a) >> n); }

float masked(bool m, float a) { return m ? a : 0.0f; }
float maskedIncrement(bool m, float a) { return a - masked(m, -1.0f); }
float maskedDecrement(bool m, float a) { return a + masked(m, -1.0f); }

float roundEven(float a)
{
    uint magnitude = abs(a) < 8388608.0f ? 0x4B000000u : 0u;
    float c = asfloat((asuint(a) & kSignBit) | magnitude);
    return (a + c) - c;
}

float floorV(float a)
{
    float r = roundEven(a);
    return maskedDecrement(r > a, r);
}

uint toInt(float a) { return uint(int(roundEven(a))); }
float toFloat(uint a) { return float(int(a)); }

float invSqrt(float a) { return 1.0f / sqrt(a); }

uint hashPrimes(uint seed, uint x, uint y, uint z, uint w)
{
    uint hash = seed;
    hash ^= x ^ (y ^ (z ^ w));

    hash *= kHashMultiplierA;

    return sra(hash, 15) ^ hash;
}

uint hashPrimesHB(uint seed, uint x, uint y, uint z, uint w)
{
    uint hash = seed;
    hash ^= x ^ (y ^ (z ^ w));

    hash *= kHashMultiplierA;
    return hash;
}

float lerpV(float a, float b, float t)
{
    return muladd(t, b - a, a);
}

float interpQuintic(float t)
{
    return t * t * t * muladd(t, muladd(t, 6.0f, -15.0f), 10.0f);
}

float getGradientDotPerlin(uint hash, float fX, float fY, float fZ, float fW)
{
    uint p = hash & (3u << 3);

    float a = select(int(p) > 0, fX, fY);
    float b = select(int(hash << 27) < 0, fY, fZ);
    float c = select(int(p) > (2 << 3), fZ, fW);

    uint aSign = hash << 31;
    uint bSign = (hash >> 1) << 31;
    uint cSign = (hash >> 2) << 31;

    return xorBits(a, aSign) + xorBits(b, bSign) + xorBits(c, cSign);
}

// VariableRange::ScaleOutput with the default range; invRangeTimesScale is
// float(1.0f / (nativeMax - nativeMin)) * 2.
float scaleOutput(float value, float nativeMin, float invRangeTimesScale)
{
    return muladd(invRangeTimesScale, value - nativeMin, kRangeMin);
}

static const float kPerlinBounding = asfloat(0x3fab56cbu); // 1.33858621
static const float kPerlinInvRange = asfloat(0x3f3f3f1eu); // 0.747056842

float perlin4(uint seed, float x, float y, float z, float w, float frequency)
{
    x *= frequency;
    y *= frequency;
    z *= frequency;
    w *= frequency;

    float xs = floorV(x);
    float ys = floorV(y);
    float zs = floorV(z);
    float ws = floorV(w);

    uint x0 = toInt(xs) * kPrimeX;
    uint y0 = toInt(ys) * kPrimeY;
    uint z0 = toInt(zs) * kPrimeZ;
    uint w0 = toInt(ws) * kPrimeW;
    uint x1 = x0 + kPrimeX;
    uint y1 = y0 + kPrimeY;
    uint z1 = z0 + kPrimeZ;
    uint w1 = w0 + kPrimeW;

    xs = x - xs;
    ys = y - ys;
    zs = z - zs;
    ws = w - ws;
    float xf0 = xs;
    float yf0 = ys;
    float zf0 = zs;
    float wf0 = ws;
    float xf1 = xf0 - 1.0f;
    float yf1 = yf0 - 1.0f;
    float zf1 = zf0 - 1.0f;
    float wf1 = wf0 - 1.0f;

    xs = interpQuintic(xs);
    ys = interpQuintic(ys);
    zs = interpQuintic(zs);
    ws = interpQuintic(ws);

    float value = lerpV( lerpV( lerpV(
        lerpV( getGradientDotPerlin( hashPrimes( seed, x0, y0, z0, w0 ), xf0, yf0, zf0, wf0 ), getGradientDotPerlin( hashPrimes( seed, x1, y0, z0, w0 ), xf1, yf0, zf0, wf0 ), xs ),
        lerpV( getGradientDotPerlin( hashPrimes( seed, x0, y1, z0, w0 ), xf0, yf1, zf0, wf0 ), getGradientDotPerlin( hashPrimes( seed, x1, y1, z0, w0 ), xf1, yf1, zf0, wf0 ), xs ), ys ),
        lerpV(
        lerpV( getGradientDotPerlin( hashPrimes( seed, x0, y0, z1, w0 ), xf0, yf0, zf1, wf0 ), getGradientDotPerlin( hashPrimes( seed, x1, y0, z1, w0 ), xf1, yf0, zf1, wf0 ), xs ),
        lerpV( getGradientDotPerlin( hashPrimes( seed, x0, y1, z1, w0 ), xf0, yf1, zf1, wf0 ), getGradientDotPerlin( hashPrimes( seed, x1, y1, z1, w0 ), xf1, yf1, zf1, wf0 ), xs ), ys ), zs ),
        lerpV( lerpV(
        lerpV( getGradientDotPerlin( hashPrimes( seed, x0, y0, z0, w1 ), xf0, yf0, zf0, wf1 ), getGradientDotPerlin( hashPrimes( seed, x1, y0, z0, w1 ), xf1, yf0, zf0, wf1 ), xs ),
        lerpV( getGradientDotPerlin( hashPrimes( seed, x0, y1, z0, w1 ), xf0, yf1, zf0, wf1 ), getGradientDotPerlin( hashPrimes( seed, x1, y1, z0, w1 ), xf1, yf1, zf0, wf1 ), xs ), ys ),
        lerpV(
        lerpV( getGradientDotPerlin( hashPrimes( seed, x0, y0, z1, w1 ), xf0, yf0, zf1, wf1 ), getGradientDotPerlin( hashPrimes( seed, x1, y0, z1, w1 ), xf1, yf0, zf1, wf1 ), xs ),
        lerpV( getGradientDotPerlin( hashPrimes( seed, x0, y1, z1, w1 ), xf0, yf1, zf1, wf1 ), getGradientDotPerlin( hashPrimes( seed, x1, y1, z1, w1 ), xf1, yf1, zf1, wf1 ), xs ), ys ), zs ), ws );

    return scaleOutput(value, -kPerlinBounding, kPerlinInvRange);
}

static const float kSkew4 = asfloat(0x3e9e377au);           // 0.309017003
static const float kTwiceUnskew4 = asfloat(0xbe4ccccdu);    // -0.200000003
static const float kUnskew4 = asfloat(0xbe0d8369u);         // -0.138196602
static const float kFalloffRadiusSquared = asfloat(0x3f4ccccdu); // 0.800000012
static const float kDistanceSquaredAHalf = asfloat(0x3ecccccdu); // 0.400000006
static const float kDistanceSquaredBHalf = asfloat(0x3f19999au); // 0.600000024
static const float kUnskew4x4p1 = asfloat(0x3ee4f92eu);     // 1 + 4u = 0.44721359
static const float kUnskew4x3p1 = asfloat(0x3f15dd71u);     // 3u + 1 = 0.585410178
static const float kUnskew4x3 = asfloat(0xbed4451du);       // 3u = -0.414589792
static const float kUnskew4x2p1 = asfloat(0x3f393e4cu);     // 2u + 1 = 0.723606825
static const float kUnskew4x2 = asfloat(0xbe8d8369u);       // 2u = -0.276393205
static const float kUnskew4p1 = asfloat(0x3f5c9f26u);       // u + 1 = 0.861803412
static const float kSimplexBounding = asfloat(0x3c394da3u); // 0.0113100139
static const float kSimplexInvRange = asfloat(0x42b0d59eu); // 88.4172211

void considerVertex(inout float maxScore, inout uint moveMaskBits, float score, uint bits)
{
    moveMaskBits = select(score > maxScore, bits, moveMaskBits);
    maxScore = vmax(maxScore, score);
}

float falloff4(float f) { return (f * f) * (f * f); }

float superSimplex4(uint seed, float x, float y, float z, float w, float frequency)
{
    x *= frequency;
    y *= frequency;
    z *= frequency;
    w *= frequency;

    float skewDelta = kSkew4 * (x + y + z + w);

    float xSkewed = x + skewDelta;
    float ySkewed = y + skewDelta;
    float zSkewed = z + skewDelta;
    float wSkewed = w + skewDelta;
    float xSkewedBase = floorV(xSkewed);
    float ySkewedBase = floorV(ySkewed);
    float zSkewedBase = floorV(zSkewed);
    float wSkewedBase = floorV(wSkewed);
    float dxSkewed = xSkewed - xSkewedBase;
    float dySkewed = ySkewed - ySkewedBase;
    float dzSkewed = zSkewed - zSkewedBase;
    float dwSkewed = wSkewed - wSkewedBase;

    // From unit cell base, find closest vertex
    {
        float twiceUnskewDelta = kTwiceUnskew4 * (dxSkewed + dySkewed + dzSkewed + dwSkewed);
        float xNormal = dxSkewed + twiceUnskewDelta;
        float yNormal = dySkewed + twiceUnskewDelta;
        float zNormal = dzSkewed + twiceUnskewDelta;
        float wNormal = dwSkewed + twiceUnskewDelta;
        float xyzwNormal = -twiceUnskewDelta;

        float maxScore = 0.6f - xyzwNormal;
        uint moveMaskBits = 0.2f > maxScore ? 0xFFFFFFFFu : 0u;
        maxScore = vmax(maxScore, 0.2f);
        considerVertex(maxScore, moveMaskBits, -wNormal, 7u);   // 0b0111
        considerVertex(maxScore, moveMaskBits, -zNormal, 11u);  // 0b1011
        considerVertex(maxScore, moveMaskBits, -yNormal, 13u);  // 0b1101
        considerVertex(maxScore, moveMaskBits, -xNormal, 14u);  // 0b1110
        maxScore += xyzwNormal - 0.2f;
        considerVertex(maxScore, moveMaskBits, xNormal, 1u);
        considerVertex(maxScore, moveMaskBits, yNormal, 2u);
        considerVertex(maxScore, moveMaskBits, zNormal, 4u);
        considerVertex(maxScore, moveMaskBits, wNormal, 8u);
        maxScore += 0.2f - xNormal;
        considerVertex(maxScore, moveMaskBits, yNormal, 3u);    // 0b0011
        considerVertex(maxScore, moveMaskBits, zNormal, 5u);    // 0b0101
        considerVertex(maxScore, moveMaskBits, wNormal, 9u);    // 0b1001
        maxScore += xNormal;
        considerVertex(maxScore, moveMaskBits, yNormal + zNormal, 6u); // 0b0110
        maxScore -= wNormal;
        considerVertex(maxScore, moveMaskBits, yNormal, 10u);   // 0b1010
        considerVertex(maxScore, moveMaskBits, zNormal, 12u);   // 0b1100

        bool moveX = (moveMaskBits & 1u) != 0u;
        bool moveY = (moveMaskBits & 2u) != 0u;
        bool moveZ = (moveMaskBits & 4u) != 0u;
        bool moveW = (moveMaskBits & 8u) != 0u;

        xSkewedBase = maskedIncrement(moveX, xSkewedBase);
        ySkewedBase = maskedIncrement(moveY, ySkewedBase);
        zSkewedBase = maskedIncrement(moveZ, zSkewedBase);
        wSkewedBase = maskedIncrement(moveW, wSkewedBase);

        dxSkewed = maskedDecrement(moveX, dxSkewed);
        dySkewed = maskedDecrement(moveY, dySkewed);
        dzSkewed = maskedDecrement(moveZ, dzSkewed);
        dwSkewed = maskedDecrement(moveW, dwSkewed);
    }

    uint xPrimedBase = toInt(xSkewedBase) * kPrimeX;
    uint yPrimedBase = toInt(ySkewedBase) * kPrimeY;
    uint zPrimedBase = toInt(zSkewedBase) * kPrimeZ;
    uint wPrimedBase = toInt(wSkewedBase) * kPrimeW;

    float skewedCoordinateSum = dxSkewed + dySkewed + dzSkewed + dwSkewed;
    float twiceUnskewDelta = kTwiceUnskew4 * skewedCoordinateSum;
    float xNormal = dxSkewed + twiceUnskewDelta;
    float yNormal = dySkewed + twiceUnskewDelta;
    float zNormal = dzSkewed + twiceUnskewDelta;
    float wNormal = dwSkewed + twiceUnskewDelta;
    float xyzwNormal = -twiceUnskewDelta;

    float unskewDelta = kUnskew4 * skewedCoordinateSum;
    float dxBase = dxSkewed + unskewDelta;
    float dyBase = dySkewed + unskewDelta;
    float dzBase = dzSkewed + unskewDelta;
    float dwBase = dwSkewed + unskewDelta;

    float coordinateSum = kUnskew4x4p1 * skewedCoordinateSum;

    // Vertex <0, 0, 0, 0>
    float value, falloffBaseStemA, falloffBaseStemB;
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
        uint sign = signMask ? kSignBit : 0u;

        uint xPrimed = xPrimedBase + select(signMask, 0u - kPrimeX, kPrimeX);
        uint yPrimed = yPrimedBase + select(signMask, 0u - kPrimeY, kPrimeY);
        uint zPrimed = zPrimedBase + select(signMask, 0u - kPrimeZ, kPrimeZ);
        uint wPrimed = wPrimedBase + select(signMask, 0u - kPrimeW, kPrimeW);

        float offset = xorBits(kUnskew4x4p1, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimed, zPrimed, wPrimed), dxBase - offset, dyBase - offset, dzBase - offset, dwBase - offset);
        float falloffBase = vmax(muladd(offset, coordinateSum, falloffBaseStemA), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <1, 1, 1, 0> or <-1, -1, -1, 0>
    {
        bool signMask = xyzwNormal < wNormal;
        uint sign = signMask ? kSignBit : 0u;

        uint xPrimed = xPrimedBase + select(signMask, 0u - kPrimeX, kPrimeX);
        uint yPrimed = yPrimedBase + select(signMask, 0u - kPrimeY, kPrimeY);
        uint zPrimed = zPrimedBase + select(signMask, 0u - kPrimeZ, kPrimeZ);

        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimed, zPrimed, wPrimedBase), dxBase - offset1, dyBase - offset1, dzBase - offset1, dwBase - offset0);
        float falloffBase = vmax(muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dwBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <1, 1, 0, 1> or <-1, -1, 0, -1>
    {
        bool signMask = xyzwNormal < zNormal;
        uint sign = signMask ? kSignBit : 0u;

        uint xPrimed = xPrimedBase + select(signMask, 0u - kPrimeX, kPrimeX);
        uint yPrimed = yPrimedBase + select(signMask, 0u - kPrimeY, kPrimeY);
        uint wPrimed = wPrimedBase + select(signMask, 0u - kPrimeW, kPrimeW);

        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimed, zPrimedBase, wPrimed), dxBase - offset1, dyBase - offset1, dzBase - offset0, dwBase - offset1);
        float falloffBase = vmax(muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dzBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <1, 0, 1, 1> or <-1, 0, -1, -1>
    {
        bool signMask = xyzwNormal < yNormal;
        uint sign = signMask ? kSignBit : 0u;

        uint xPrimed = xPrimedBase + select(signMask, 0u - kPrimeX, kPrimeX);
        uint zPrimed = zPrimedBase + select(signMask, 0u - kPrimeZ, kPrimeZ);
        uint wPrimed = wPrimedBase + select(signMask, 0u - kPrimeW, kPrimeW);

        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimedBase, zPrimed, wPrimed), dxBase - offset1, dyBase - offset0, dzBase - offset1, dwBase - offset1);
        float falloffBase = vmax(muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dyBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <0, 1, 1, 1> or <0, -1, -1, -1>
    {
        bool signMask = xyzwNormal < xNormal;
        uint sign = signMask ? kSignBit : 0u;

        uint yPrimed = yPrimedBase + select(signMask, 0u - kPrimeY, kPrimeY);
        uint zPrimed = zPrimedBase + select(signMask, 0u - kPrimeZ, kPrimeZ);
        uint wPrimed = wPrimedBase + select(signMask, 0u - kPrimeW, kPrimeW);

        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimed, zPrimed, wPrimed), dxBase - offset0, dyBase - offset1, dzBase - offset1, dwBase - offset1);
        float falloffBase = vmax(muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dxBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <1, 0, 0, 0> or <-1, 0, 0, 0>
    {
        bool signMask = xNormal < 0.0f;
        uint sign = signMask ? kSignBit : 0u;

        uint xPrimed = xPrimedBase + select(signMask, 0u - kPrimeX, kPrimeX);

        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimedBase, zPrimedBase, wPrimedBase), dxBase - offset1, dyBase - offset0, dzBase - offset0, dwBase - offset0);
        float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dxBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <1, 1, 0, 0> or <-1, -1, 0, 0>
    {
        bool signMask = xNormal < -yNormal;
        uint sign = signMask ? kSignBit : 0u;

        uint xPrimed = xPrimedBase + select(signMask, 0u - kPrimeX, kPrimeX);
        uint yPrimed = yPrimedBase + select(signMask, 0u - kPrimeY, kPrimeY);

        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimed, zPrimedBase, wPrimedBase), dxBase - offset1, dyBase - offset1, dzBase - offset0, dwBase - offset0);
        float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dyBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <1, 0, 1, 0> or <-1, 0, -1, 0>
    {
        bool signMask = xNormal < -zNormal;
        uint sign = signMask ? kSignBit : 0u;

        uint xPrimed = xPrimedBase + select(signMask, 0u - kPrimeX, kPrimeX);
        uint zPrimed = zPrimedBase + select(signMask, 0u - kPrimeZ, kPrimeZ);

        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimedBase, zPrimed, wPrimedBase), dxBase - offset1, dyBase - offset0, dzBase - offset1, dwBase - offset0);
        float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dzBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <1, 0, 0, 1> or <-1, 0, 0, -1>
    {
        bool signMask = xNormal < -wNormal;
        uint sign = signMask ? kSignBit : 0u;

        uint xPrimed = xPrimedBase + select(signMask, 0u - kPrimeX, kPrimeX);
        uint wPrimed = wPrimedBase + select(signMask, 0u - kPrimeW, kPrimeW);

        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimedBase, zPrimedBase, wPrimed), dxBase - offset1, dyBase - offset0, dzBase - offset0, dwBase - offset1);
        float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dwBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <0, 1, 0, 0> or <0, -1, 0, 0>
    {
        bool signMask = yNormal < 0.0f;
        uint sign = signMask ? kSignBit : 0u;

        uint yPrimed = yPrimedBase + select(signMask, 0u - kPrimeY, kPrimeY);

        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimed, zPrimedBase, wPrimedBase), dxBase - offset0, dyBase - offset1, dzBase - offset0, dwBase - offset0);
        float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dyBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <0, 1, 1, 0> or <0, -1, -1, 0>
    {
        bool signMask = yNormal < -zNormal;
        uint sign = signMask ? kSignBit : 0u;

        uint yPrimed = yPrimedBase + select(signMask, 0u - kPrimeY, kPrimeY);
        uint zPrimed = zPrimedBase + select(signMask, 0u - kPrimeZ, kPrimeZ);

        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimed, zPrimed, wPrimedBase), dxBase - offset0, dyBase - offset1, dzBase - offset1, dwBase - offset0);
        float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dyBase + dzBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <0, 1, 0, 1> or <0, -1, 0, -1>
    {
        bool signMask = yNormal < -wNormal;
        uint sign = signMask ? kSignBit : 0u;

        uint yPrimed = yPrimedBase + select(signMask, 0u - kPrimeY, kPrimeY);
        uint wPrimed = wPrimedBase + select(signMask, 0u - kPrimeW, kPrimeW);

        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimed, zPrimedBase, wPrimed), dxBase - offset0, dyBase - offset1, dzBase - offset0, dwBase - offset1);
        float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dyBase + dwBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <0, 0, 1, 0> or <0, 0, -1, 0>
    {
        bool signMask = zNormal < 0.0f;
        uint sign = signMask ? kSignBit : 0u;

        uint zPrimed = zPrimedBase + select(signMask, 0u - kPrimeZ, kPrimeZ);

        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimedBase, zPrimed, wPrimedBase), dxBase - offset0, dyBase - offset0, dzBase - offset1, dwBase - offset0);
        float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dzBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <0, 0, 1, 1> or <0, 0, -1, -1>
    {
        bool signMask = zNormal < -wNormal;
        uint sign = signMask ? kSignBit : 0u;

        uint zPrimed = zPrimedBase + select(signMask, 0u - kPrimeZ, kPrimeZ);
        uint wPrimed = wPrimedBase + select(signMask, 0u - kPrimeW, kPrimeW);

        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimedBase, zPrimed, wPrimed), dxBase - offset0, dyBase - offset0, dzBase - offset1, dwBase - offset1);
        float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dzBase + dwBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    // Vertex <0, 0, 0, 1> or <0, 0, 0, -1>
    {
        bool signMask = wNormal < 0.0f;
        uint sign = signMask ? kSignBit : 0u;

        uint wPrimed = wPrimedBase + select(signMask, 0u - kPrimeW, kPrimeW);

        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);

        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimedBase, zPrimedBase, wPrimed), dxBase - offset0, dyBase - offset0, dzBase - offset0, dwBase - offset1);
        float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dwBase, sign), 0.0f);
        value = muladd(falloff4(falloffBase), gradientRampValue, value);
    }

    return scaleOutput(value, -kSimplexBounding, kSimplexInvRange);
}

static const float kJitter4D = asfloat(0x3ebb67a1u);        // 0.366025
static const float kCellularInvRange = asfloat(0x3f8930a8u); // 2 / (1 + 0.366025)^2
static const float kHalfByte = 127.5f;

float cellularF1(uint seed, float x, float y, float z, float w, float frequency, float gridJitter)
{
    float jitter = kJitter4D * gridJitter;

    float distance0 = asfloat(0x7f800000u);

    x *= frequency;
    y *= frequency;
    z *= frequency;
    w *= frequency;

    uint xc = toInt(x) + 0xFFFFFFFFu;
    uint ycBase = toInt(y) + 0xFFFFFFFFu;
    uint zcBase = toInt(z) + 0xFFFFFFFFu;
    uint wcBase = toInt(w) + 0xFFFFFFFFu;

    float xcf = toFloat(xc);
    float ycfBase = toFloat(ycBase);
    float zcfBase = toFloat(zcBase);
    float wcfBase = toFloat(wcBase);

    xc *= kPrimeX;
    ycBase *= kPrimeY;
    zcBase *= kPrimeZ;
    wcBase *= kPrimeW;

    for (int xi = 0; xi < 3; xi++)
    {
        float xcfOffset = xcf - x;
        float ycf = ycfBase;
        uint yc = ycBase;
        for (int yi = 0; yi < 3; yi++)
        {
            float ycfOffset = ycf - y;
            float zcf = zcfBase;
            uint zc = zcBase;
            for (int zi = 0; zi < 3; zi++)
            {
                float zcfOffset = zcf - z;
                float wcf = wcfBase;
                uint wc = wcBase;
                for (int wi = 0; wi < 3; wi++)
                {
                    uint hash = hashPrimesHB(seed, xc, yc, zc, wc);
                    float xd = toFloat(hash & 0xffu) - kHalfByte;
                    float yd = toFloat((hash >> 8) & 0xffu) - kHalfByte;
                    float zd = toFloat((hash >> 16) & 0xffu) - kHalfByte;
                    float wd = toFloat(hash >> 24) - kHalfByte;

                    float invMag = jitter * invSqrt(muladd(xd, xd, muladd(yd, yd, muladd(zd, zd, wd * wd))));
                    xd = muladd(xd, invMag, xcfOffset);
                    yd = muladd(yd, invMag, ycfOffset);
                    zd = muladd(zd, invMag, zcfOffset);
                    wd = muladd(wd, invMag, wcf - w);

                    float newDistance = xd * xd;
                    newDistance = muladd(yd, yd, newDistance);
                    newDistance = muladd(zd, zd, newDistance);
                    newDistance = muladd(wd, wd, newDistance);

                    distance0 = vmin(distance0, newDistance);

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

    distance0 = sqrt(distance0);
    return scaleOutput(distance0, 0.0f, kCellularInvRange);
}

float source(int type, uint seed, float x, float y, float z, float w)
{
    if (type == 0)
        return perlin4(seed, x, y, z, w, params.frequency);
    if (type == 1)
        return superSimplex4(seed, x, y, z, w, params.frequency);
    return cellularF1(seed, x, y, z, w, params.frequency, params.gridJitter);
}

float fbm(uint seed, float x, float y, float z, float w)
{
    int type = params.type;
    if (params.octaves <= 1)
        return source(type, seed, x, y, z, w);

    float amp = 1.0f;
    float noise = source(type, seed, x, y, z, w);
    float sum = noise * amp;

    for (int i = 1; i < params.octaves; i++)
    {
        seed -= 0xFFFFFFFFu;
        amp *= lerpV(1.0f, (noise + 1.0f) * 0.5f, params.weightedStrength);
        amp *= params.gain;

        x *= params.lacunarity;
        y *= params.lacunarity;
        z *= params.lacunarity;
        w *= params.lacunarity;
        noise = source(type, seed, x, y, z, w);
        sum += noise * amp;
    }
    return sum;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    float x = params.originX + (float(tid.x) + 0.5f);
    float y = params.originY + (float(tid.y) + 0.5f);
    dstBuf[tid.y * params.width + tid.x] = fbm(params.seed, x, y, params.z, params.w);
}
