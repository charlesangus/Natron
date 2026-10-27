/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/common/guided_filter.c,
 * src/common/fast_guided_filter.h and data/kernels/guided_filter.cl, Copyright (C) 2009-2026
 * darktable developers, distributed under the GNU GPL version 3 or later, with the arithmetic of the
 * CPU port in Engine/Nodes/Filter/EdgeAware/GuidedFilter.cpp. The box means run on
 * Kernels/boxfilter.slang.
 *
 * Images are tightly packed, top-down, channels interleaved. Arithmetic is precise so it rounds in
 * the CPU's order, without contracted multiply-adds.
 */

RWStructuredBuffer<float> guideBuf;
RWStructuredBuffer<float> targetBuf;
// gf_colour_*: the 4 means, then a_r, a_g, a_b and b. gf_grey_*: the 4 moments.
RWStructuredBuffer<float> aBuf;
// gf_colour_*: the 9 covariances. gf_grey_*, gf_blend: a and b.
RWStructuredBuffer<float> bBuf;
RWStructuredBuffer<float> outBuf;
// gf_interpolate: for each output column then each output row, the previous and next input
// position and the weights of the next and the previous, as the host computed them in float.
RWStructuredBuffer<uint> tableBuf;

// GpuGuidedParams in Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.cpp mirrors it.
struct GuidedParams
{
    uint width;
    uint height;
    uint guideChannels;
    uint channels;
    uint mode;
    uint widthIn;
    float guideWeight;
    float eps;
    float minValue;
    float maxValue;
    float sampling;
};
[[vk::push_constant]] ConstantBuffer<GuidedParams> params;

static const float kMinFloat = 1.52587890625e-05f;
static const float kFltEpsilon = 1.1920928955078125e-07f;

// gf_blend's mode bits.
static const uint kBlendGeomean = 1;
static const uint kBlendKeepNonFinite = 2;
// gf_interpolate's and gf_quantize's mode: the source's non-finite samples read as 0.
static const uint kInterpolateFinite = 1;
static const uint kQuantizeFinite = 1;

bool isFiniteBits(float v)
{
    return (asuint(v) & 0x7f800000u) != 0x7f800000u;
}

float finiteOrZero(float v)
{
    return isFiniteBits(v) ? v : 0.0f;
}

// std::fmax(a, b) for a b that is not NaN.
float fmaxCpu(float a, float b)
{
    if (isnan(a))
        return b;
    return a > b ? a : b;
}

// std::fmin(a, b) for a b that is not NaN.
float fminCpu(float a, float b)
{
    if (isnan(a))
        return b;
    return a < b ? a : b;
}

bool inImage(uint3 tid)
{
    return tid.x < params.width && tid.y < params.height;
}

// The guide's first three channels times guideWeight, and the input, of a guided-filter tile.
[shader("compute")]
[numthreads(16, 16, 1)]
void gf_colour_prep(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const uint g = k * params.guideChannels;
    const float w = params.guideWeight;
    precise float r = finiteOrZero(guideBuf[g]) * w;
    precise float gg = finiteOrZero(guideBuf[g + 1]) * w;
    precise float b = finiteOrZero(guideBuf[g + 2]) * w;
    const float input = finiteOrZero(targetBuf[k]);
    aBuf[4 * k] = input;
    aBuf[4 * k + 1] = r;
    aBuf[4 * k + 2] = gg;
    aBuf[4 * k + 3] = b;
    precise float v0 = r * input;
    precise float v1 = gg * input;
    precise float v2 = b * input;
    precise float v3 = r * r;
    precise float v4 = r * gg;
    precise float v5 = r * b;
    precise float v6 = gg * gg;
    precise float v7 = gg * b;
    precise float v8 = b * b;
    bBuf[9 * k] = v0;
    bBuf[9 * k + 1] = v1;
    bBuf[9 * k + 2] = v2;
    bBuf[9 * k + 3] = v3;
    bBuf[9 * k + 4] = v4;
    bBuf[9 * k + 5] = v5;
    bBuf[9 * k + 6] = v6;
    bBuf[9 * k + 7] = v7;
    bBuf[9 * k + 8] = v8;
}

