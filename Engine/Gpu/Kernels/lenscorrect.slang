import pixelwrap;
import coordmap;
import resamplecore;

// LensCorrect::renderOp per pixel, with LensCorrectProfile's map() and vignettingGain() in float. The
// wrap's push block carries the Source, which is both sampled and blended under the mask and mix,
// the destination and the mask; the parameter block in wrapAux carries the settings and the profile.
// LensCorrectGpuParams in Engine/Nodes/Transform/LensCorrect.cpp mirrors the block's words.

static const uint kLensCorrectClamp = 1;
static const uint kLensCorrectBlackOutside = 2;
static const uint kLensCorrectJacobian = 4;
// A Source to sample and a Source format.
static const uint kLensCorrectSample = 8;
static const uint kLensCorrectReverse = 16;
static const uint kLensCorrectManualVignette = 32;
static const uint kLensCorrectVignetteMask = 64;

static const uint kTcaTermsBase = 20;
static const uint kVignettingTermsBase = 32;
static const uint kCoordBase = 36;
static const uint kMaxCoordSteps = 4;

// LensCorrectProfile::CoordOp.
static const uint kScale = 0;
static const uint kDistPoly3 = 1;
static const uint kUnDistPoly3 = 2;
static const uint kDistPoly5 = 3;
static const uint kUnDistPoly5 = 4;
static const uint kDistPTLens = 5;
static const uint kUnDistPTLens = 6;
static const uint kDistACM = 7;
static const uint kGeomFishEyeRect = 8;
static const uint kGeomRectFishEye = 9;
static const uint kGeomPanoramicRect = 10;
static const uint kGeomRectPanoramic = 11;
static const uint kGeomFishEyePanoramic = 12;
static const uint kGeomPanoramicFishEye = 13;
static const uint kGeomERectRect = 14;
static const uint kGeomRectERect = 15;
static const uint kGeomERectFishEye = 16;
static const uint kGeomFishEyeERect = 17;
static const uint kGeomERectPanoramic = 18;
static const uint kGeomPanoramicERect = 19;
static const uint kGeomOrthographicERect = 20;
static const uint kGeomERectOrthographic = 21;
static const uint kGeomStereographicERect = 22;
static const uint kGeomERectStereographic = 23;
static const uint kGeomEquisolidERect = 24;
static const uint kGeomERectEquisolid = 25;
static const uint kGeomThobyERect = 26;
static const uint kGeomERectThoby = 27;

// LensCorrectProfile::TcaOp and VignettingOp.
static const uint kTcaNone = 0;
static const uint kTcaLinear = 1;
static const uint kTcaPoly3 = 2;
static const uint kTcaUnPoly3 = 3;
static const uint kTcaACM = 4;
static const uint kDevignette = 1;

static const float kPi = 3.14159265358979323846;
static const float kNewtonEps = 0.00001;
static const float kEpsilon = 1.0e-10;
static const float kThobyK1 = 1.47;
static const float kThobyK2 = 0.713;
static const float kHuge = 1.6e16;

struct LensCorrect
{
    uint filter;
    uint flags;
    uint nCoordSteps;
    uint tca;
    float scaleX;
    float scaleY;
    float formatX1;
    float formatY2;
    float normScale;
    float normUnScale;
    float centerX;
    float centerY;
    uint vignetting;
    float vignetteStrength;
    float vignetteRadius;
    float vignetteSteepness;
    float w2;
    float h2;
    float invMaxR;
}

LensCorrect lensCorrectLoad(StructuredBuffer<uint> params)
{
    LensCorrect d;
    d.filter = params[0];
    d.flags = params[1];
    d.nCoordSteps = min(params[2], kMaxCoordSteps);
    d.tca = params[3];
    d.scaleX = asfloat(params[4]);
    d.scaleY = asfloat(params[5]);
    d.formatX1 = asfloat(params[6]);
    d.formatY2 = asfloat(params[7]);
    d.normScale = asfloat(params[8]);
    d.normUnScale = asfloat(params[9]);
    d.centerX = asfloat(params[10]);
    d.centerY = asfloat(params[11]);
    d.vignetting = params[12];
    d.vignetteStrength = asfloat(params[13]);
    d.vignetteRadius = asfloat(params[14]);
    d.vignetteSteepness = asfloat(params[15]);
    d.w2 = asfloat(params[16]);
    d.h2 = asfloat(params[17]);
    d.invMaxR = asfloat(params[18]);
    return d;
}

float term(uint index)
{
    return asfloat(wrapAux[index]);
}

float nanValue()
{
    return asfloat(0x7fc00000u);
}

