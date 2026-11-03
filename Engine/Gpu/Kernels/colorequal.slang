// ColorEqualizer: darktable's color equalizer (src/iop/colorequal.c and data/kernels/colorequal.cl,
// release-5.6.2, GPL-3.0-or-later), the twin of ColorEqualizer::process() for a matrix working space.
// The passes between these kernels (the guided filters' blurs and resampling) are EdgeAwareGpu's.
// Every plane is tightly packed and top-down, width x height pixels, except the source (the fill,
// bottom-up, nComps floats per pixel over the same domain) and the destination (the filtered image).
// ColorEqualizerGpuParams in Engine/Nodes/Color/ColorEqualizer.cpp mirrors CEParams; every entry point
// binds the parameter words, the source, ten role buffers and the destination, in that order.
import dtcolorspaces;

StructuredBuffer<uint> prm;
StructuredBuffer<uint> src;
RWStructuredBuffer<uint> r0;
RWStructuredBuffer<uint> r1;
RWStructuredBuffer<uint> r2;
RWStructuredBuffer<uint> r3;
RWStructuredBuffer<uint> r4;
RWStructuredBuffer<uint> r5;
RWStructuredBuffer<uint> r6;
RWStructuredBuffer<uint> r7;
RWStructuredBuffer<uint> r8;
RWStructuredBuffer<uint> r9;
RWStructuredBuffer<uint> dst;

struct CEParams
{
    uint width;
    uint height;
    uint dsWidth;
    uint dsHeight;
    uint nComps;
    uint flags;
    uint output;
    uint outOffset;
    uint outStride;
    int outX0;
    int outY0;
    uint outWidth;
    uint outHeight;
    int frameX;
    int frameY;
    int frameWidth;
    int frameHeight;
    float white;
    float satShift;
    float brightShift;
    float gradientAmp;
    float eps;
};

[[vk::push_constant]] ConstantBuffer<CEParams> params;

// The parameter words, as ColorEqualizer::recordGpu() packs them. Matrices are row-major.
static const uint kToPipe = 0;
static const uint kFromPipe = 9;
static const uint kInputMatrix = 18;
static const uint kOutputMatrix = 27;
static const uint kLutHue = 36;
static const uint kLutSaturation = kLutHue + 512;
static const uint kLutBrightness = kLutSaturation + 512;
static const uint kGamut = kLutBrightness + 512;
static const uint kSatWeights = kGamut + 512;

static const uint kUseFilter = 1;
static const uint kGuided = 2;
static const uint kPassThrough = 4;

static const uint kOutputResult = 0;
static const uint kOutputHue = 1;
static const uint kOutputSaturation = 2;
static const uint kOutputBrightness = 3;
static const uint kOutputSaturationThreshold = 4;
static const uint kOutputBrightnessThreshold = 5;

static const int kSatSize = 4096;
static const float kNormMin = 1.52587890625e-05;
static const float kSatEffect = 2.0;
static const float kBrightEffect = 8.0;

float3 loadFloat3(uint i)
{
    return float3(asfloat(prm[i]), asfloat(prm[i + 1]), asfloat(prm[i + 2]));
}

float3 applyMatrix(uint at, float3 v)
{
    return dtMul(loadFloat3(at), loadFloat3(at + 3), loadFloat3(at + 6), v);
}

float rd(RWStructuredBuffer<uint> b, uint i)
{
    return asfloat(b[i]);
}

void wr(RWStructuredBuffer<uint> b, uint i, float v)
{
    b[i] = asuint(v);
}

// darktable's _get_satweight().
float satWeight(float sat)
{
    const float isat = float(kSatSize) * (1.0 + clamp(sat, -1.0, 1.0 - 1.0 / float(kSatSize)));
    const float base = floor(isat);
    const uint i = uint(int(base));
    const float w0 = asfloat(prm[kSatWeights + i]);
    const float w1 = asfloat(prm[kSatWeights + i + 1]);
    return w0 + (isat - base) * (w1 - w0);
}

// a = c inverse(cov + eps on the diagonal), 0 where that is singular.
float4 solveA(float4 cov, float4 c, float eps)
{
    const float s0 = cov.x + eps;
    const float s1 = cov.y;
    const float s2 = cov.z;
    const float s3 = cov.w + eps;
    const float det = s0 * s3 - s1 * s2;
    if (abs(det) <= 4.0 * 1.192092896e-07)
        return float4(0.0);
    const float i0 = s3 / det;
    const float i1 = -s1 / det;
    const float i2 = -s2 / det;
    const float i3 = s0 / det;
    return float4(c.x * i0 + c.y * i1, c.x * i2 + c.y * i3, c.z * i0 + c.w * i1, c.z * i2 + c.w * i3);
}