// Solves the 3x3 system per pixel, a_r, a_g, a_b and b replacing the means in aBuf.
[shader("compute")]
[numthreads(16, 16, 1)]
void gf_colour_solve(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const float eps = params.eps;
    const float inpMean = aBuf[4 * k];
    const float guideR = aBuf[4 * k + 1];
    const float guideG = aBuf[4 * k + 2];
    const float guideB = aBuf[4 * k + 3];
    const uint v = 9 * k;
    precise float Sigma_0_0 = bBuf[v + 3] - (guideR * guideR) + eps;
    precise float Sigma_0_1 = bBuf[v + 4] - (guideR * guideG);
    precise float Sigma_0_2 = bBuf[v + 5] - (guideR * guideB);
    precise float Sigma_1_1 = bBuf[v + 6] - (guideG * guideG) + eps;
    precise float Sigma_1_2 = bBuf[v + 7] - (guideG * guideB);
    precise float Sigma_2_2 = bBuf[v + 8] - (guideB * guideB) + eps;
    precise float det0 = Sigma_0_0 * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
        - Sigma_0_1 * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
        + Sigma_0_2 * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
    float a_r = 0.0f;
    float a_g = 0.0f;
    float a_b = 0.0f;
    float b = inpMean;
    if (abs(det0) > 4.0f * kFltEpsilon)
    {
        precise float cov_r = bBuf[v] - guideR * inpMean;
        precise float cov_g = bBuf[v + 1] - guideG * inpMean;
        precise float cov_b = bBuf[v + 2] - guideB * inpMean;
        precise float det1 = cov_r * (Sigma_1_1 * Sigma_2_2 - Sigma_1_2 * Sigma_1_2)
            - Sigma_0_1 * (cov_g * Sigma_2_2 - cov_b * Sigma_1_2)
            + Sigma_0_2 * (cov_g * Sigma_1_2 - cov_b * Sigma_1_1);
        precise float det2 = Sigma_0_0 * (cov_g * Sigma_2_2 - cov_b * Sigma_1_2)
            - cov_r * (Sigma_0_1 * Sigma_2_2 - Sigma_0_2 * Sigma_1_2)
            + Sigma_0_2 * (Sigma_0_1 * cov_b - Sigma_0_2 * cov_g);
        precise float det3 = Sigma_0_0 * (Sigma_1_1 * cov_b - Sigma_1_2 * cov_g)
            - Sigma_0_1 * (Sigma_0_1 * cov_b - Sigma_0_2 * cov_g)
            + cov_r * (Sigma_0_1 * Sigma_1_2 - Sigma_0_2 * Sigma_1_1);
        a_r = det1 / det0;
        a_g = det2 / det0;
        a_b = det3 / det0;
        precise float bb = inpMean - a_r * guideR - a_g * guideG - a_b * guideB;
        b = bb;
    }
    aBuf[4 * k] = a_r;
    aBuf[4 * k + 1] = a_g;
    aBuf[4 * k + 2] = a_b;
    aBuf[4 * k + 3] = b;
}

// The averaged a and b applied to the guide, clamped; a pixel whose input or guide is not finite
// gets its input.
[shader("compute")]
[numthreads(16, 16, 1)]
void gf_colour_apply(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const uint g = k * params.guideChannels;
    const float g0 = guideBuf[g];
    const float g1 = guideBuf[g + 1];
    const float g2 = guideBuf[g + 2];
    const float input = targetBuf[k];
    if (!isFiniteBits(input) || !isFiniteBits(g0) || !isFiniteBits(g1) || !isFiniteBits(g2))
    {
        outBuf[k] = input;
        return;
    }
    precise float res = params.guideWeight * (aBuf[4 * k] * g0 + aBuf[4 * k + 1] * g1 + aBuf[4 * k + 2] * g2);
    precise float sum = res + aBuf[4 * k + 3];
    outBuf[k] = (sum > params.maxValue) ? params.maxValue : ((sum < params.minValue) ? params.minValue : sum);
}

// variance_analyse()'s inputs: guide, target, guide^2 and guide x target.
[shader("compute")]
[numthreads(16, 16, 1)]
void gf_grey_prep(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const float g = finiteOrZero(guideBuf[k]);
    const float m = finiteOrZero(targetBuf[k]);
    precise float gg = g * g;
    precise float gm = g * m;
    aBuf[4 * k] = g;
    aBuf[4 * k + 1] = m;
    aBuf[4 * k + 2] = gg;
    aBuf[4 * k + 3] = gm;
}

// variance_analyse()'s fit from the box means of its inputs, eps being the feathering.
[shader("compute")]
[numthreads(16, 16, 1)]
void gf_grey_ab(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const float m0 = aBuf[4 * k];
    const float m1 = aBuf[4 * k + 1];
    const float m2 = aBuf[4 * k + 2];
    const float m3 = aBuf[4 * k + 3];
    precise float var = (m2 - m0 * m0) + params.eps;
    const float d = fmaxCpu(var, 1e-15f);
    precise float cov = m3 - m0 * m1;
    const float a = cov / d;
    precise float b = m1 - a * m0;
    bBuf[2 * k] = a;
    bBuf[2 * k + 1] = b;
}