void erectWrap(inout float x, inout float theta)
{
    if (theta < 0.0)
    {
        theta = -theta;
        x += kPi;
    }
    if (theta > kPi)
    {
        theta = 2.0 * kPi - theta;
        x += kPi;
    }
}

// Newton's method on a radius, as lensfun's undistortion callbacks: the scale ru / rd, 1 when it
// does not converge (the point is left alone) and NaN for poly3's failure.
float2 applyCoord(uint op, uint base, float2 p)
{
    const float t0 = term(base);
    const float t1 = term(base + 1);
    const float t2 = term(base + 2);
    const float t3 = term(base + 3);
    const float t4 = term(base + 4);
    const float x = p.x;
    const float y = p.y;
    switch (op)
    {
    case kScale:
        return p * t0;
    case kUnDistPoly3:
    {
        const float invK1 = 1.0 / t0;
        const float rd = sqrt(x * x + y * y);
        if (rd == 0.0)
            return p;
        const float rdDivK1 = rd * invK1;
        float ru = rd;
        for (int step = 0;; step++)
        {
            const float fru = ru * ru * ru + ru * invK1 - rdDivK1;
            if (fru >= -kNewtonEps && fru < kNewtonEps)
                break;
            if (step > 5)
                return float2(nanValue(), nanValue());
            ru -= fru / (3.0 * ru * ru + invK1);
        }
        if (ru < 0.0)
            return float2(nanValue(), nanValue());
        return p * (ru / rd);
    }
    case kDistPoly3:
        return p * (t0 * (x * x + y * y) + 1.0);
    case kUnDistPoly5:
    {
        const float rd = sqrt(x * x + y * y);
        if (rd == 0.0)
            return p;
        float ru = rd;
        for (int step = 0;; step++)
        {
            const float ru2 = ru * ru;
            const float fru = ru * (1.0 + t0 * ru2 + t1 * ru2 * ru2) - rd;
            if (fru >= -kNewtonEps && fru < kNewtonEps)
                break;
            if (step > 5)
                return p;
            ru -= fru / (1.0 + 3.0 * t0 * ru2 + 5.0 * t1 * ru2 * ru2);
        }
        if (ru < 0.0)
            return p;
        return p * (ru / rd);
    }
    case kDistPoly5:
    {
        const float ru2 = x * x + y * y;
        return p * (1.0 + t0 * ru2 + t1 * ru2 * ru2);
    }
    case kUnDistPTLens:
    {
        const float rd = sqrt(x * x + y * y);
        if (rd == 0.0)
            return p;
        float ru = rd;
        for (int step = 0;; step++)
        {
            const float fru = ru * (t0 * ru * ru * ru + t1 * ru * ru + t2 * ru + 1.0) - rd;
            if (fru >= -kNewtonEps && fru < kNewtonEps)
                break;
            if (step > 5)
                return p;
            ru -= fru / (4.0 * t0 * ru * ru * ru + 3.0 * t1 * ru * ru + 2.0 * t2 * ru + 1.0);
        }
        if (ru < 0.0)
            return p;
        return p * (ru / rd);
    }
    case kDistPTLens:
    {
        const float ru2 = x * x + y * y;
        const float r = sqrt(ru2);
        return p * (t0 * ru2 * r + t1 * ru2 + t2 * r + 1.0);
    }
    case kDistACM:
    {
        const float ru2 = x * x + y * y;
        const float ru4 = ru2 * ru2;
        const float common = 1.0 + t0 * ru2 + t1 * ru4 + t2 * ru4 * ru2 + 2.0 * (t3 * y + t4 * x);
        return float2(x * common + t4 * ru2, y * common + t3 * ru2);
    }
    case kGeomFishEyeRect:
    {
        const float r = sqrt(x * x + y * y);
        float rho;
        if (r >= kPi / 2.0)
            rho = kHuge;
        else if (r == 0.0)
            rho = 1.0;
        else
            rho = tan(r) / r;
        return p * rho;
    }
    case kGeomRectFishEye:
    {
        const float r = sqrt(x * x + y * y);
        return p * ((r == 0.0) ? 1.0 : atan(r) / r);
    }
    case kGeomPanoramicRect:
        return float2(tan(x), y / cos(x));
    case kGeomRectPanoramic:
    {
        const float a = atan(x);
        return float2(a, y * cos(a));
    }
    case kGeomFishEyePanoramic:
    case kGeomFishEyeERect:
    {
        const float r = sqrt(x * x + y * y);
        const float s = (r == 0.0) ? 1.0 : (sin(r) / r);
        const float vx = cos(r);
        const float vy = s * x;
        const float q = s * y / sqrt(vx * vx + vy * vy);
        return float2(atan2(vy, vx), (op == kGeomFishEyePanoramic) ? q : atan(q));
    }
    case kGeomPanoramicFishEye:
    {
        const float s = sin(x);
        const float r = sqrt(s * s + y * y);
        const float theta = (r == 0.0) ? 0.0 : atan2(r, cos(x)) / r;
        return float2(theta * s, theta * y);
    }
    case kGeomERectRect:
    {
        float xx = x;
        float theta = -y + kPi / 2.0;
        erectWrap(xx, theta);
        return float2(tan(xx), 1.0 / (tan(theta) * cos(xx)));
    }
    case kGeomRectERect:
        return float2(atan2(x, 1.0), atan2(y, sqrt(1.0 + x * x)));
    case kGeomERectFishEye:
    {
        float xx = x;
        float theta = -y + kPi / 2.0;
        erectWrap(xx, theta);
        const float s = sin(theta);
        const float vx = s * sin(xx);
        const float vy = cos(theta);
        const float r = sqrt(vx * vx + vy * vy);
        theta = atan2(r, s * cos(xx));
        return float2(theta * vx / r, theta * vy / r);
    }
    case kGeomERectPanoramic:
        return float2(x, tan(y));
    case kGeomPanoramicERect:
        return float2(x, atan(y));
    case kGeomOrthographicERect:
    case kGeomEquisolidERect:
    case kGeomThobyERect:
    {
        const float r = sqrt(x * x + y * y);
        float theta;
        if (op == kGeomOrthographicERect)
            theta = (r < 1.0) ? asin(r) : kPi / 2.0;
        else if (op == kGeomEquisolidERect)
            theta = (r < 2.0) ? 2.0 * asin(r / 2.0) : kPi / 2.0;
        else
        {
            if (r > kThobyK1)
                return float2(kHuge, kHuge);
            theta = asin(r / kThobyK1) / kThobyK2;
        }
        const float phi = atan2(y, x);
        const float s = (theta == 0.0) ? 1.0 : (sin(theta) / theta);
        const float vx = cos(theta);
        const float vy = s * theta * cos(phi);
        return float2(atan2(vy, vx), atan(s * theta * sin(phi) / sqrt(vx * vx + vy * vy)));
    }
    case kGeomERectOrthographic:
    case kGeomERectThoby:
    {
        float xx = x;
        float theta = -y + kPi / 2.0;
        erectWrap(xx, theta);
        const float s = sin(theta);
        const float vx = s * sin(xx);
        const float vy = cos(theta);
        theta = atan2(sqrt(vx * vx + vy * vy), s * cos(xx));
        const float a = atan2(vy, vx);
        const float rho = (op == kGeomERectOrthographic) ? sin(theta) : kThobyK1 * sin(theta * kThobyK2);
        return float2(rho * cos(a), rho * sin(a));
    }
    case kGeomStereographicERect:
    {
        const float rh = sqrt(x * x + y * y);
        const float c = 2.0 * atan(rh / 2.0);
        const float sinc = sin(c);
        const float cosc = cos(c);
        if (abs(rh) <= kEpsilon)
            return float2(0.0, kHuge);
        const float lat = asin(y * sinc / rh);
        if (abs(cosc) >= kEpsilon || abs(x) >= kEpsilon)
            return float2(atan2(x * sinc, cosc * rh), lat);
        return float2(kHuge, lat);
    }
    case kGeomERectStereographic:
    {
        const float cosphi = cos(y);
        const float ksp = 2.0 / (1.0 + cosphi * cos(x));
        return float2(ksp * cosphi * sin(x), ksp * sin(y));
    }
    case kGeomERectEquisolid:
    {
        if (abs(cos(y) * cos(x) + 1.0) <= kEpsilon)
            return float2(kHuge, kHuge);
        const float k1 = sqrt(2.0 / (1.0 + cos(y) * cos(x)));
        return float2(k1 * cos(y) * sin(x), k1 * sin(y));
    }
    default:
        return p;
    }
}