float4 load4(RWStructuredBuffer<uint> b, uint k)
{
    return float4(rd(b, 4 * k), rd(b, 4 * k + 1), rd(b, 4 * k + 2), rd(b, 4 * k + 3));
}

float2 load2(RWStructuredBuffer<uint> b, uint k)
{
    return float2(rd(b, 2 * k), rd(b, 2 * k + 1));
}

void store4(RWStructuredBuffer<uint> b, uint k, float4 v)
{
    wr(b, 4 * k, v.x);
    wr(b, 4 * k + 1, v.y);
    wr(b, 4 * k + 2, v.z);
    wr(b, 4 * k + 3, v.w);
}

void store2(RWStructuredBuffer<uint> b, uint k, float2 v)
{
    wr(b, 2 * k, v.x);
    wr(b, 2 * k + 1, v.y);
}

// darktable's gamut_map_HSB().
float3 gamutMapHSB(float3 hsb, float white)
{
    const float3 jch = dtUcsHSBToJCH(hsb);
    const float maxColorfulness = dtLookupGamut(prm, kGamut, jch.z);
    const float maxChroma = dtUcsChroma(jch.x * white, white, maxColorfulness);
    const float3 boundary = dtUcsJCHToHSB(float3(jch.x, maxChroma, jch.z));
    return float3(hsb.x, dtSoftClip(hsb.y, 0.8 * boundary.y, boundary.y), hsb.z);
}

// The source pixel at top-down (x, y), in linear Rec.2020.
float3 sourceRgb(uint x, uint y)
{
    const uint i = ((params.height - 1 - y) * params.width + x) * params.nComps;
    return applyMatrix(kToPipe, float3(asfloat(src[i]), asfloat(src[i + 1]), asfloat(src[i + 2])));
}

// UV (r0), the raw saturation (r1) and L* (r2).
[shader("compute")]
[numthreads(8, 8, 1)]
void ce_prepare(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint k = tid.y * params.width + tid.x;
    const float3 pix = sourceRgb(tid.x, tid.y);
    const float3 xyY = dtD65XyzToXyY(applyMatrix(kInputMatrix, pix));
    const float dmin = min(pix.x, min(pix.y, pix.z));
    const float dmax = max(pix.x, max(pix.y, pix.z));
    const float delta = dmax - dmin;
    wr(r1, k, (dmax > kNormMin && delta > kNormMin) ? delta / dmax : 0.0);
    store2(r0, k, dtXyYToUcsUV(xyY));
    wr(r2, k, dtYToUcsLStar(xyY.z));
}

// The covariance products (r1) of UV (r0), over the downsampled grid.
[shader("compute")]
[numthreads(8, 8, 1)]
void ce_covariance(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.dsWidth || tid.y >= params.dsHeight)
        return;
    const uint k = tid.y * params.dsWidth + tid.x;
    const float2 uv = load2(r0, k);
    store4(r1, k, float4(uv.x * uv.x, uv.x * uv.y, uv.x * uv.y, uv.y * uv.y));
}

// The chromaticity prefilter's a (r2) and b (r3) from the mean UV (r0) and products (r1).
[shader("compute")]
[numthreads(8, 8, 1)]
void ce_prefilter_ab(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.dsWidth || tid.y >= params.dsHeight)
        return;
    const uint k = tid.y * params.dsWidth + tid.x;
    const float2 uv = load2(r0, k);
    const float4 cov = load4(r1, k) - float4(uv.x * uv.x, uv.x * uv.y, uv.x * uv.y, uv.y * uv.y);
    const float4 a = solveA(cov, cov, params.eps);
    store4(r2, k, a);
    store2(r3, k, float2(uv.x - a.x * uv.x - a.y * uv.y, uv.y - a.z * uv.x - a.w * uv.y));
}

// The filtered UV into r0, from the guide UV (r4), the saturation (r1), a (r2) and b (r3).
[shader("compute")]
[numthreads(8, 8, 1)]
void ce_prefilter_apply(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint k = tid.y * params.width + tid.x;
    const float2 uv = load2(r4, k);
    const float4 a = load4(r2, k);
    const float2 b = load2(r3, k);
    const float2 cv = float2(a.x * uv.x + a.y * uv.y + b.x, a.z * uv.x + a.w * uv.y + b.y);
    const float w = satWeight(rd(r1, k) - params.satShift);
    store2(r0, k, w * (cv - uv) + uv);
}

