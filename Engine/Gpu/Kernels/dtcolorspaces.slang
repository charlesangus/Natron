// darktable's scene-referred colour-space helpers, the twin of Engine/Nodes/Color/DtColorSpaces.h
// (ported from darktable release-5.6.2, GPL-3.0-or-later). Keep both in step. Mat3 products are
// row-major, out = m·in. The gamut LUTs are built on the CPU and read here from a parameter
// buffer.

static const uint kDtGamutLutSize = 512;
static const float kDtPi = 3.14159265358979323846;
static const float kDtTwoPi = 6.28318530717958647693;
static const float kDtHalfPi = 1.57079632679489661923;
static const float kDtFltMin = 1.175494351e-38;

static const float kDtD65x = 0.31271;
static const float kDtD65y = 0.32902;
static const float kDtYrgWhiteR = 0.21902143;
static const float kDtYrgWhiteG = 0.54371398;
static const float kDtUcsLStarRange = 2.098883786377;
static const float kDtUcsLStarUpperLimit = 2.09885;
static const float kDtUcsBrightnessExponent = 1.33654221029386;

float3 dtMul(float3 r0, float3 r1, float3 r2, float3 v)
{
    return float3(r0.x * v.x + r0.y * v.y + r0.z * v.z,
                  r1.x * v.x + r1.y * v.y + r1.z * v.z,
                  r2.x * v.x + r2.y * v.y + r2.z * v.z);
}

float dtFastHypot(float x, float y)
{
    return sqrt(x * x + y * y);
}

float3 dtXyzToLms(float3 xyz)
{
    return dtMul(float3(0.257085, 0.859943, -0.031061),
                 float3(-0.394427, 1.175800, 0.106423),
                 float3(0.064856, -0.076250, 0.559067), xyz);
}

float3 dtLmsToXyz(float3 lms)
{
    return dtMul(float3(1.80794659, -1.29971660, 0.34785879),
                 float3(0.61783960, 0.39595453, -0.04104687),
                 float3(-0.12546960, 0.20478038, 1.74274183), lms);
}

float3 dtGradingRgbToLms(float3 rgb)
{
    return dtMul(float3(0.95, 0.38, 0.00),
                 float3(0.05, 0.62, 0.03),
                 float3(0.00, 0.00, 0.97), rgb);
}

float3 dtLmsToGradingRgb(float3 lms)
{
    return dtMul(float3(1.08771930, -0.66666667, 0.02061856),
                 float3(-0.0877193, 1.66666667, -0.05154639),
                 float3(0.0, 0.0, 1.03092784), lms);
}

float3 dtLmsToYrg(float3 lms)
{
    const float y = 0.68990272 * lms.x + 0.34832189 * lms.y;
    const float a = lms.x + lms.y + lms.z;
    const float3 n = (a == 0.0) ? float3(0.0, 0.0, 0.0) : lms / a;
    const float3 rgb = dtLmsToGradingRgb(n);
    return float3(y, rgb.x, rgb.y);
}

float3 dtYrgToLms(float3 yrg)
{
    const float3 n = dtGradingRgbToLms(float3(yrg.y, yrg.z, 1.0 - yrg.y - yrg.z));
    const float denom = 0.68990272 * n.x + 0.34832189 * n.y;
    const float a = (denom == 0.0) ? 0.0 : yrg.x / denom;
    return n * a;
}

// Y, chroma, and the cosine and sine of the hue around the D65 white.
float4 dtYrgToYch(float3 yrg)
{
    const float r = yrg.y - kDtYrgWhiteR;
    const float g = yrg.z - kDtYrgWhiteG;
    const float c = dtFastHypot(g, r);
    return float4(yrg.x, c, (c != 0.0) ? r / c : 1.0, (c != 0.0) ? g / c : 0.0);
}

float3 dtYchToYrg(float4 ych)
{
    return float3(ych.x, ych.y * ych.z + kDtYrgWhiteR, ych.y * ych.w + kDtYrgWhiteG);
}

float4 dtGamutCheckYrg(float4 ych)
{
    const float3 yrg = dtYchToYrg(ych);
    float maxC = ych.y;
    if (yrg.y < 0.0)
        maxC = min(-kDtYrgWhiteR / ych.z, maxC);
    if (yrg.z < 0.0)
        maxC = min(-kDtYrgWhiteG / ych.w, maxC);
    if (yrg.y + yrg.z > 1.0)
        maxC = min((1.0 - kDtYrgWhiteR - kDtYrgWhiteG) / (ych.z + ych.w), maxC);
    return float4(ych.x, maxC, ych.z, ych.w);
}