float2 unTcaPoly3Channel(float v, float c, float b, float2 p)
{
    const float rd = length(p);
    if (rd == 0.0)
        return p;
    float ru = rd;
    for (int step = 0;; step++)
    {
        const float ru2 = ru * ru;
        const float fru = b * ru2 * ru + c * ru2 + v * ru - rd;
        if (fru >= -kNewtonEps && fru < kNewtonEps)
            break;
        if (step > 5)
            return p;
        ru -= fru / (3.0 * b * ru2 + 2.0 * c * ru + v);
    }
    return (ru > 0.0) ? p * (ru / rd) : p;
}

float2 tcaPoly3Channel(float v, float c, float b, bool noC, float2 p)
{
    const float ru2 = dot(p, p);
    return p * (noC ? b * ru2 + v : b * ru2 + c * sqrt(ru2) + v);
}

// The channel's six terms, interleaved with the other channel's from word base.
float2 tcaAcmChannel(uint base, float2 p)
{
    const float k0 = term(base);
    const float k1 = term(base + 2);
    const float k2 = term(base + 4);
    const float k3 = term(base + 6);
    const float k4 = term(base + 8);
    const float k5 = term(base + 10);
    const float ru2 = dot(p, p);
    const float ru4 = ru2 * ru2;
    const float common = 1.0 + k1 * ru2 + k2 * ru4 + k3 * ru4 * ru2 + 2.0 * (k4 * p.y + k5 * p.x);
    return k0 * float2(p.x * common + k5 * ru2, p.y * common + k4 * ru2);
}

