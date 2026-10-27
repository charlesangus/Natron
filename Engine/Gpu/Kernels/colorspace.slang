import pixelwrap;

// The Colorspace node's conversions, with ColorMath.h's colorspace* functions written the same
// way, one rounding per statement, so the CPU and the GPU agree to float rounding. The models are
// ColorMath::ColorspaceModelEnum.

static const uint kModelRGB = 0;
static const uint kModelHSV = 1;
static const uint kModelHSL = 2;
static const uint kModelHSI = 3;
static const uint kModelXYZ = 12;
static const uint kModelXyY = 13;
static const uint kModelLab = 14;

struct ColorspaceSide
{
    uint model;
    float kr, kg, kb, kgInv;
    float yScale, yOffset, yInv;
    float cOffset, cb, cr, cbInv, crInv;
}

ColorspaceSide loadSide(StructuredBuffer<uint> params, uint model, uint index)
{
    ColorspaceSide s;
    s.model = model;
    s.kr = wrapParamFloat(params, index);
    s.kg = wrapParamFloat(params, index + 1);
    s.kb = wrapParamFloat(params, index + 2);
    s.kgInv = wrapParamFloat(params, index + 3);
    s.yScale = wrapParamFloat(params, index + 4);
    s.yOffset = wrapParamFloat(params, index + 5);
    s.yInv = wrapParamFloat(params, index + 6);
    s.cOffset = wrapParamFloat(params, index + 7);
    s.cb = wrapParamFloat(params, index + 8);
    s.cr = wrapParamFloat(params, index + 9);
    s.cbInv = wrapParamFloat(params, index + 10);
    s.crInv = wrapParamFloat(params, index + 11);
    return s;
}

float csCbrtPositive(float x)
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

float csAtanUnit(float tIn)
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