static const float kDtJzB = 1.15;
static const float kDtJzG = 0.66;
static const float kDtJzC1 = 0.8359375;
static const float kDtJzC2 = 18.8515625;
static const float kDtJzC3 = 18.6875;
static const float kDtJzN = 0.159301758;
static const float kDtJzP = 134.034375;
static const float kDtJzD = -0.56;
static const float kDtJzD0 = 1.6295499532821566e-11;

// darktable's AI_trans rows 1 and 2: LMS[i] = Iz + az·kDtJzAI1[i] + bz·kDtJzAI2[i].
static const float3 kDtJzAI1 = float3(0.1386050432715393, -0.1386050432715393, -0.0960192420263190);
static const float3 kDtJzAI2 = float3(0.0580473161561189, -0.0580473161561189, -0.8118918960560390);

float3 dtXyzToJzAzBz(float3 xyzD65)
{
    const float3 xyz = float3(kDtJzB * xyzD65.x - (kDtJzB - 1.0) * xyzD65.z,
                              kDtJzG * xyzD65.y - (kDtJzG - 1.0) * xyzD65.x,
                              xyzD65.z);
    float3 lms = dtMul(float3(0.41478972, 0.57999900, 0.01464800),
                       float3(-0.2015100, 1.1206490, 0.0531008),
                       float3(-0.0166008, 0.2648000, 0.6684799), xyz);
    for (int i = 0; i < 3; ++i)
    {
        float l = pow(max(lms[i] / 10000.0, 0.0), kDtJzN);
        lms[i] = pow((kDtJzC1 + kDtJzC2 * l) / (1.0 + kDtJzC3 * l), kDtJzP);
    }
    float3 jab = dtMul(float3(0.5, 0.5, 0.0),
                       float3(3.524000, -4.066708, 0.542708),
                       float3(0.199076, 1.096799, -1.295875), lms);
    jab.x = max(((1.0 + kDtJzD) * jab.x) / (1.0 + kDtJzD * jab.x) - kDtJzD0, 0.0);
    return jab;
}

float dtJzToIz(float jz)
{
    const float iz = jz + kDtJzD0;
    return max(iz / (1.0 + kDtJzD - kDtJzD * iz), 0.0);
}

float3 dtJzAzBzToXyz(float3 jab)
{
    const float iz = dtJzToIz(jab.x);
    float3 lms = iz + jab.y * kDtJzAI1 + jab.z * kDtJzAI2;
    for (int i = 0; i < 3; ++i)
    {
        float l = pow(max(lms[i], 0.0), 1.0 / kDtJzP);
        l = (kDtJzC1 - l) / (kDtJzC3 * l - kDtJzC2);
        lms[i] = 10000.0 * pow(max(l, 0.0), 1.0 / kDtJzN);
    }
    const float3 xyz = dtMul(float3(1.9242264357876067, -1.0047923125953657, 0.0376514040306180),
                             float3(0.3503167620949991, 0.7264811939316552, -0.0653844229480850),
                             float3(-0.0909828109828475, -0.3127282905230739, 1.5227665613052603), lms);
    const float x = (xyz.x + (kDtJzB - 1.0) * xyz.z) / kDtJzB;
    const float y = (xyz.y + (kDtJzG - 1.0) * x) / kDtJzG;
    return float3(x, y, xyz.z);
}

float3 dtD65XyzToXyY(float3 sxyz)
{
    const float3 xyz = max(sxyz, float3(0.0, 0.0, 0.0));
    const float sum = xyz.x + xyz.y + xyz.z;
    return float3((sum > 0.0) ? xyz.x / sum : kDtD65x, (sum > 0.0) ? xyz.y / sum : kDtD65y, xyz.y);
}

float3 dtXyYToXyz(float3 xyY)
{
    if (xyY.y == 0.0)
        return float3(0.0, 0.0, 0.0);
    return float3(xyY.z * xyY.x / xyY.y, xyY.z, xyY.z * (1.0 - xyY.x - xyY.y) / xyY.y);
}

float dtYToUcsLStar(float y)
{
    const float yHat = pow(y, 0.631651345306265);
    return kDtUcsLStarRange * yHat / (yHat + 1.12426773749357);
}

float dtUcsLStarToY(float lStar)
{
    return pow((1.12426773749357 * lStar / (kDtUcsLStarRange - lStar)), 1.5831518565279648);
}

float dtSignedFloor(float d)
{
    return (d >= 0.0) ? max(kDtFltMin, d) : min(-kDtFltMin, d);
}