float scharr(uint row, uint col)
{
    const uint w = params.width;
    const uint c = row * w + col;
    const float pnw = rd(r1, c - w - 1);
    const float pn = rd(r1, c - w);
    const float pne = rd(r1, c - w + 1);
    const float pw = rd(r1, c - 1);
    const float pe = rd(r1, c + 1);
    const float psw = rd(r1, c + w - 1);
    const float ps = rd(r1, c + w);
    const float pse = rd(r1, c + w + 1);
    const float gx = 47.0 / 255.0 * (pnw - pne + psw - pse) + 162.0 / 255.0 * (pw - pe);
    const float gy = 47.0 / 255.0 * (pnw - psw + pne - pse) + 162.0 / 255.0 * (pn - ps);
    return dtFastHypot(gx, gy);
}

// From UV (r0), the saturation (r1) and L* (r2): HSB (r3, 4 floats), the hue and saturation
// corrections (r4), the brightness correction (r5) and the saturation gradient (r6).
[shader("compute")]
[numthreads(8, 8, 1)]
void ce_corrections(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint k = tid.y * params.width + tid.x;
    const float2 uv = load2(r0, k);
    const float lStar = rd(r2, k);
    const float3 jch = float3(lStar / params.white, dtUcsChroma(lStar, params.white, uv.x * uv.x + uv.y * uv.y), atan2(uv.y, uv.x));
    const float3 hsb = dtUcsJCHToHSB(jch);
    store4(r3, k, float4(hsb, 0.0));
    float g = 0.0;
    if ((params.flags & kUseFilter) != 0 && params.width >= 3 && params.height >= 3)
    {
        const uint vrow = clamp(tid.y, 1u, params.height - 2);
        const uint vcol = clamp(tid.x, 1u, params.width - 2);
        g = params.gradientAmp * (max(0.0, scharr(vrow, vcol) - 0.02) * max(0.0, scharr(vrow, vcol) - 0.02));
    }
    wr(r6, k, g);
    if (jch.y > kNormMin)
    {
        store2(r4, k, float2(dtLookupGamut(prm, kLutHue, hsb.x), dtLookupGamut(prm, kLutSaturation, hsb.x)));
        wr(r5, k, hsb.y * (dtLookupGamut(prm, kLutBrightness, hsb.x) - 1.0));
    }
    else
    {
        store2(r4, k, float2(0.0, 1.0));
        wr(r5, k, 0.0);
    }
}

// The guide's products: the covariance (r3) of UV (r0) and the correlations (r4) of UV with the
// saturation correction (r1's second channel) and the brightness correction (r2).
[shader("compute")]
[numthreads(8, 8, 1)]
void ce_guide_init(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.dsWidth || tid.y >= params.dsHeight)
        return;
    const uint k = tid.y * params.dsWidth + tid.x;
    const float2 uv = load2(r0, k);
    const float c = rd(r1, 2 * k + 1);
    const float bc = rd(r2, k);
    store4(r3, k, float4(uv.x * uv.x, uv.x * uv.y, uv.x * uv.y, uv.y * uv.y));
    store4(r4, k, float4(uv.x * c, uv.y * c, uv.x * bc, uv.y * bc));
}

// The guide's a (r5) and b (r6) from the means of UV (r0), its products (r1), the corrections
// (r2, r3) and the correlations (r4).
[shader("compute")]
[numthreads(8, 8, 1)]
void ce_guide_ab(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.dsWidth || tid.y >= params.dsHeight)
        return;
    const uint k = tid.y * params.dsWidth + tid.x;
    const float2 uv = load2(r0, k);
    const float c = rd(r2, 2 * k + 1);
    const float bc = rd(r3, k);
    const float4 cov = load4(r1, k) - float4(uv.x * uv.x, uv.x * uv.y, uv.x * uv.y, uv.y * uv.y);
    const float4 correl = load4(r4, k) - float4(uv.x * c, uv.y * c, uv.x * bc, uv.y * bc);
    const float4 a = solveA(cov, correl, params.eps);
    store4(r5, k, a);
    store2(r6, k, float2(c - a.x * uv.x - a.y * uv.y, bc - a.z * uv.x - a.w * uv.y));
}

