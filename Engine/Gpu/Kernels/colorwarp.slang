import pixelwrap;

// The parameter words ColorWarpOp::gpu() packs; keep the two layouts in step.
static const uint CW_PIN_COUNT = 0;
static const uint CW_KERNEL = 1;
static const uint CW_TRANSFER = 2;
static const uint CW_LIGHTNESS_STRETCH = 3;
static const uint CW_LIGHTNESS_AXIS = 4;
static const uint CW_ORIGIN = 7;
static const uint CW_INV_SCALE = 10;
static const uint CW_CUTOFF = 11;
static const uint CW_PRE = 12;
static const uint CW_POST = 21;
static const uint CW_INV_POST = 30;
static const uint CW_INV_PRE = 39;
static const uint CW_AFFINE = 48;
static const uint CW_WEIGHT_SUM = 60;
static const uint CW_WEIGHT_MOMENT = 63;
static const uint CW_FADE_RADIUS = 72;
static const uint CW_FADE_INV_WIDTH = 73;
static const uint CW_HEADER_WORDS = 74;
static const uint CW_PIN_WORDS = 7;

static const uint CW_KERNEL_GAUSSIAN = 0;
static const uint CW_KERNEL_RBF_CUBIC = 2;

static const uint CW_TRANSFER_LOG = 1;
static const uint CW_TRANSFER_CBRT = 2;

// colorWarpLogEncode()'s toe: log2 above 2^-7, the tangent line below it.
static const float CW_LOG_TOE = 0.0078125;
static const float CW_LOG_TOE_ENCODED = -7.0;
static const float CW_LOG_TOE_SLOPE = 184.66496523378731;

static const float CW_GAUSSIAN_EXACT_HIT = 1e-30;

float3x3 cwMatrix(uint base)
{
    return float3x3(wrapParamFloat(wrapAux, base + 0), wrapParamFloat(wrapAux, base + 1), wrapParamFloat(wrapAux, base + 2),
                    wrapParamFloat(wrapAux, base + 3), wrapParamFloat(wrapAux, base + 4), wrapParamFloat(wrapAux, base + 5),
                    wrapParamFloat(wrapAux, base + 6), wrapParamFloat(wrapAux, base + 7), wrapParamFloat(wrapAux, base + 8));
}

float3 cwVector(uint base)
{
    return float3(wrapParamFloat(wrapAux, base), wrapParamFloat(wrapAux, base + 1), wrapParamFloat(wrapAux, base + 2));
}

float cwLogEncode(float x)
{
    if (x >= CW_LOG_TOE)
        return log2(x);
    return CW_LOG_TOE_ENCODED + (x - CW_LOG_TOE) * CW_LOG_TOE_SLOPE;
}

float cwLogDecode(float y)
{
    if (y >= CW_LOG_TOE_ENCODED)
        return exp2(y);
    return CW_LOG_TOE + (y - CW_LOG_TOE_ENCODED) / CW_LOG_TOE_SLOPE;
}

float cwCbrt(float x)
{
    return sign(x) * pow(abs(x), 1.0 / 3.0);
}