float2 dtXyYToUcsUV(float3 xyY)
{
    const float3 uvd = float3(-0.783941002840055, 0.745273540913283, 0.318707282433486) * xyY.x
                       + float3(0.277512987809202, -0.205375866083878, 2.16743692732158) * xyY.y
                       + float3(0.153836578598858, -0.165478376301988, 0.291320554395942);
    const float div = dtSignedFloor(uvd.z);
    const float2 uv = float2(uvd.x / div, uvd.y / div);
    const float2 factors = float2(1.39656225667, 1.4513954287);
    const float2 halfValues = float2(1.49217352929, 1.52488637914);
    const float2 uvStar = factors * uv / (abs(uv) + halfValues);
    return float2(-1.124983854323892 * uvStar.x - 0.980483721769325 * uvStar.y,
                  1.86323315098672 * uvStar.x + 1.971853092390862 * uvStar.y);
}

float dtUcsChroma(float lStar, float lWhite, float m2)
{
    return 15.932993652962535 * pow(lStar, 0.6523997524738018) * pow(m2, 0.6007557017508491) / lWhite;
}

float3 dtXyYToUcsJCH(float3 xyY, float lWhite)
{
    const float2 uv = dtXyYToUcsUV(xyY);
    const float lStar = dtYToUcsLStar(xyY.z);
    const float m2 = uv.x * uv.x + uv.y * uv.y;
    return float3(lStar / lWhite, dtUcsChroma(lStar, lWhite, m2), atan2(uv.y, uv.x));
}

float3 dtUcsJCHToXyY(float3 jch, float lWhite)
{
    const float lStar = clamp(jch.x * lWhite, 0.0, kDtUcsLStarUpperLimit);
    const float m = (lStar != 0.0)
                    ? pow(jch.y * lWhite / (15.932993652962535 * pow(lStar, 0.6523997524738018)), 0.8322850678616855)
                    : 0.0;
    const float up = m * cos(jch.z);
    const float vp = m * sin(jch.z);
    const float2 uvStar = float2(-5.037522385190711 * up - 2.504856328185843 * vp,
                                 4.760029407436461 * up + 2.874012963239247 * vp);
    const float2 factors = float2(1.39656225667, 1.4513954287);
    const float2 halfValues = float2(1.49217352929, 1.52488637914);
    const float2 uv = -halfValues * uvStar / (abs(uvStar) - factors);
    const float3 xyD = float3(0.167171472114775, -0.150959086409163, 0.940254742367256) * uv.x
                       + float3(0.141299802443708, -0.155185060382272, 1.0) * uv.y
                       + float3(-0.00801531300850582, -0.00843312433578007, -0.0256325967652889);
    const float div = dtSignedFloor(xyD.z);
    return float3(xyD.x / div, xyD.y / div, dtUcsLStarToY(lStar));
}

float3 dtUcsJCHToHSB(float3 jch)
{
    const float b = jch.x * (pow(jch.y, kDtUcsBrightnessExponent) + 1.0);
    return float3(jch.z, (b > 0.0) ? jch.y / b : 0.0, b);
}

float3 dtUcsHSBToJCH(float3 hsb)
{
    const float c = hsb.y * hsb.z;
    return float3(hsb.z / (pow(c, kDtUcsBrightnessExponent) + 1.0), c, hsb.x);
}

float3 dtUcsJCHToHCB(float3 jch)
{
    return float3(jch.z, jch.y, jch.x * (pow(jch.y, kDtUcsBrightnessExponent) + 1.0));
}

float3 dtUcsHCBToJCH(float3 hcb)
{
    return float3(hcb.z / (pow(hcb.y, kDtUcsBrightnessExponent) + 1.0), hcb.y, hcb.x);
}

float dtSoftClip(float x, float softThreshold, float hardThreshold)
{
    const float norm = hardThreshold - softThreshold;
    return (x > softThreshold) ? softThreshold + (1.0 - exp(-(x - softThreshold) / norm)) * norm : x;
}

// The gamut LUT stored as kDtGamutLutSize floats from word base of buf, at hue in [-pi, pi).
float dtLookupGamut(StructuredBuffer<uint> buf, uint base, float hue)
{
    const float x = float(kDtGamutLutSize) * (hue + kDtPi) / kDtTwoPi;
    const float xPrev = floor(x);
    const float xNext = ceil(x);
    const uint xi = uint(int(xPrev)) & (kDtGamutLutSize - 1);
    const uint xii = uint(int(xNext)) & (kDtGamutLutSize - 1);
    const float yPrev = asfloat(buf[base + xi]);
    return yPrev + ((xi != xii) ? (x - xPrev) * (asfloat(buf[base + xii]) - yPrev) : 0.0);
}
