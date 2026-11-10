import pixelwrap;

// ChromaKeyKernel::process() in Engine/Nodes/Keyer/ChromaKeyer.cpp per pixel, in float where the
// CPU keys partly in double, run through the wrap's stages. The parameter block carries the
// per-frame constants the host derives and the Bg view; Bg is the input bound after it.
// ChromaKeyGpuParams in ChromaKeyer.cpp mirrors the block's words.

[[vk::binding(4, 0)]] StructuredBuffer<float> ckBg;

static const uint kCkAcceptAll = 1;
static const uint kCkSuppressAll = 2;
static const uint kCkSourceHasAlpha = 4;

static const uint kCkGainRamp = 0;
static const uint kCkGainNonPositive = 1;
static const uint kCkLiftFull = 2;

static const uint kCkShowIntermediate = 0;
static const uint kCkShowPremultiplied = 1;
static const uint kCkShowUnpremultiplied = 2;
static const uint kCkShowComposite = 3;

static const uint kCkSourceAlphaInsideMask = 1;
static const uint kCkSourceAlphaNormal = 2;

struct CkView
{
    uint offset;
    uint stride;
    int x1;
    int y1;
    int x2;
    int y2;
    uint comps;
}

struct ChromaKey
{
    float cosKey;
    float sinKey;
    float ys;
    float tanAcceptance;
    float tanSuppression;
    float keyLift;
    float oneMinusLift;
    float gainXKey;
    float kr;
    float kb;
    float kg;
    float twoOneMinusKb;
    float twoOneMinusKr;
    uint flags;
    uint gainMode;
    uint show;
    uint sourceAlpha;
    CkView bg;
}

CkView ckLoadView(StructuredBuffer<uint> params, uint i)
{
    CkView v;
    v.offset = params[i];
    v.stride = params[i + 1];
    v.x1 = int(params[i + 2]);
    v.y1 = int(params[i + 3]);
    v.x2 = int(params[i + 4]);
    v.y2 = int(params[i + 5]);
    v.comps = params[i + 6];
    return v;
}

// Parameter block, in words: the thirteen floats in member order, flags, gainMode, show,
// sourceAlpha, then the Bg view (offset, stride, x1, y1, x2, y2, comps).
ChromaKey ckLoad(StructuredBuffer<uint> params)
{
    ChromaKey k;
    k.cosKey = wrapParamFloat(params, 0);
    k.sinKey = wrapParamFloat(params, 1);
    k.ys = wrapParamFloat(params, 2);
    k.tanAcceptance = wrapParamFloat(params, 3);
    k.tanSuppression = wrapParamFloat(params, 4);
    k.keyLift = wrapParamFloat(params, 5);
    k.oneMinusLift = wrapParamFloat(params, 6);
    k.gainXKey = wrapParamFloat(params, 7);
    k.kr = wrapParamFloat(params, 8);
    k.kb = wrapParamFloat(params, 9);
    k.kg = wrapParamFloat(params, 10);
    k.twoOneMinusKb = wrapParamFloat(params, 11);
    k.twoOneMinusKr = wrapParamFloat(params, 12);
    k.flags = params[13];
    k.gainMode = params[14];
    k.show = params[15];
    k.sourceAlpha = params[16];
    k.bg = ckLoadView(params, 17);
    return k;
}

// std::min and std::max, whose NaN handling GLSL min and max leave undefined.
float ckMin(float a, float b)
{
    return (b < a) ? b : a;
}

float ckMax(float a, float b)
{
    return (a < b) ? b : a;
}

float ckClamp(float v, float lo, float hi)
{
    return ckMax(lo, ckMin(v, hi));
}

// ColorMath::boundForKeying: FLT_MAX / 4.
float ckBound(float v)
{
    const float bound = asfloat(0x7e7fffffu);
    if (v > bound)
        return bound;
    if (v < -bound)
        return -bound;
    return v;
}

void ckToYPbPr(ChromaKey k, float r, float g, float b, out float y, out float pb, out float pr)
{
    precise float yy = k.kr * r + k.kg * g + k.kb * b;
    precise float cb = (b - yy) / k.twoOneMinusKb;
    precise float cr = (r - yy) / k.twoOneMinusKr;
    y = yy;
    pb = cb;
    pr = cr;
}

void ckToRGB(ChromaKey k, float y, float pb, float pr, out float r, out float g, out float b)
{
    precise float bb = pb * k.twoOneMinusKb + y;
    precise float rr = pr * k.twoOneMinusKr + y;
    precise float gg = (y - k.kr * rr - k.kb * bb) / k.kg;
    r = rr;
    g = gg;
    b = bb;
}

bool ckInside(CkView v, int2 p)
{
    return wrapInside(p, v.x1, v.y1, v.x2, v.y2);
}

// readColorRow(): zero outside the view; one component is alpha alone, two or three have no
// alpha, which reads one.
float4 ckReadBg(CkView v, int2 p)
{
    float4 c = float4(0.0, 0.0, 0.0, 0.0);
    if (!ckInside(v, p))
        return c;
    const uint base = v.offset + uint(p.y - v.y1) * v.stride + uint(p.x - v.x1) * v.comps;
    for (uint i = 0; i < v.comps && i < 4; ++i)
        c[wrapChannelBit(v.comps, i)] = ckBg[base + i];
    if (v.comps == 2 || v.comps == 3)
        c.a = 1.0;
    return c;
}