float csAtan2Turns(float y, float x)
{
    const float ax = abs(x);
    const float ay = abs(y);
    if (ax == 0.0 && ay == 0.0)
        return 0.0;
    precise float a;
    if (ay <= ax)
    {
        precise float q = ay / ax;
        a = csAtanUnit(q);
    }
    else
    {
        precise float q = ax / ay;
        precise float t = csAtanUnit(q);
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

float csTanNear(float u)
{
    precise float z = u * u;
    precise float p = z * -2.505210838544172e-08;
    p = p + 2.7557319223985893e-06;
    p = p * z;
    p = p - 0.0001984126984126984;
    p = p * z;
    p = p + 0.008333333333333333;
    p = p * z;
    p = p - 0.16666666666666666;
    p = p * z;
    p = p * u;
    precise float s = u + p;
    precise float q = z * 2.08767569878681e-09;
    q = q - 2.755731922398589e-07;
    q = q * z;
    q = q + 2.48015873015873e-05;
    q = q * z;
    q = q - 0.001388888888888889;
    q = q * z;
    q = q + 0.041666666666666664;
    q = q * z;
    q = q - 0.5;
    q = q * z;
    precise float c = 1.0 + q;
    precise float o = s / c;
    return o;
}

float csHexHue(float3 v, float maxv, float delta)
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

void csHueSector(float h, float sectors, int count, out int sector, out float fraction)
{
    precise float hs = h * sectors;
    const float fl = floor(hs);
    if (!(abs(fl) < 16777216.0))
    {
        sector = 0;
        fraction = 0.0;
        return;
    }
    precise float f = hs - fl;
    fraction = f;
    int k = int(fl) % count;
    if (k < 0)
        k += count;
    sector = k;
}

float3 csHexToRgb(float h, float hi, float c)
{
    int i;
    float f;
    csHueSector(h, 6.0, 6, i, f);
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

float3 csRgbToHsv(float3 v)
{
    const float maxv = max(max(v.r, v.g), v.b);
    const float minv = min(min(v.r, v.g), v.b);
    precise float delta = maxv - minv;
    const float h = csHexHue(v, maxv, delta);
    precise float s = 0.0;
    if (maxv != 0.0)
        s = delta / maxv;
    return float3(h, s, maxv);
}

float3 csHsvToRgb(float3 v)
{
    precise float c = v.z * v.y;
    return csHexToRgb(v.x, v.z, c);
}

float3 csRgbToHsl(float3 v)
{
    const float maxv = max(max(v.r, v.g), v.b);
    const float minv = min(min(v.r, v.g), v.b);
    precise float delta = maxv - minv;
    const float h = csHexHue(v, maxv, delta);
    precise float sum = maxv + minv;
    precise float l = sum * 0.5;
    precise float den = (l <= 0.5) ? sum : (2.0 - sum);
    precise float s = 0.0;
    if (den != 0.0)
        s = delta / den;
    return float3(h, s, l);
}

float3 csHslToRgb(float3 v)
{
    const float l = v.z;
    precise float sum = l + l;
    precise float den = (l <= 0.5) ? sum : (2.0 - sum);
    precise float c = den * v.y;
    precise float half = c * 0.5;
    precise float hi = l + half;
    return csHexToRgb(v.x, hi, c);
}

float3 csRgbToHsi(float3 v)
{
    const float minv = min(min(v.r, v.g), v.b);
    precise float rg = v.r + v.g;
    precise float sum = rg + v.b;
    precise float i = sum * 0.3333333432674408;
    precise float s = 0.0;
    if (i != 0.0)
    {
        precise float q = minv / i;
        s = 1.0 - q;
    }
    precise float x1 = v.r - v.g;
    precise float x2 = v.r - v.b;
    precise float x = x1 + x2;
    precise float gb = v.g - v.b;
    precise float y = gb * 1.7320508075688772;
    return float3(csAtan2Turns(y, x), s, i);
}

float3 csHsiToRgb(float3 v)
{
    const float i = v.z;
    precise float oneMinusS = 1.0 - v.y;
    precise float a = i * oneMinusS;
    precise float is = i * v.y;
    int k;
    float f;
    csHueSector(v.x, 3.0, 3, k, f);
    precise float fc = f - 0.5;
    precise float u = fc * 2.094395160675049;
    const float tanU = csTanNear(u);
    precise float st = tanU * 0.8660253882408142;
    precise float ratio = 0.5 - st;
    precise float isr = is * ratio;
    precise float c1 = i + isr;
    precise float threeI = i * 3.0;
    precise float pair = c1 + a;
    precise float rest = threeI - pair;
    switch (k)
    {
    case 0: return float3(c1, rest, a);
    case 1: return float3(a, c1, rest);
    default: return float3(rest, a, c1);
    }
}

float3 csRgbToLumaChroma(ColorspaceSide sd, float3 v)
{
    precise float y = sd.kr * v.r;
    precise float tg = sd.kg * v.g;
    y = y + tg;
    precise float tb = sd.kb * v.b;
    y = y + tb;
    precise float bd = v.b - y;
    precise float rd = v.r - y;
    precise float pb = bd * sd.cb;
    precise float pr = rd * sd.cr;
    precise float ys = y * sd.yScale;
    precise float o0 = ys + sd.yOffset;
    precise float o1 = pb + sd.cOffset;
    precise float o2 = pr + sd.cOffset;
    return float3(o0, o1, o2);
}

float3 csLumaChromaToRgb(ColorspaceSide sd, float3 v)
{
    precise float yd = v.x - sd.yOffset;
    precise float y = yd * sd.yInv;
    precise float pb = v.y - sd.cOffset;
    precise float pr = v.z - sd.cOffset;
    precise float bd = pb * sd.cbInv;
    precise float rd = pr * sd.crInv;
    precise float r = y + rd;
    precise float b = y + bd;
    precise float tr = sd.kr * r;
    precise float tb = sd.kb * b;
    precise float gy = y - tr;
    gy = gy - tb;
    precise float g = gy * sd.kgInv;
    return float3(r, g, b);
}

float csLabF(float t)
{
    if (t > 0.008856451679035631)
        return csCbrtPositive(t);
    precise float l = t * 7.787037037037037;
    precise float o = l + 0.13793103448275862;
    return o;
}

float csLabFInv(float u)
{
    if (u > 0.20689655172413793)
    {
        precise float u2 = u * u;
        precise float o = u2 * u;
        return o;
    }
    precise float d = u - 0.13793103448275862;
    precise float o = d * 0.12841854934601665;
    return o;
}

struct ColorspaceOp : IPixelOp
{
    ColorspaceSide inSide;
    ColorspaceSide outSide;
    float toXYZ[9];
    float fromXYZ[9];
    float3 white;
    float3 whiteInv;
    float2 whiteXy;

    // Parameter block, in words: the in and out models, the working space's matrices to and from
    // XYZ (row-major), its white XYZ, the white's reciprocals, its xy, then the in side's and the
    // out side's twelve luma/chroma coefficients.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        [ForceUnroll]
        for (uint i = 0; i < 9; ++i)
        {
            toXYZ[i] = wrapParamFloat(params, 2 + i);
            fromXYZ[i] = wrapParamFloat(params, 11 + i);
        }
        white = float3(wrapParamFloat(params, 20), wrapParamFloat(params, 21), wrapParamFloat(params, 22));
        whiteInv = float3(wrapParamFloat(params, 23), wrapParamFloat(params, 24), wrapParamFloat(params, 25));
        whiteXy = float2(wrapParamFloat(params, 26), wrapParamFloat(params, 27));
        inSide = loadSide(params, params[0], 28);
        outSide = loadSide(params, params[1], 40);
    }

    float3 mul3(bool fromXyz, float3 v)
    {
        float3 o;
        [ForceUnroll]
        for (uint r = 0; r < 3; ++r)
        {
            const float m0 = fromXyz ? fromXYZ[r * 3] : toXYZ[r * 3];
            const float m1 = fromXyz ? fromXYZ[r * 3 + 1] : toXYZ[r * 3 + 1];
            const float m2 = fromXyz ? fromXYZ[r * 3 + 2] : toXYZ[r * 3 + 2];
            precise float s = m0 * v.x;
            precise float g = m1 * v.y;
            s = s + g;
            precise float b = m2 * v.z;
            s = s + b;
            o[r] = s;
        }
        return o;
    }

    float3 xyzToLab(float3 v)
    {
        precise float x = v.x * whiteInv.x;
        precise float y = v.y * whiteInv.y;
        precise float z = v.z * whiteInv.z;
        const float fx = csLabF(x);
        const float fy = csLabF(y);
        const float fz = csLabF(z);
        precise float l = fy * 116.0;
        precise float dxy = fx - fy;
        precise float dyz = fy - fz;
        precise float o0 = l - 16.0;
        precise float o1 = dxy * 500.0;
        precise float o2 = dyz * 200.0;
        return float3(o0, o1, o2);
    }

    float3 labToXyz(float3 v)
    {
        precise float l = v.x + 16.0;
        precise float fy = l * 0.008620689655172414;
        precise float da = v.y * 0.002;
        precise float db = v.z * 0.005;
        precise float fx = fy + da;
        precise float fz = fy - db;
        precise float o0 = csLabFInv(fx) * white.x;
        precise float o1 = csLabFInv(fy) * white.y;
        precise float o2 = csLabFInv(fz) * white.z;
        return float3(o0, o1, o2);
    }

    float3 xyzToXyY(float3 v)
    {
        precise float xy = v.x + v.y;
        precise float sum = xy + v.z;
        const float bigY = v.y;
        if (bigY == 0.0 || sum == 0.0)
            return float3(whiteXy.x, whiteXy.y, bigY);
        precise float x = v.x / sum;
        precise float y = bigY / sum;
        return float3(x, y, bigY);
    }

    float3 xyYToXyz(float3 v)
    {
        const float bigY = v.z;
        if (v.y == 0.0)
            return float3(0.0, bigY, 0.0);
        precise float k = bigY / v.y;
        precise float oneMinusX = 1.0 - v.x;
        precise float zc = oneMinusX - v.y;
        precise float x = v.x * k;
        precise float z = zc * k;
        return float3(x, bigY, z);
    }

    float3 toReference(ColorspaceSide sd, float3 v)
    {
        switch (sd.model)
        {
        case kModelRGB:
        case kModelXYZ:
            return v;
        case kModelHSV:
            return csHsvToRgb(v);
        case kModelHSL:
            return csHslToRgb(v);
        case kModelHSI:
            return csHsiToRgb(v);
        case kModelXyY:
            return xyYToXyz(v);
        case kModelLab:
            return labToXyz(v);
        default:
            return csLumaChromaToRgb(sd, v);
        }
    }

    float3 fromReference(ColorspaceSide sd, float3 v)
    {
        switch (sd.model)
        {
        case kModelRGB:
        case kModelXYZ:
            return v;
        case kModelHSV:
            return csRgbToHsv(v);
        case kModelHSL:
            return csRgbToHsl(v);
        case kModelHSI:
            return csRgbToHsi(v);
        case kModelXyY:
            return xyzToXyY(v);
        case kModelLab:
            return xyzToLab(v);
        default:
            return csRgbToLumaChroma(sd, v);
        }
    }

    float4 apply(float4 v, int2 xy)
    {
        if (inSide.model == outSide.model)
            return v;
        float3 c = toReference(inSide, v.rgb);
        const bool inXyz = inSide.model >= kModelXYZ;
        const bool outXyz = outSide.model >= kModelXYZ;
        if (inXyz != outXyz)
            c = mul3(inXyz, c);
        c = fromReference(outSide, c);
        return float4(c, v.a);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void colorspace_wrapped(uint3 tid : SV_DispatchThreadID)
{
    ColorspaceOp op;
    runWrapped<ColorspaceOp>(op, tid);
}