struct ColorWarpOp : IPixelOp
{
    uint pinCount;
    uint kernel;
    uint transfer;
    float lightnessStretch;
    float3 lightnessAxis;
    float3 origin;
    float invScale;
    float cutoff;
    float fadeRadius;
    float fadeInvWidth;
    float3x3 pre;
    float3x3 post;
    float3x3 invPost;
    float3x3 invPre;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        pinCount = params[CW_PIN_COUNT];
        kernel = params[CW_KERNEL];
        transfer = params[CW_TRANSFER];
        lightnessStretch = wrapParamFloat(params, CW_LIGHTNESS_STRETCH);
        lightnessAxis = cwVector(CW_LIGHTNESS_AXIS);
        origin = cwVector(CW_ORIGIN);
        invScale = wrapParamFloat(params, CW_INV_SCALE);
        cutoff = wrapParamFloat(params, CW_CUTOFF);
        fadeRadius = wrapParamFloat(params, CW_FADE_RADIUS);
        fadeInvWidth = wrapParamFloat(params, CW_FADE_INV_WIDTH);
        pre = cwMatrix(CW_PRE);
        post = cwMatrix(CW_POST);
        invPost = cwMatrix(CW_INV_POST);
        invPre = cwMatrix(CW_INV_PRE);
    }

    float3 toWarp(float3 rgb)
    {
        float3 m = mul(pre, rgb);
        [ForceUnroll]
        for (uint c = 0; c < 3; ++c)
        {
            if (transfer == CW_TRANSFER_LOG)
                m[c] = cwLogEncode(m[c]);
            else if (transfer == CW_TRANSFER_CBRT)
                m[c] = cwCbrt(m[c]);
        }
        return mul(post, m);
    }

    float3 fromWarp(float3 w)
    {
        float3 m = mul(invPost, w);
        [ForceUnroll]
        for (uint c = 0; c < 3; ++c)
        {
            if (transfer == CW_TRANSFER_LOG)
                m[c] = cwLogDecode(m[c]);
            else if (transfer == CW_TRANSFER_CBRT)
                m[c] = m[c] * m[c] * m[c];
        }
        return mul(invPre, m);
    }

    float3 gaussian(float3 w)
    {
        float sumW = 0.0;
        float3 acc = float3(0.0);
        for (uint i = 0; i < pinCount; ++i)
        {
            const uint base = CW_HEADER_WORDS + i * CW_PIN_WORDS;
            const float3 d = w - cwVector(base);
            const float3 di = cwVector(base + 3);
            const float inv2s2 = wrapParamFloat(wrapAux, base + 6);
            const float along = dot(d, lightnessAxis);
            const float r2 = max(0.0, dot(d, d) + lightnessStretch * along * along);
            const float t = r2 * inv2s2;
            if (r2 == 0.0 || (inv2s2 > 0.0 && t < CW_GAUSSIAN_EXACT_HIT))
                return di;
            if (inv2s2 <= 0.0 || t >= cutoff)
                continue;
            const float wi = exp(-t) / (2.0 * t);
            sumW += wi;
            acc += wi * di;
        }
        if (sumW > 0.0)
            return acc / (1.0 + sumW);
        return float3(0.0);
    }

    // colorWarpFade(): 1 up to fadeRadius, then 1 - smoothstep to 0 one fade width further.
    float fade(float R)
    {
        if (!(R > fadeRadius))
            return 1.0;
        if (fadeInvWidth <= 0.0)
            return 0.0;
        const float t = (R - fadeRadius) * fadeInvWidth;
        if (!(t < 1.0))
            return 0.0;
        return 1.0 - t * t * (3.0 - 2.0 * t);
    }

    // With R = |x|, s = |c|^2 - 2 x.c = r^2 - R^2 and r = |x - c|:
    //   r^3 = R^3 - 3 R x.c + 1.5 R |c|^2 + s^2 (2 r + R) / (2 (r + R)^2)
    //   -r = -R - s / (r + R)
    // The R^3, R x.c and R terms summed over the pins are the weight sums times R, which keeps
    // float from cancelling terms of order R^3 far outside the pins.
    float3 rbf(float3 w)
    {
        const float3 x = (w - origin) * invScale;
        const float R = length(x);
        const float weight = fade(R);
        if (weight <= 0.0)
            return float3(0.0);
        const bool cubic = (kernel == CW_KERNEL_RBF_CUBIC);
        const float3 weightSum = cwVector(CW_WEIGHT_SUM);
        const float3 momentDotX = mul(cwMatrix(CW_WEIGHT_MOMENT), x);
        float3 d;
        [ForceUnroll]
        for (uint c = 0; c < 3; ++c)
        {
            d[c] = wrapParamFloat(wrapAux, CW_AFFINE + c) + wrapParamFloat(wrapAux, CW_AFFINE + 3 + c * 3) * x.x +
                   wrapParamFloat(wrapAux, CW_AFFINE + 4 + c * 3) * x.y + wrapParamFloat(wrapAux, CW_AFFINE + 5 + c * 3) * x.z;
        }
        d += cubic ? (weightSum * (R * R * R) - 3.0 * R * momentDotX) : (-R * weightSum);
        for (uint i = 0; i < pinCount; ++i)
        {
            const uint base = CW_HEADER_WORDS + i * CW_PIN_WORDS;
            const float3 c = cwVector(base);
            const float cc = dot(c, c);
            const float s = cc - 2.0 * dot(x, c);
            const float r = length(x - c);
            const float rR = r + R;
            float phi;
            if (cubic)
                phi = 1.5 * R * cc + ((rR > 0.0) ? s * s * (2.0 * r + R) / (2.0 * rR * rR) : 0.0);
            else
                phi = (rR > 0.0) ? -s / rR : 0.0;
            d += cwVector(base + 3) * phi;
        }
        return d * weight;
    }

    float4 apply(float4 v, int2 xy)
    {
        if (pinCount == 0 || any(isnan(v.rgb)) || any(isinf(v.rgb)))
            return v;
        float3 w = toWarp(v.rgb);
        const float3 d = (kernel == CW_KERNEL_GAUSSIAN) ? gaussian(w) : rbf(w);
        if (all(d == float3(0.0)))
            return v;
        return float4(fromWarp(w + d), v.w);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void colorwarp_wrapped(uint3 tid : SV_DispatchThreadID)
{
    ColorWarpOp op;
    runWrapped<ColorWarpOp>(op, tid);
}
