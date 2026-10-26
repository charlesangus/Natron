import pixelwrap;

// The HSVTool, HueCorrect and HSVKeyer nodes, with HSVTool.cpp, HueCorrect.cpp, HSVKeyer.cpp and
// HueCurves.h written the same way, one rounding per statement, so the CPU and the GPU agree to
// float rounding.

// ColorMath::colorspaceHexHue: the hue of v in turns, red at 0.
float hueHexHue(float3 v, float maxv, float delta)
{
    if (!(delta > 0.0))
        return 0.0;
    precise float h;
    if (v.r == maxv)
    {
        precise float d = v.g - v.b;
        h = d / delta;
    }
    else if (v.g == maxv)
    {
        precise float d = v.b - v.r;
        precise float q = d / delta;
        h = 2.0 + q;
    }
    else
    {
        precise float d = v.r - v.g;
        precise float q = d / delta;
        h = 4.0 + q;
    }
    if (h < 0.0)
        h = h + 6.0;
    precise float o = h * 0.1666666716337204;
    return o;
}

// ColorMath::colorspaceHexToRgb: the colour of hue h (turns), largest channel hi and chroma c.
float3 hueHexToRgb(float h, float hi, float c)
{
    precise float hs = h * 6.0;
    const float fl = floor(hs);
    int i = 0;
    precise float f = 0.0;
    if (abs(fl) < 16777216.0)
    {
        f = hs - fl;
        i = int(fl) % 6;
        if (i < 0)
            i += 6;
    }
    precise float lo = hi - c;
    precise float cf = c * f;
    precise float up = lo + cf;
    precise float down = hi - cf;
    switch (i)
    {
    case 0: return float3(hi, up, lo);
    case 1: return float3(down, hi, lo);
    case 2: return float3(lo, hi, up);
    case 3: return float3(lo, down, hi);
    case 4: return float3(up, lo, hi);
    default: return float3(hi, lo, down);
    }
}

float hueMax3(float3 v)
{
    return max(v.r, max(v.g, v.b));
}

float hueMin3(float3 v)
{
    return min(v.r, min(v.g, v.b));
}

float hueWrapTurn(float x)
{
    const float fl = floor(x);
    precise float o = x - fl;
    return o;
}

float hueSignedTurn(float x)
{
    precise float t = x + 0.5;
    const float fl = floor(t);
    precise float u = t - fl;
    precise float o = u - 0.5;
    return o;
}

bool hueWithinArc(float h, float h0, float h1)
{
    return ((h1 < h0) && ((h <= h1) || (h0 <= h))) || ((h0 <= h) && (h <= h1));
}

float hueArcRise(float hIn, float h0, float h1In)
{
    if (!hueWithinArc(hIn, h0, h1In))
        return 0.0;
    if (h1In == h0)
        return 1.0;
    precise float h = hIn;
    precise float h1 = h1In;
    if (h1 < h0)
    {
        h1 = h1 + 1.0;
        if (h < h0)
            h = h + 1.0;
    }
    precise float n = h - h0;
    precise float d = h1 - h0;
    precise float o = n / d;
    return o;
}

float hueArcFall(float hIn, float h0, float h1In)
{
    if (!hueWithinArc(hIn, h0, h1In))
        return 0.0;
    if (h1In == h0)
        return 1.0;
    precise float h = hIn;
    precise float h1 = h1In;
    if (h1 < h0)
    {
        h1 = h1 + 1.0;
        if (h < h0)
            h = h + 1.0;
    }
    precise float n = h1 - h;
    precise float d = h1 - h0;
    precise float o = n / d;
    return o;
}

float hueTrapezoid4(float x, float a, float b, float c, float d)
{
    if (!(x >= a))
        return 0.0;
    if (x < b)
    {
        precise float n = x - a;
        precise float w = b - a;
        precise float o = n / w;
        return o;
    }
    if (x <= c)
        return 1.0;
    if (x <= d)
    {
        precise float n = d - x;
        precise float w = d - c;
        precise float o = n / w;
        return o;
    }
    return 0.0;
}

