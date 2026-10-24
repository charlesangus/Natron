import pixelwrap;

// RawColorCalibration: RawColorCalibrationMath::processPixel(), operation for operation in the
// CPU's order. Every step is precise, since a contracted or reassociated step would break parity
// with the CPU; std::fmax(), std::fmin(), comparisons that may see a NaN and std::pow() are spelt
// out below, because Vulkan leaves their NaN and edge cases to the device.

static const uint kAdaptationLinearBradford = 0;
static const uint kAdaptationCat16 = 1;
static const uint kAdaptationFullBradford = 2;
static const uint kAdaptationXyz = 3;

static const uint kVersion1 = 0;
static const uint kVersion3 = 2;

// The CPU's constants as bit patterns, so that no literal parsing stands between the two sides;
// D50's x and y are doubles there, narrowed to float.
static const float kNormMin = asfloat(0x37800000u);
static const float kD50x = asfloat(0x3eb0fba9u);
static const float kD50y = asfloat(0x3eb78d50u);
static const float kLuvD50u = asfloat(0x3e562dd1u);
static const float kLuvD50v = asfloat(0x3ef9e502u);
static const float kInvSqrt3 = asfloat(0x3f13cd3au);
static const float kFltMax = asfloat(0x7f7fffffu);
static const float kInf = asfloat(0x7f800000u);
static const float kNan = asfloat(0x7fc00000u);

bool ccalIsNan(float a)
{
    return (asuint(a) & 0x7fffffffu) > 0x7f800000u;
}

bool ccalIsInf(float a)
{
    return (asuint(a) & 0x7fffffffu) == 0x7f800000u;
}

bool ccalGreater(float a, float b)
{
    return !ccalIsNan(a) && !ccalIsNan(b) && a > b;
}

bool ccalLess(float a, float b)
{
    return !ccalIsNan(a) && !ccalIsNan(b) && a < b;
}

// glibc's fmaxf()/fminf(): a NaN operand yields the other one, and of two equal operands the first.
float ccalFmax(float a, float b)
{
    if (ccalIsNan(a))
        return b;
    if (ccalIsNan(b))
        return a;
    return a >= b ? a : b;
}

float ccalFmin(float a, float b)
{
    if (ccalIsNan(a))
        return b;
    if (ccalIsNan(b))
        return a;
    return a <= b ? a : b;
}

float3 ccalClipNegNan(float3 v)
{
    return float3(ccalFmax(v.x, 0.0), ccalFmax(v.y, 0.0), ccalFmax(v.z, 0.0));
}

// A float pair hi + lo, |lo| below half an ulp of hi, for the extended-precision steps of ccalPow().
struct FF
{
    float hi;
    float lo;
}