// out = a guide + b with the guide's non-finite samples as 0; a pixel whose guide or target is not
// finite gets its target.
[shader("compute")]
[numthreads(16, 16, 1)]
void gf_grey_apply(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const float g = guideBuf[k];
    const float t = targetBuf[k];
    if (!isFiniteBits(g) || !isFiniteBits(t))
    {
        outBuf[k] = t;
        return;
    }
    precise float v = bBuf[2 * k] * g + bBuf[2 * k + 1];
    outBuf[k] = v;
}

// apply_linear_blending() and its geometric-mean variant, in place on outBuf with a and b from
// bBuf. With kBlendKeepNonFinite a non-finite sample is left as it is.
[shader("compute")]
[numthreads(16, 16, 1)]
void gf_blend(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const float v = outBuf[k];
    if ((params.mode & kBlendKeepNonFinite) != 0 && !isFiniteBits(v))
        return;
    precise float lin = v * bBuf[2 * k] + bBuf[2 * k + 1];
    const float clipped = fmaxCpu(lin, kMinFloat);
    if ((params.mode & kBlendGeomean) != 0)
    {
        precise float prod = v * clipped;
        outBuf[k] = sqrt(prod);
    }
    else
    {
        outBuf[k] = clipped;
    }
}

// quantize(): exposure steps of params.sampling EV, clamped, from targetBuf into outBuf; width is
// the sample count of a row. With kQuantizeFinite a non-finite sample reads as 0.
[shader("compute")]
[numthreads(16, 16, 1)]
void gf_quantize(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    float v = targetBuf[k];
    if ((params.mode & kQuantizeFinite) != 0)
        v = finiteOrZero(v);
    const float s = params.sampling;
    if (s == 0.0f)
    {
        outBuf[k] = v;
        return;
    }
    float q;
    if (isnan(v) || v < 0.0f)
    {
        // log2 of a negative or NaN sample is NaN, and fmin(NaN, max) is max.
        q = params.maxValue;
    }
    else if (v == 0.0f)
    {
        q = 0.0f;
    }
    else if (s == 1.0f)
    {
        // exp2(floor(log2(v))) from the exponent bits, exact where log2 is not.
        q = asfloat(asuint(v) & 0x7f800000u);
    }
    else
    {
        precise float e = floor(log2(v) / s) * s;
        q = exp2(e);
    }
    outBuf[k] = fmaxCpu(fminCpu(q, params.maxValue), params.minValue);
}

// interpolate_bilinear() from targetBuf (widthIn wide) into outBuf (width x height), params.channels
// floats a pixel, with the host's sample positions and weights.
[shader("compute")]
[numthreads(16, 16, 1)]
void gf_interpolate(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint ch = params.channels;
    const uint xr = 4 * tid.x;
    const uint yr = 4 * (params.width + tid.y);
    const uint xPrev = tableBuf[xr];
    const uint xNext = tableBuf[xr + 1];
    const float dxNext = asfloat(tableBuf[xr + 2]);
    const float dxPrev = asfloat(tableBuf[xr + 3]);
    const uint yPrev = tableBuf[yr];
    const uint yNext = tableBuf[yr + 1];
    const float dyNext = asfloat(tableBuf[yr + 2]);
    const float dyPrev = asfloat(tableBuf[yr + 3]);
    const uint nw = (yPrev * params.widthIn + xPrev) * ch;
    const uint ne = (yPrev * params.widthIn + xNext) * ch;
    const uint se = (yNext * params.widthIn + xNext) * ch;
    const uint sw = (yNext * params.widthIn + xPrev) * ch;
    const uint o = (tid.y * params.width + tid.x) * ch;
    const bool finite = (params.mode & kInterpolateFinite) != 0;
    for (uint c = 0; c < ch; ++c)
    {
        float qnw = targetBuf[nw + c];
        float qne = targetBuf[ne + c];
        float qse = targetBuf[se + c];
        float qsw = targetBuf[sw + c];
        if (finite)
        {
            qnw = finiteOrZero(qnw);
            qne = finiteOrZero(qne);
            qse = finiteOrZero(qse);
            qsw = finiteOrZero(qsw);
        }
        precise float v = dyPrev * (qsw * dxNext + qse * dxPrev) + dyNext * (qnw * dxNext + qne * dxPrev);
        outBuf[o + c] = v;
    }
}