float hueCircleTrapezoid(float h, float low, float rise, float full, float fall)
{
    precise float d = h - low;
    precise float t = hueWrapTurn(d);
    float k = hueTrapezoid4(t, 0.0, rise, full, fall);
    for (uint turn = 0; turn < 3; ++turn)
    {
        t = t + 1.0;
        if (!(t <= fall))
            break;
        k = max(k, hueTrapezoid4(t, 0.0, rise, full, fall));
    }
    return k;
}

// ---------------------------------------------------------------------------------------------
// HSVTool. Parameter block, in words: flags, the alpha output, then the hue's low, high, low
// rolloff, high rolloff, mean, rotation and gain, then the saturation's and the brightness's low,
// high, low rolloff, high rolloff, rolloff, adjust, gain and mid.

static const uint kHsvAllHues = 1;
static const uint kHsvChangesColour = 2;
static const uint kHsvClampBlack = 4;
static const uint kHsvClampWhite = 8;

float hsvRangeCoeff(float x, float lo, float hi, float loR, float hiR, float r)
{
    if ((lo <= x) && (x <= hi))
        return 1.0;
    if ((loR <= x) && (x <= lo))
    {
        precise float n = x - loR;
        precise float o = n / r;
        return o;
    }
    if ((hi <= x) && (x <= hiR))
    {
        precise float n = hiR - x;
        precise float o = n / r;
        return o;
    }
    return 0.0;
}

struct HsvToolOp : IPixelOp
{
    uint flags;
    uint outputAlpha;
    float p[23];

    [mutating] void load(StructuredBuffer<uint> params)
    {
        flags = params[0];
        outputAlpha = params[1];
        for (uint i = 0; i < 23; ++i)
            p[i] = wrapParamFloat(params, 2 + i);
    }