// ChromaKeyKernel::process() with fg present when hasSrc.
float4 ckProcess(ChromaKey k, bool hasSrc, float4 fg, float4 bg)
{
    const bool fgHasAlpha = (k.flags & kCkSourceHasAlpha) != 0;
    float inMask = 0.0;
    if (k.sourceAlpha == kCkSourceAlphaInsideMask && hasSrc && fgHasAlpha)
        inMask = ckClamp(fg.a, 0.0, 1.0);
    float Kbg = 0.0;

    float fgr = hasSrc ? ckBound(fg.r) : 0.0;
    float fgg = hasSrc ? ckBound(fg.g) : 0.0;
    float fgb = hasSrc ? ckBound(fg.b) : 0.0;

    if (!hasSrc)
    {
        Kbg = 1.0;
        fgr = 0.0;
        fgg = 0.0;
        fgb = 0.0;
    }
    else
    {
        float fgy, fgcb, fgcr;
        ckToYPbPr(k, fgr, fgg, fgb, fgy, fgcb, fgcr);

        precise float fgcbp = fgcb * 2.0;
        precise float fgcrp = fgcr * 2.0;
        precise float fgx = k.cosKey * fgcbp + k.sinKey * fgcrp;
        precise float fgz = -k.sinKey * fgcbp + k.cosKey * fgcrp;
        const float az = abs(fgz);

        float Kfg;
        if (fgx <= 0.0 || ((k.flags & kCkAcceptAll) != 0 && fgx >= 0.0) || az / fgx > k.tanAcceptance)
        {
            Kfg = 0.0;
        }
        else
        {
            precise float d = fgx - az / k.tanAcceptance;
            Kfg = k.tanAcceptance > 0.0 ? d : 0.0;
        }
        float fgxScaled = fgx;

        float KfgNew = Kfg;
        precise float inLimit = 1.0 - inMask;
        if (inMask > 0.0 && Kfg > inLimit)
            KfgNew = inLimit;
        // Written as a negated equality: a NaN key differs from zero, as on the CPU.
        if (!(Kfg == 0.0))
        {
            precise float s = KfgNew + az / k.tanAcceptance;
            fgxScaled = s;
        }
        Kfg = KfgNew;

        if (k.show != kCkShowIntermediate)
        {
            precise float residual = fgxScaled - az / k.tanSuppression;
            if (fgxScaled > 0.0 && ((k.flags & kCkSuppressAll) != 0 || residual > 0.0))
            {
                fgcb = 0.0;
                fgcr = 0.0;
            }
            else
            {
                precise float cb = fgcb - Kfg * k.cosKey / 2.0;
                precise float cr = fgcr - Kfg * k.sinKey / 2.0;
                fgcb = cb;
                fgcr = cr;
            }

            precise float y = fgy - k.ys * Kfg;
            ckToRGB(k, y, fgcb, fgcr, fgr, fgg, fgb);
        }

        if (k.gainMode == kCkGainNonPositive)
        {
            Kbg = (Kfg > 0.0) ? 1.0 : 0.0;
        }
        else if (k.gainMode == kCkLiftFull)
        {
            Kbg = (Kfg >= k.gainXKey) ? 1.0 : 0.0;
        }
        else
        {
            precise float ramp = (Kfg / k.gainXKey - k.keyLift) / k.oneMinusLift;
            Kbg = ckBound(ramp);
        }
    }

    precise float fga = 1.0 - Kbg;
    const float compAlpha = (k.show == kCkShowComposite && k.sourceAlpha == kCkSourceAlphaNormal && hasSrc) ? (fgHasAlpha ? fg.a : 1.0) : 1.0;
    float4 o;
    if (k.show == kCkShowIntermediate)
    {
        o.rgb = hasSrc ? fg.rgb : float3(0.0, 0.0, 0.0);
    }
    else if (k.show == kCkShowPremultiplied)
    {
        o.rgb = float3(fgr, fgg, fgb);
    }
    else if (k.show == kCkShowUnpremultiplied)
    {
        if (fga == 0.0)
        {
            o.rgb = float3(1.0, 1.0, 1.0);
        }
        else
        {
            precise float3 q = float3(fgr, fgg, fgb) / fga;
            o.rgb = q;
        }
    }
    else
    {
        precise float3 c = compAlpha * (float3(fgr, fgg, fgb) + bg.rgb * Kbg) + (1.0 - compAlpha) * bg.rgb;
        o.rgb = c;
    }
    o.a = fga;
    return o;
}

struct ChromaKeyerOp : IPixelOp
{
    ChromaKey k;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        k = ckLoad(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        const bool hasSrc = wrapInside(xy, wrap.srcX1, wrap.srcY1, wrap.srcX2, wrap.srcY2);
        return ckProcess(k, hasSrc, v, ckReadBg(k.bg, xy));
    }
};

[shader("compute")]
[numthreads(8, 8, 1)]
void chromakeyer_wrapped(uint3 tid : SV_DispatchThreadID)
{
    ChromaKeyerOp op;
    runWrapped<ChromaKeyerOp>(op, tid);
}