// The output over the output rect, from HSB (r0), the guide UV (r1), the hue correction (r2's first
// channel), the unguided corrections (r3, r4), the guide's a and b (r5, r6), the saturation (r7)
// and the blurred gradient (r8).
[shader("compute")]
[numthreads(8, 8, 1)]
void ce_final(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = params.nComps;
    const uint dx = uint(params.outX0) + tid.x;
    const uint by = uint(params.outY0) + tid.y;
    const uint srcIndex = (by * params.width + dx) * n;
    const uint o = params.outOffset + tid.y * params.outStride + tid.x * n;
    if ((params.flags & kPassThrough) != 0)
    {
        for (uint c = 0; c < n; ++c)
            dst[o + c] = src[srcIndex + c];
        return;
    }
    const uint row = params.height - 1 - by;
    const uint k = row * params.width + dx;
    const float3 hsb0 = load4(r0, k).xyz;
    const float sat = rd(r7, k);
    const float hueCorr = rd(r2, 2 * k);
    float satCorr = rd(r3, 2 * k + 1);
    float bCorr = rd(r4, k);
    if ((params.flags & kGuided) != 0)
    {
        const float2 uv = load2(r1, k);
        const float4 a = load4(r5, k);
        const float2 b = load2(r6, k);
        const float cv0 = a.x * uv.x + a.y * uv.y + b.x;
        const float cv1 = a.z * uv.x + a.w * uv.y + b.y;
        satCorr = 1.0 + (cv0 - 1.0) * satWeight(sat - params.satShift);
        bCorr = cv1 * (1.0 - clamp(rd(r8, k), 0.0, 1.0)) * satWeight(sat - params.brightShift);
    }

    float3 rgb;
    if (params.output == kOutputResult)
    {
        float3 hsb = float3(hsb0.x + hueCorr,
                            max(0.0, hsb0.y * (1.0 + kSatEffect * (satCorr - 1.0))),
                            max(0.0, hsb0.z * (1.0 + kBrightEffect * bCorr)));
        hsb = gamutMapHSB(hsb, params.white);
        const float3 xyz = dtXyYToXyz(dtUcsJCHToXyY(dtUcsHSBToJCH(hsb), params.white));
        rgb = applyMatrix(kFromPipe, applyMatrix(kOutputMatrix, xyz));
    }
    else
    {
        const float val = sqrt(hsb0.z * params.white);
        float corr;
        if (params.output == kOutputBrightness)
            corr = kBrightEffect * bCorr;
        else if (params.output == kOutputSaturation)
            corr = kSatEffect * (satCorr - 1.0);
        else if (params.output == kOutputBrightnessThreshold)
            corr = satWeight(sat - params.brightShift) - 0.5;
        else if (params.output == kOutputSaturationThreshold)
            corr = satWeight(sat - params.satShift) - 0.5;
        else
            corr = 0.2 * hueCorr;
        const bool neg = corr < 0.0;
        corr = abs(corr);
        corr = (corr < 2e-3) ? 0.0 : corr;
        rgb = float3(max(0.0, neg ? val - corr : val), max(0.0, val - corr), max(0.0, neg ? val : val - corr));
        const float grad = rd(r8, k);
        if (params.output == kOutputBrightness && (params.flags & kUseFilter) != 0 && grad > 0.1)
            rgb = float3(0.0, grad, 0.0);
        const bool gradMode = params.output == kOutputSaturationThreshold || params.output == kOutputBrightnessThreshold;
        if (gradMode && params.frameWidth > 0 && params.frameHeight > 0)
        {
            const float shift = (params.output == kOutputSaturationThreshold) ? params.satShift : params.brightShift;
            const int ax = params.frameX + int(dx);
            const int dtRow = params.frameHeight - 1 - (params.frameY + int(by));
            const float scaler = float(params.frameWidth) * 16.0;
            const float eps = 0.5 / float(params.frameHeight);
            for (int s = 0; s < 16; ++s)
            {
                const float weight = satWeight(float(16 * ax + s) / scaler - shift);
                if (weight > eps && weight < 1.0 - eps && int((1.0 - weight) * float(params.frameHeight - 1)) == dtRow)
                {
                    rgb = float3(0.0, 1.0, 0.0);
                    break;
                }
            }
        }
    }
    dst[o] = asuint(rgb.x);
    dst[o + 1] = asuint(rgb.y);
    dst[o + 2] = asuint(rgb.z);
    if (n > 3)
        dst[o + 3] = src[srcIndex + 3];
}