    float4 apply(float4 v, int2 xy)
    {
        const float hueLow = p[0], hueHigh = p[1], hueLowRolloff = p[2], hueHighRolloff = p[3];
        const float hueMean = p[4], hueRotation = p[5], hueGain = p[6];
        const float satLow = p[7], satHigh = p[8], satLowRolloff = p[9], satHighRolloff = p[10];
        const float satRolloff = p[11], satAdjust = p[12], satGain = p[13], satMid = p[14];
        const float valLow = p[15], valHigh = p[16], valLowRolloff = p[17], valHighRolloff = p[18];
        const float valRolloff = p[19], valAdjust = p[20], valGain = p[21], valMid = p[22];

        const float maxv = hueMax3(v.rgb);
        const float minv = hueMin3(v.rgb);
        precise float delta = maxv - minv;
        precise float h = hueHexHue(v.rgb, maxv, delta);
        if (h >= 1.0)
            h = h - 1.0;
        precise float s = 0.0;
        if (maxv > 0.0)
            s = delta / maxv;
        const float val = maxv;

        float hc = 1.0;
        if ((flags & kHsvAllHues) == 0 && !hueWithinArc(h, hueLow, hueHigh))
        {
            const float c0 = hueArcRise(h, hueLowRolloff, hueLow);
            const float c1 = hueArcFall(h, hueHigh, hueHighRolloff);
            hc = max(c0, c1);
        }
        const float sc = hsvRangeCoeff(s, satLow, satHigh, satLowRolloff, satHighRolloff, satRolloff);
        const float vc = hsvRangeCoeff(val, valLow, valHigh, valLowRolloff, valHighRolloff, valRolloff);
        const float coeff = min(min(hc, sc), vc);

        float3 o = v.rgb;
        if ((flags & kHsvChangesColour) != 0 && coeff > 0.0)
        {
            precise float dh = h - hueMean;
            const float sh = hueSignedTurn(dh);
            precise float gh = hueGain * sh;
            precise float ah = hueRotation + gh;
            precise float ch = coeff * ah;
            precise float hn = h + ch;
            precise float ds = s - satMid;
            precise float gs = satGain * ds;
            precise float as_ = satAdjust + gs;
            precise float cs = coeff * as_;
            precise float sn = s + cs;
            if (sn < 0.0)
                sn = 0.0;
            precise float dv = val - valMid;
            precise float gv = valGain * dv;
            precise float av = valAdjust + gv;
            precise float cv = coeff * av;
            precise float vn = val + cv;
            precise float chroma = delta;
            if (maxv > 0.0)
                chroma = vn * sn;
            o = hueHexToRgb(hueWrapTurn(hn), vn, chroma);
        }
        if ((flags & kHsvClampBlack) != 0)
            o = max(o, float3(0.0));
        if ((flags & kHsvClampWhite) != 0)
            o = min(o, float3(1.0));
        float a = v.a;
        switch (outputAlpha)
        {
        case 1: a = hc; break;
        case 2: a = sc; break;
        case 3: a = vc; break;
        case 4: a = min(hc, sc); break;
        case 5: a = min(hc, vc); break;
        case 6: a = min(sc, vc); break;
        case 7: a = coeff; break;
        default: break;
        }
        return float4(o, a);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void hsvtool_wrapped(uint3 tid : SV_DispatchThreadID)
{
    HsvToolOp op;
    runWrapped<HsvToolOp>(op, tid);
}

// ---------------------------------------------------------------------------------------------
// HueCorrect. Parameter block, in words: flags, the luminance kind (weighted, average, max), the
// three weights, the luminance mix, two unused words, then the ten curves' tables of
// kLutSize entries each, in HueCorrectOp::CurveEnum order.

static const uint kHcClampBlack = 1;
static const uint kHcClampWhite = 2;
static const uint kHcHeaderWords = 8;
static const uint kHcLutIntervals = 1024;
static const uint kHcLutSize = 1025;
static const float kHcLutScale = 170.66667175292969;

static const uint kHcHue = 0;
static const uint kHcSat = 1;
static const uint kHcLum = 2;
static const uint kHcRed = 3;
static const uint kHcGreen = 4;
static const uint kHcBlue = 5;
static const uint kHcRSup = 6;
static const uint kHcGSup = 7;
static const uint kHcBSup = 8;
static const uint kHcSatThrsh = 9;

float hcLutAt(uint curve, float x)
{
    const uint base = kHcHeaderWords + curve * kHcLutSize;
    precise float t = x * kHcLutScale;
    const float fl = floor(t);
    if (!(fl >= 0.0))
        return asfloat(wrapAux[base]);
    if (fl >= float(kHcLutIntervals))
        return asfloat(wrapAux[base + kHcLutIntervals]);
    const uint i = uint(fl);
    precise float f = t - fl;
    const float a = asfloat(wrapAux[base + i]);
    const float b = asfloat(wrapAux[base + i + 1]);
    precise float d = b - a;
    precise float e = d * f;
    precise float o = a + e;
    return o;
}

// HueCurves::curvePosition: the curve abscissa of hue h, red at 1.
float hcCurvePosition(float hue)
{
    precise float p = hue * 6.0;
    precise float x = p + 1.0;
    if (x > 6.0)
        x = x - 6.0;
    return x;
}

float hcSuppress(float c, float m, float k)
{
    if (c > m)
    {
        precise float d = c - m;
        precise float e = k * d;
        precise float o = m + e;
        return o;
    }
    return c;
}

float hcClampOut(uint flags, float v)
{
    if ((flags & kHcClampBlack) != 0 && v < 0.0)
        return 0.0;
    if ((flags & kHcClampWhite) != 0 && v > 1.0)
        return 1.0;
    return v;
}

struct HueCorrectOp : IPixelOp
{
    uint flags;
    uint lumaKind;
    float3 weights;
    float mixLuminance;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        flags = params[0];
        lumaKind = params[1];
        weights = float3(wrapParamFloat(params, 2), wrapParamFloat(params, 3), wrapParamFloat(params, 4));
        mixLuminance = wrapParamFloat(params, 5);
    }

    float luminance(float3 c)
    {
        if (lumaKind == 1)
        {
            precise float s1 = c.r + c.g;
            precise float s = s1 + c.b;
            precise float o = s / 3.0;
            return o;
        }
        if (lumaKind == 2)
            return max(max(c.r, c.g), c.b);
        precise float a = weights.x * c.r;
        precise float b = weights.y * c.g;
        precise float cc = weights.z * c.b;
        precise float ab = a + b;
        precise float o = ab + cc;
        return o;
    }

    float4 apply(float4 v, int2 xy)
    {
        float lIn = 0.0;
        if (mixLuminance > 0.0)
            lIn = luminance(v.rgb);
        const float maxv = hueMax3(v.rgb);
        const float minv = hueMin3(v.rgb);
        precise float delta = maxv - minv;
        const float h0 = hueHexHue(v.rgb, maxv, delta);
        precise float s = 0.0;
        if (maxv > 0.0)
            s = delta / maxv;
        const float x = hcCurvePosition(h0);
        float k[10];
        for (uint curve = 0; curve < 10; ++curve)
            k[curve] = hcLutAt(curve, x);
        precise float r = v.r;
        precise float g = v.g;
        precise float b = v.b;

        if (k[kHcHue] != 1.0)
        {
            precise float d = k[kHcHue] - 1.0;
            precise float half = d * 0.5;
            precise float h1 = h0 + half;
            const float3 c = hueHexToRgb(hueWrapTurn(h1), maxv, delta);
            r = c.r;
            g = c.g;
            b = c.b;
        }
        if (k[kHcRSup] != 1.0)
            r = hcSuppress(r, min(g, b), k[kHcRSup]);
        if (k[kHcGSup] != 1.0)
            g = hcSuppress(g, min(r, b), k[kHcGSup]);
        if (k[kHcBSup] != 1.0)
            b = hcSuppress(b, min(r, g), k[kHcBSup]);
        const float thr = k[kHcSatThrsh];
        const float lum = k[kHcLum];
        precise float rl = k[kHcRed] * lum;
        precise float gl = k[kHcGreen] * lum;
        precise float bl = k[kHcBlue] * lum;
        if (s > thr)
        {
            precise float e = s - thr;
            precise float tr = e * rl;
            precise float tg = e * gl;
            precise float tb = e * bl;
            precise float nr = thr + tr;
            precise float ng = thr + tg;
            precise float nb = thr + tb;
            precise float qr = nr / s;
            precise float qg = ng / s;
            precise float qb = nb / s;
            r = r * qr;
            g = g * qg;
            b = b * qb;
        }
        else if (thr == 0.0)
        {
            r = r * rl;
            g = g * gl;
            b = b * bl;
        }
        const float sat = k[kHcSat];
        if (sat != 1.0)
        {
            const float l = luminance(float3(r, g, b));
            precise float om = 1.0 - sat;
            precise float a = om * l;
            precise float sr = sat * r;
            precise float sg = sat * g;
            precise float sb = sat * b;
            r = a + sr;
            g = a + sg;
            b = a + sb;
        }
        if (mixLuminance > 0.0)
        {
            const float lOut = luminance(float3(r, g, b));
            if (lOut <= 0.0)
            {
                r = lIn;
                g = lIn;
                b = lIn;
            }
            else
            {
                precise float q = lIn / lOut;
                precise float q1 = q - 1.0;
                precise float mq = mixLuminance * q1;
                precise float f = 1.0 + mq;
                r = r * f;
                g = g * f;
                b = b * f;
            }
        }
        return float4(hcClampOut(flags, r), hcClampOut(flags, g), hcClampOut(flags, b), v.a);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void huecorrect_wrapped(uint3 tid : SV_DispatchThreadID)
{
    HueCorrectOp op;
    runWrapped<HueCorrectOp>(op, tid);
}

// ---------------------------------------------------------------------------------------------
// HSVKeyer. Parameter block, in words: flags (1: invert, 2/4/8: use hue/saturation/luma, bits 4-5:
// the model, 0 HSV, 1 HSL, 2 OkLCh, bits 6-7: the output, 0 alpha, 1 premultiplied, 2 matte), the
// hue trapezoid's low, rise, full and fall, the saturation and luma trapezoids' four points, then
// the working RGB to OkLab LMS matrix, row-major. Mirrors HSVKeyerOp::key and
// ColorMath::keyModelComponents statement by statement.

static const uint kHkInvert = 1;
static const uint kHkUseHue = 2;
static const uint kHkUseSat = 4;
static const uint kHkUseLuma = 8;
static const uint kHkModelShift = 4;
static const uint kHkOutputShift = 6;
static const uint kHkModelHsl = 1;
static const uint kHkModelOkLch = 2;
static const uint kHkOutputPremultiplied = 1;
static const uint kHkOutputMatte = 2;
// ColorMath::kKeyHueFadeRelativeChroma and kKeyHueFadeOkChroma.
static const float kHkFadeRelative = 0.05;
static const float kHkFadeOk = 0.02;

// ColorMath::colorspaceCbrtPositive.
float hueCbrtPositive(float x)
{
    precise float r = asfloat(0x54a21d2au - asuint(x) / 3u);
    [ForceUnroll]
    for (uint i = 0; i < 3; ++i)
    {
        precise float r2 = r * r;
        precise float r3 = r2 * r;
        precise float xr3 = x * r3;
        precise float third = xr3 * 0.3333333432674408;
        precise float d = 1.3333333730697632 - third;
        r = r * d;
    }
    precise float r2 = r * r;
    precise float o = x * r2;
    return o;
}

// ColorMath::colorspaceSignedCbrt.
float hueSignedCbrt(float x)
{
    if (x > 0.0)
        return hueCbrtPositive(x);
    if (x < 0.0)
    {
        precise float n = -x;
        return -hueCbrtPositive(n);
    }
    return 0.0;
}

// ColorMath::colorspaceAtanUnit.
float hueAtanUnit(float tIn)
{
    precise float t = tIn;
    precise float base = 0.0;
    if (t > 0.4142135679721832)
    {
        precise float num = t - 1.0;
        precise float den = t + 1.0;
        t = num / den;
        base = 0.7853981852531433;
    }
    precise float z = t * t;
    precise float p = 0.0805374458432197 * z;
    p = p - 0.13877685368061066;
    p = p * z;
    p = p + 0.19977711141109467;
    p = p * z;
    p = p - 0.3333294987678528;
    p = p * z;
    p = p * t;
    p = p + t;
    precise float o = base + p;
    return o;
}

// ColorMath::colorspaceAtan2Turns.
float hueAtan2Turns(float y, float x)
{
    const float ax = abs(x);
    const float ay = abs(y);
    if (ax == 0.0 && ay == 0.0)
        return 0.0;
    precise float a;
    if (ay <= ax)
    {
        precise float q = ay / ax;
        a = hueAtanUnit(q);
    }
    else
    {
        precise float q = ax / ay;
        precise float t = hueAtanUnit(q);
        a = 1.5707963705062866 - t;
    }
    if (x < 0.0)
        a = 3.1415927410125732 - a;
    if (y < 0.0)
        a = -a;
    precise float turns = a * 0.15915493667125702;
    if (turns < 0.0)
        turns = turns + 1.0;
    return turns;
}

// ColorMath::keyHueFade.
float hueKeyFade(float c, float edge)
{
    if (!(c > 0.0))
        return 0.0;
    if (c >= edge)
        return 1.0;
    precise float t = c / edge;
    precise float t2 = t * t;
    precise float tt = t + t;
    precise float u = 3.0 - tt;
    precise float o = t2 * u;
    return o;
}

// m (row-major) times v, rounding as ColorMath::colorspaceMul3 does.
float3 hueMul3(float m[9], float3 v)
{
    float3 o;
    [ForceUnroll]
    for (uint r = 0; r < 3; ++r)
    {
        precise float s = m[r * 3] * v.x;
        precise float g = m[r * 3 + 1] * v.y;
        s = s + g;
        precise float b = m[r * 3 + 2] * v.z;
        s = s + b;
        o[r] = s;
    }
    return o;
}

struct HSVKeyerOp : IPixelOp
{
    uint flags;
    float4 hue;
    float4 sat;
    float4 luma;
    float toLms[9];

    [mutating] void load(StructuredBuffer<uint> params)
    {
        flags = params[0];
        hue = wrapParamFloat4(params, 1);
        sat = wrapParamFloat4(params, 5);
        luma = wrapParamFloat4(params, 9);
        [ForceUnroll]
        for (uint i = 0; i < 9; ++i)
            toLms[i] = wrapParamFloat(params, 13 + i);
    }

    float4 apply(float4 v, int2 xy)
    {
        const uint model = (flags >> kHkModelShift) & 3u;
        float3 comps;
        float weight;
        if (model == kHkModelOkLch)
        {
            float3 lms = hueMul3(toLms, v.rgb);
            lms = float3(hueSignedCbrt(lms.x), hueSignedCbrt(lms.y), hueSignedCbrt(lms.z));
            float m2[9] = { 0.2104542553, 0.7936177850, -0.0040720468,
                            1.9779984951, -2.4285922050, 0.4505937099,
                            0.0259040371, 0.7827717662, -0.8086757660 };
            const float3 lab = hueMul3(m2, lms);
            precise float aa = lab.y * lab.y;
            precise float bb = lab.z * lab.z;
            precise float cc = aa + bb;
            precise float c = sqrt(cc);
            comps = float3(hueAtan2Turns(lab.z, lab.y), c, lab.x);
            weight = hueKeyFade(c, kHkFadeOk);
        }
        else
        {
            const float maxv = hueMax3(v.rgb);
            const float minv = hueMin3(v.rgb);
            precise float delta = maxv - minv;
            const float h = hueHexHue(v.rgb, maxv, delta);
            precise float relative = 0.0;
            if (maxv > 0.0)
                relative = delta / maxv;
            if (model == kHkModelHsl)
            {
                precise float sum = maxv + minv;
                precise float l = sum * 0.5;
                precise float den = (l <= 0.5) ? sum : (2.0 - sum);
                precise float s = 0.0;
                if (den != 0.0)
                    s = delta / den;
                comps = float3(h, s, l);
            }
            else
            {
                comps = float3(h, relative, maxv);
            }
            weight = hueKeyFade(relative, kHkFadeRelative);
        }
        precise float k = 1.0;
        if ((flags & kHkUseHue) != 0)
        {
            const float hk = hueCircleTrapezoid(comps.x, hue.x, hue.y, hue.z, hue.w);
            precise float weighted = hk * weight;
            k = k * weighted;
        }
        if ((flags & kHkUseSat) != 0)
        {
            const float sk = hueTrapezoid4(comps.y, sat.x, sat.y, sat.z, sat.w);
            k = k * sk;
        }
        if ((flags & kHkUseLuma) != 0)
        {
            const float lk = hueTrapezoid4(comps.z, luma.x, luma.y, luma.z, luma.w);
            k = k * lk;
        }
        if ((flags & kHkInvert) != 0)
            k = 1.0 - k;
        const uint output = (flags >> kHkOutputShift) & 3u;
        if (output == kHkOutputMatte)
            return float4(k, k, k, k);
        if (output == kHkOutputPremultiplied)
        {
            precise float3 rgb = v.rgb * k;
            return float4(rgb, k);
        }
        return float4(v.rgb, k);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void hsvkeyer_wrapped(uint3 tid : SV_DispatchThreadID)
{
    HSVKeyerOp op;
    runWrapped<HSVKeyerOp>(op, tid);
}