struct Mapped
{
    float2 r;
    float2 g;
    float2 b;
}

// Profile::map: lensfun pixel coordinates of the output to those of the Source, per channel.
Mapped lensCorrectMap(LensCorrect d, float2 u)
{
    Mapped m;
    if (d.nCoordSteps == 0 && d.tca == kTcaNone)
    {
        m.r = u;
        m.g = u;
        m.b = u;
        return m;
    }
    float2 p = u * d.normScale - float2(d.centerX, d.centerY);
    for (uint i = 0; i < d.nCoordSteps; ++i)
    {
        const uint base = kCoordBase + 6 * i;
        p = applyCoord(wrapAux[base], base + 1, p);
    }
    m.r = p;
    m.g = p;
    m.b = p;
    const uint t = kTcaTermsBase;
    if (d.tca == kTcaLinear)
    {
        m.r = p * term(t);
        m.b = p * term(t + 1);
    }
    else if (d.tca == kTcaPoly3)
    {
        const bool noC = term(t + 2) == 0.0 && term(t + 3) == 0.0;
        m.r = tcaPoly3Channel(term(t), term(t + 2), term(t + 4), noC, p);
        m.b = tcaPoly3Channel(term(t + 1), term(t + 3), term(t + 5), noC, p);
    }
    else if (d.tca == kTcaUnPoly3)
    {
        m.r = unTcaPoly3Channel(term(t), term(t + 2), term(t + 4), p);
        m.b = unTcaPoly3Channel(term(t + 1), term(t + 3), term(t + 5), p);
    }
    else if (d.tca == kTcaACM)
    {
        m.r = tcaAcmChannel(t, p);
        m.b = tcaAcmChannel(t + 1, p);
    }
    const float2 c = float2(d.centerX, d.centerY);
    m.r = (m.r + c) * d.normUnScale;
    m.g = (m.g + c) * d.normUnScale;
    m.b = (m.b + c) * d.normUnScale;
    return m;
}

float vignettingGain(LensCorrect d, float2 p)
{
    if (d.vignetting == 0)
        return 1.0;
    const float2 q = p * d.normScale - float2(d.centerX, d.centerY);
    const float r2 = dot(q, q);
    const float r4 = r2 * r2;
    const float c = 1.0 + term(kVignettingTermsBase) * r2 + term(kVignettingTermsBase + 1) * r4 +
                    term(kVignettingTermsBase + 2) * r4 * r2;
    return (d.vignetting == kDevignette) ? 1.0 / c : c;
}

// darktable's manual vignette at lensfun position p.
float manualVignette(LensCorrect d, float2 p)
{
    const float r = min(1.0, length(p - float2(d.w2, d.h2)) * d.invMaxR);
    const float b = 1.0 + d.vignetteRadius * 10.0;
    const float v = d.vignetteSteepness;
    const float mul = -v / tanh(b);
    return max(0.0, 2.0 * d.vignetteStrength * (v + mul * tanh(b * (1.0 - r))));
}

// The output pixel (px, py)'s centre in lensfun pixel coordinates.
float2 lensfunAt(LensCorrect d, float2 pixel)
{
    return float2((pixel.x + 0.5) / d.scaleX - d.formatX1 - 0.5, d.formatY2 - 0.5 - (pixel.y + 0.5) / d.scaleY);
}

// Lensfun pixel coordinates to Source pixel coordinates at the render scale.
float2 toSource(LensCorrect d, float2 lf)
{
    return float2((lf.x + d.formatX1 + 0.5) * d.scaleX, (d.formatY2 - 0.5 - lf.y) * d.scaleY);
}