FF ffTwoSum(float a, float b)
{
    precise float s = a + b;
    precise float bb = s - a;
    precise float e = (a - (s - bb)) + (b - bb);
    FF r;
    r.hi = s;
    r.lo = e;
    return r;
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

// Dekker's exact product, as Vulkan does not require fma() to round once (lavapipe does not).
FF ffTwoProd(float a, float b)
{
    precise float p = a * b;
    float ah;
    float al;
    float bh;
    float bl;
    ffSplit(a, ah, al);
    ffSplit(b, bh, bl);
    precise float e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    FF r;
    r.hi = p;
    r.lo = e;
    return r;
}

FF ffAdd(FF a, FF b)
{
    const FF s = ffTwoSum(a.hi, b.hi);
    precise float lo = s.lo + (a.lo + b.lo);
    return ffTwoSum(s.hi, lo);
}

static const float kLn2Hi = asfloat(0x3f317218u);
static const float kLn2Lo = asfloat(0xb102e308u);

// x^y for a finite x > 0 and a finite y, within about an ulp of glibc's powf(), which SPIR-V Pow
// (exp2(y * log2(x)) at the device's precision) is not: ln x and y ln x are carried in float pairs.
float ccalPowPositive(float x, float y)
{
    int e = 0;
    float xn = x;
    if (asuint(x) < 0x00800000u)
    {
        precise float scaled = x * 16777216.0;
        xn = scaled;
        e = -24;
    }
    const uint u = asuint(xn);
    e += int(u >> 23) - 127;
    float m = asfloat((u & 0x007fffffu) | 0x3f800000u);
    if (m > 1.41421356)
    {
        precise float half = m * 0.5;
        m = half;
        e += 1;
    }

    // ln m = 2 atanh(s), s = (m - 1) / (m + 1), |s| < 0.172; m - 1 is exact.
    precise float f = m - 1.0;
    const FF d = ffTwoSum(m, 1.0);
    precise float sh = f / d.hi;
    const FF q = ffTwoProd(sh, d.hi);
    precise float rem = ((f - q.hi) - q.lo) - sh * d.lo;
    precise float sl = rem / d.hi;
    precise float t = sh * sh;
    precise float series = 2.0 / 3.0 + t * (2.0 / 5.0 + t * (2.0 / 7.0 + t * (2.0 / 9.0 + t * (2.0 / 11.0 + t * (2.0 / 13.0)))));
    precise float tail = sh * t * series;
    FF lnM = ffTwoSum(2.0 * sh, tail);
    precise float lnMLo = lnM.lo + 2.0 * sl;
    lnM.lo = lnMLo;

    const float ef = float(e);
    FF eLn2 = ffTwoProd(ef, kLn2Hi);
    precise float eLn2Lo = eLn2.lo + ef * kLn2Lo;
    eLn2.lo = eLn2Lo;
    const FF lnX = ffAdd(eLn2, lnM);

    FF z = ffTwoProd(y, lnX.hi);
    precise float zLo = z.lo + y * lnX.lo;
    z.lo = zLo;

    if (z.hi > 88.75)
        return kInf;
    if (z.hi < -104.0)
        return 0.0;

    precise float kArg = z.hi * 1.44269504 + 0.5;
    precise float kf = floor(kArg);
    const FF kLn2 = ffTwoProd(kf, kLn2Hi);
    precise float rh0 = z.hi - kLn2.hi;
    precise float rlRaw = (z.lo - kLn2.lo) - kf * kLn2Lo;
    // The low part can reach several ulps of rh, too much to leave out of the polynomial's terms.
    const FF r = ffTwoSum(rh0, rlRaw);
    const float rh = r.hi;
    const float rl = r.lo;
    precise float poly = 1.0 / 2.0 + rh * (1.0 / 6.0 + rh * (1.0 / 24.0 + rh * (1.0 / 120.0 + rh * (1.0 / 720.0 + rh * (1.0 / 5040.0 + rh * (1.0 / 40320.0))))));
    precise float expm1 = rh + (rl + rh * rh * poly);
    precise float mant = 1.0 + expm1;

    // 2^k in two factors, each a normal float for every k the bounds above let through.
    const int k = int(kf);
    const int k1 = k / 2;
    const int k2 = k - k1;
    precise float result = (mant * asfloat(uint(k1 + 127) << 23)) * asfloat(uint(k2 + 127) << 23);
    return result;
}

// std::pow(x, y) for the y the parameter block carries, with C's special cases.
float ccalPow(float x, float y)
{
    if (y == 0.0 || x == 1.0)
        return 1.0;
    if (y == 1.0)
        return x;
    if (ccalIsNan(x) || ccalIsNan(y))
        return kNan;
    const float ax = abs(x);
    if (ccalIsInf(y))
    {
        if (ax == 1.0)
            return 1.0;
        return ((ax > 1.0) == (y > 0.0)) ? kInf : 0.0;
    }
    const bool yInteger = floor(y) == y;
    precise float halfY = y * 0.5;
    const bool yOdd = yInteger && (floor(halfY) != halfY);
    if (x == 0.0)
    {
        if (y > 0.0)
            return yOdd ? x : 0.0;
        precise float pole = 1.0 / x;
        return yOdd ? pole : kInf;
    }
    if (x < 0.0 && !yInteger)
        return kNan;
    const float r = ccalIsInf(ax) ? ((y > 0.0) ? kInf : 0.0) : ccalPowPositive(ax, y);
    return (x < 0.0 && yOdd) ? -r : r;
}

float3 ccalApply(float m[9], float3 v)
{
    precise float a = m[0] * v.x + m[1] * v.y + m[2] * v.z;
    precise float b = m[3] * v.x + m[4] * v.y + m[5] * v.z;
    precise float c = m[6] * v.x + m[7] * v.y + m[8] * v.z;
    return float3(a, b, c);
}

float3 ccalDownscale(float3 v, float scaling)
{
    if (ccalGreater(scaling, kNormMin))
    {
        precise float d = scaling + kNormMin;
        precise float3 r = float3(v.x / d, v.y / d, v.z / d);
        return r;
    }
    precise float3 r = float3(v.x / kNormMin, v.y / kNormMin, v.z / kNormMin);
    return r;
}

float3 ccalUpscale(float3 v, float scaling)
{
    if (ccalGreater(scaling, kNormMin))
    {
        precise float d = scaling + kNormMin;
        precise float3 r = float3(v.x * d, v.y * d, v.z * d);
        return r;
    }
    precise float3 r = float3(v.x * kNormMin, v.y * kNormMin, v.z * kNormMin);
    return r;
}

float ccalDot(float3 a, float3 b)
{
    precise float acc = 0.0;
    acc = acc + a.x * b.x;
    acc = acc + a.y * b.y;
    acc = acc + a.z * b.z;
    return acc;
}

float ccalNorm(float3 v)
{
    precise float sq = v.x * v.x + v.y * v.y + v.z * v.z;
    precise float n = sqrt(sq);
    return ccalFmax(n, kNormMin);
}

struct CalibrationOp : IPixelOp
{
    float rgbToXyz[9];
    float xyzToRgb[9];
    float rgbToLms[9];
    float mixToXyz[9];
    float xyzToLms[9];
    float lmsToXyz[9];
    float3 illuminant;
    float3 d50;
    float3 saturation;
    float3 lightness;
    float3 grey;
    float p;
    float gamut;
    uint adaptation;
    uint version;
    bool clip;
    bool applyGrey;

    // Parameter block, in words: the row-major RGB to XYZ, XYZ to RGB, RGB to LMS, mix to XYZ,
    // XYZ to LMS and LMS to XYZ matrices (9 each); the illuminant and D50 in LMS, the saturation,
    // lightness and grey rows (3 each); p, gamut, adaptation, version, clip, apply grey (1 each).
    [mutating] void load(StructuredBuffer<uint> params)
    {
        [ForceUnroll]
        for (uint i = 0; i < 9; ++i)
        {
            rgbToXyz[i] = asfloat(params[i]);
            xyzToRgb[i] = asfloat(params[9 + i]);
            rgbToLms[i] = asfloat(params[18 + i]);
            mixToXyz[i] = asfloat(params[27 + i]);
            xyzToLms[i] = asfloat(params[36 + i]);
            lmsToXyz[i] = asfloat(params[45 + i]);
        }
        [ForceUnroll]
        for (uint c = 0; c < 3; ++c)
        {
            illuminant[c] = asfloat(params[54 + c]);
            d50[c] = asfloat(params[57 + c]);
            saturation[c] = asfloat(params[60 + c]);
            lightness[c] = asfloat(params[63 + c]);
            grey[c] = asfloat(params[66 + c]);
        }
        p = asfloat(params[69]);
        gamut = asfloat(params[70]);
        adaptation = params[71];
        version = params[72];
        clip = params[73] != 0;
        applyGrey = params[74] != 0;
    }

    bool isLms()
    {
        return adaptation == kAdaptationLinearBradford || adaptation == kAdaptationCat16 || adaptation == kAdaptationFullBradford;
    }

    // convertAnyXyzToLms(): XYZ adaptation leaves the values as they are, infinities included.
    float3 xyzToAnyLms(float3 v)
    {
        return isLms() ? ccalApply(xyzToLms, v) : v;
    }

    float3 anyLmsToXyz(float3 v)
    {
        return isLms() ? ccalApply(lmsToXyz, v) : v;
    }

    // bradfordAdaptD50(); the exponent is skipped for a blue ratio that is not above zero.
    float3 bradfordAdapt(float3 lms, bool full)
    {
        precise float t0 = lms.x / illuminant.x;
        precise float t1 = lms.y / illuminant.y;
        precise float t2 = lms.z / illuminant.z;
        if (full && ccalGreater(t2, 0.0))
            t2 = ccalPow(t2, p);
        precise float3 r = float3(d50.x * t0, d50.y * t1, d50.z * t2);
        return r;
    }

    // cat16AdaptD50() with full adaptation, and xyzAdaptD50(): in * D50 / illuminant.
    float3 vonKries(float3 v)
    {
        precise float3 r = float3((v.x * d50.x) / illuminant.x, (v.y * d50.y) / illuminant.y, (v.z * d50.z) / illuminant.z);
        return r;
    }

    float3 gamutMapping(float3 input)
    {
        precise float sum = input.x + input.y + input.z;
        const float Y = input.y;
        const bool positive = ccalGreater(sum, 0.0);
        precise float x = positive ? input.x / sum : kD50x;
        precise float y = positive ? input.y / sum : kD50y;

        precise float uvDen = -2.0 * x + 12.0 * y + 3.0;
        precise float u = 4.0 * x / uvDen;
        precise float v = 9.0 * y / uvDen;
        precise float du = kLuvD50u - u;
        precise float dv = kLuvD50v - v;
        precise float delta = Y * (du * du + dv * dv);
        const float correction = (gamut == 0.0) ? 0.0 : ccalPow(delta, gamut);

        // The correction stops at D50, so that it never flips the hue to its opposite.
        precise float tu = correction * du + u;
        precise float tv = correction * dv + v;
        u = ccalGreater(u, kLuvD50u) ? ccalFmax(tu, kLuvD50u) : ccalFmin(tu, kLuvD50u);
        v = ccalGreater(v, kLuvD50v) ? ccalFmax(tv, kLuvD50v) : ccalFmin(tv, kLuvD50v);

        precise float xyDen = 6.0 * u - 16.0 * v + 12.0;
        precise float nx = 9.0 * u / xyDen;
        precise float ny = 4.0 * v / xyDen;
        if (clip)
        {
            nx = ccalFmax(nx, 0.0);
            ny = ccalFmax(ny, 0.0);
        }
        ny = ccalFmax(ny, kNormMin);
        precise float scale = nx + ny;
        if (!ccalIsNan(scale) && scale >= 1.0)
        {
            precise float sx = nx / scale;
            precise float sy = ny / scale;
            nx = sx;
            ny = sy;
        }
        if (ny == 0.0)
            return float3(0.0, 0.0, 0.0);
        precise float X = Y * nx / ny;
        precise float Z = Y * (1.0 - nx - ny) / ny;
        return float3(X, Y, Z);
    }

    float3 lumaChroma(float3 input)
    {
        float norm = ccalNorm(input);
        precise float mean = (input.x + input.y + input.z) / 3.0;
        const float avg = ccalFmax(mean, kNormMin);
        if (!(ccalGreater(norm, 0.0) && ccalGreater(avg, 0.0)))
            return input;
        const float mix = ccalDot(input, lightness);
        if (version == kVersion3)
        {
            precise float n = norm * kInvSqrt3;
            norm = n;
        }
        precise float3 ratio = float3(input.x / norm, input.y / norm, input.z / norm);
        float coeffRatio = 0.0;
        if (version == kVersion1)
        {
            precise float acc = 0.0;
            [ForceUnroll]
            for (uint c = 0; c < 3; ++c)
                acc = acc + (1.0 - ratio[c]) * (1.0 - ratio[c]) * saturation[c];
            coeffRatio = acc;
        }
        else
        {
            precise float cr = ccalDot(ratio, saturation) / 3.0;
            coeffRatio = cr;
        }
        float3 output;
        [ForceUnroll]
        for (uint c = 0; c < 3; ++c)
        {
            // A ratio already negative may stay so, or bright saturated blues would turn black.
            const float minRatio = ccalLess(ratio[c], 0.0) ? ratio[c] : 0.0;
            precise float pushed = (1.0 - ratio[c]) * coeffRatio + ratio[c];
            output[c] = ccalFmax(pushed, minRatio);
        }
        if (version == kVersion3)
        {
            precise float n = norm / (ccalNorm(output) * kInvSqrt3);
            norm = n;
        }
        precise float gain = 1.0 + mix / avg;
        precise float scaled = norm * ccalFmax(gain, 0.0);
        precise float3 r = float3(output.x * scaled, output.y * scaled, output.z * scaled);
        return r;
    }

    float4 apply(float4 v, int2 xy)
    {
        if (wrap.nComps < 3)
            return v;
        const float minval = clip ? 0.0 : -kFltMax;
        float3 two = float3(ccalFmax(v.x, minval), ccalFmax(v.y, minval), ccalFmax(v.z, minval));
        float3 one;

        switch (adaptation)
        {
        case kAdaptationFullBradford:
        {
            one = ccalApply(rgbToXyz, two);
            const float Y = one.y;
            two = ccalDownscale(ccalApply(xyzToLms, one), Y);
            two = ccalUpscale(bradfordAdapt(two, true), Y);
            break;
        }
        case kAdaptationLinearBradford:
            two = bradfordAdapt(ccalApply(rgbToLms, two), false);
            break;
        case kAdaptationCat16:
            two = vonKries(ccalApply(rgbToLms, two));
            break;
        case kAdaptationXyz:
            two = vonKries(ccalApply(rgbToXyz, two));
            break;
        default:
            break;
        }

        one = ccalApply(mixToXyz, two);
        if (clip)
            one = ccalClipNegNan(one);
        two = gamutMapping(one);

        one = (adaptation <= kAdaptationXyz) ? xyzToAnyLms(two) : ccalApply(xyzToRgb, two);
        if (clip)
            one = ccalClipNegNan(one);

        two = lumaChroma(one);
        if (clip)
            two = ccalClipNegNan(two);

        if (applyGrey)
        {
            // The grey stays in the adaptation's space, as darktable leaves it.
            const float g = ccalFmax(ccalDot(two, grey), 0.0);
            two = float3(g, g, g);
        }
        else
        {
            one = (adaptation <= kAdaptationXyz) ? anyLmsToXyz(two) : ccalApply(rgbToXyz, two);
            if (clip)
                one = ccalClipNegNan(one);
            two = ccalApply(xyzToRgb, one);
            if (clip)
                two = ccalClipNegNan(two);
        }
        return float4(two.x, two.y, two.z, v.w);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rawcolorcalibration_wrapped(uint3 tid : SV_DispatchThreadID)
{
    CalibrationOp op;
    runWrapped<CalibrationOp>(op, tid);
}