float2 greenSource(LensCorrect d, float2 pixel)
{
    return toSource(d, lensCorrectMap(d, lensfunAt(d, pixel)).g);
}

ResampleSource lensCorrectSource(LensCorrect d)
{
    ResampleSource src;
    src.buf = wrapSrc;
    src.offset = wrap.srcOffset;
    src.stride = wrap.srcStride;
    src.x1 = wrap.srcX1;
    src.y1 = wrap.srcY1;
    src.x2 = wrap.srcX2;
    src.y2 = wrap.srcY2;
    src.nComps = wrap.nComps;
    src.valid = wrap.srcX2 > wrap.srcX1 && wrap.srcY2 > wrap.srcY1;
    src.blackOutside = (d.flags & kLensCorrectBlackOutside) != 0;
    return src;
}

bool finite2(float2 p)
{
    return !isnan(p.x) && !isinf(p.x) && !isnan(p.y) && !isinf(p.y);
}

// A difference that is not finite contributes nothing, as coordJacobians() on the CPU.
float finiteOr0(float v)
{
    return (isnan(v) || isinf(v)) ? 0.0 : v;
}

float4 lensCorrectPixel(LensCorrect d, int2 p)
{
    const float4 zero = float4(0.0, 0.0, 0.0, 0.0);
    if ((d.flags & kLensCorrectSample) == 0)
        return zero;
    const float2 pixel = float2(p);
    const float2 u = lensfunAt(d, pixel);
    const Mapped m = lensCorrectMap(d, u);
    const float2 sg = toSource(d, m.g);
    // Central differences of green's Source position, reused for red and blue.
    float4 J = zero;
    const bool jacobian = (d.flags & kLensCorrectJacobian) != 0;
    if (jacobian)
    {
        const float2 xm = greenSource(d, pixel - float2(1.0, 0.0));
        const float2 xp = greenSource(d, pixel + float2(1.0, 0.0));
        const float2 ym = greenSource(d, pixel - float2(0.0, 1.0));
        const float2 yp = greenSource(d, pixel + float2(0.0, 1.0));
        J = float4(finiteOr0(0.5 * (xp.x - xm.x)), finiteOr0(0.5 * (yp.x - ym.x)), finiteOr0(0.5 * (xp.y - xm.y)),
                   finiteOr0(0.5 * (yp.y - ym.y)));
    }
    const ResampleSource src = lensCorrectSource(d);
    const bool clampFilter = (d.flags & kLensCorrectClamp) != 0;
    float4 v = sampleAt(src, sg.x, sg.y, J, jacobian, d.filter, clampFilter, kWrapNone, kWrapNone, int4(0, 0, 0, 0));
    if (wrap.nComps == 1)
        return float4(0.0, 0.0, 0.0, v.x);
    if (wrap.nComps < 3)
        return v;
    if (d.tca != kTcaNone)
    {
        const float2 sr = toSource(d, m.r);
        const float2 sb = toSource(d, m.b);
        v.x = sampleAt(src, sr.x, sr.y, J, jacobian, d.filter, clampFilter, kWrapNone, kWrapNone, int4(0, 0, 0, 0)).x;
        v.z = sampleAt(src, sb.x, sb.y, J, jacobian, d.filter, clampFilter, kWrapNone, kWrapNone, int4(0, 0, 0, 0)).z;
    }
    const bool manual = (d.flags & kLensCorrectManualVignette) != 0;
    if (d.vignetting != 0 || manual)
    {
        const bool reverse = (d.flags & kLensCorrectReverse) != 0;
        float2 lf[3] = { m.r, m.g, m.b };
        for (int c = 0; c < 3; ++c)
        {
            if (!finite2(lf[c]))
            {
                v[c] = 0.0;
                continue;
            }
            float gain = vignettingGain(d, reverse ? u : lf[c]);
            if (manual)
                gain *= 1.0 + manualVignette(d, lf[c]);
            v[c] *= gain;
        }
        if ((d.flags & kLensCorrectVignetteMask) != 0 && wrap.nComps == 4)
            v.w = finite2(m.g) ? manualVignette(d, m.g) : 0.0;
    }
    return v;
}

struct LensCorrectOp : IPixelOp
{
    LensCorrect d;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        d = lensCorrectLoad(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        return lensCorrectPixel(d, xy);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void lenscorrect_main(uint3 tid : SV_DispatchThreadID)
{
    LensCorrectOp op;
    runWrapped(op, tid);
}
