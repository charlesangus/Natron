/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/common/eigf.h and
 * src/common/gaussian.c (order 0), Copyright (C) 2009-2026 darktable developers, distributed under
 * the GNU GPL version 3 or later, with the arithmetic of the CPU port in
 * Engine/Nodes/Filter/EdgeAware/Eigf.cpp. darktable has no OpenCL eigf kernel; the subsampling and
 * quantization run on Kernels/guidedfilter.slang.
 *
 * Images are tightly packed, top-down, channels interleaved. Arithmetic is precise so it rounds in
 * the CPU's order, without contracted multiply-adds.
 */

RWStructuredBuffer<float> inBuf;
RWStructuredBuffer<float> outBuf;
// eigf_blend_*: the subsampled statistics brought back to full size.
RWStructuredBuffer<float> avBuf;
// eigf_blend_mask: the quantized guide.
RWStructuredBuffer<float> maskBuf;
// The per-channel minima, then the maxima, as orderedKey() of the floats.
RWStructuredBuffer<uint> rangeBuf;

// GpuEigfParams in Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.cpp mirrors it.
struct EigfParams
{
    uint width;
    uint height;
    uint channels;
    uint mode;
    float a0;
    float a1;
    float a2;
    float a3;
    float b1;
    float b2;
    float coefp;
    float coefn;
    float feathering;
};
[[vk::push_constant]] ConstantBuffer<EigfParams> params;

static const uint kMaxChannels = 4;
static const float kMinFloat = 1.52587890625e-05f;
// eigf_blend_*'s mode.
static const uint kBlendGeomean = 1;

// std::fmax(a, b) for a b that is not NaN.
float fmaxCpu(float a, float b)
{
    if (isnan(a))
        return b;
    return a > b ? a : b;
}

// A uint whose unsigned order is the order of the non-NaN floats.
uint orderedKey(float v)
{
    const uint bits = asuint(v);
    return (bits & 0x80000000u) != 0 ? ~bits : (bits | 0x80000000u);
}

float fromOrderedKey(uint key)
{
    return asfloat((key & 0x80000000u) != 0 ? (key & 0x7fffffffu) : ~key);
}

bool inImage(uint3 tid)
{
    return tid.x < params.width && tid.y < params.height;
}

// The per-channel range of inBuf, folded into rangeBuf, which the host starts at darktable's
// 1e7 and 0.
[shader("compute")]
[numthreads(16, 16, 1)]
void eigf_range(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint ch = params.channels;
    const uint base = (tid.y * params.width + tid.x) * ch;
    for (uint c = 0; c < ch; ++c)
    {
        const uint key = orderedKey(inBuf[base + c]);
        InterlockedMin(rangeBuf[c], key);
        InterlockedMax(rangeBuf[ch + c], key);
    }
}

float clampRange(float a, float mn, float mx)
{
    return (a >= mn) ? ((a <= mx) ? a : mx) : mn;
}

// count samples step floats apart from base, causal pass then anticausal pass added to it.
void gaussianLine(uint base, uint step, uint count)
{
    const uint ch = params.channels;
    float mn[kMaxChannels];
    float mx[kMaxChannels];
    float xp[kMaxChannels];
    float yb[kMaxChannels];
    float yp[kMaxChannels];
    for (uint k = 0; k < ch; ++k)
    {
        mn[k] = fromOrderedKey(rangeBuf[k]);
        mx[k] = fromOrderedKey(rangeBuf[ch + k]);
        xp[k] = clampRange(inBuf[base + k], mn[k], mx[k]);
        precise float y = xp[k] * params.coefp;
        yb[k] = y;
        yp[k] = y;
    }
    for (uint j = 0; j < count; ++j)
    {
        const uint offset = base + j * step;
        for (uint k = 0; k < ch; ++k)
        {
            const float xc = clampRange(inBuf[offset + k], mn[k], mx[k]);
            precise float yc = (params.a0 * xc) + (params.a1 * xp[k]) - (params.b1 * yp[k]) - (params.b2 * yb[k]);
            outBuf[offset + k] = yc;
            xp[k] = xc;
            yb[k] = yp[k];
            yp[k] = yc;
        }
    }
    const uint last = base + (count - 1) * step;
    float xn[kMaxChannels];
    float xa[kMaxChannels];
    float yn[kMaxChannels];
    float ya[kMaxChannels];
    for (uint k = 0; k < ch; ++k)
    {
        xn[k] = clampRange(inBuf[last + k], mn[k], mx[k]);
        xa[k] = xn[k];
        precise float y = xn[k] * params.coefn;
        yn[k] = y;
        ya[k] = y;
    }
    for (uint jj = count; jj > 0; --jj)
    {
        const uint offset = base + (jj - 1) * step;
        for (uint k = 0; k < ch; ++k)
        {
            const float xc = clampRange(inBuf[offset + k], mn[k], mx[k]);
            precise float yc = (params.a2 * xn[k]) + (params.a3 * xa[k]) - (params.b1 * yn[k]) - (params.b2 * ya[k]);
            xa[k] = xn[k];
            xn[k] = xc;
            ya[k] = yn[k];
            yn[k] = yc;
            precise float sum = outBuf[offset + k] + yc;
            outBuf[offset + k] = sum;
        }
    }
}

// The vertical recursion, one thread per column.
[shader("compute")]
[numthreads(64, 1, 1)]
void eigf_gauss_columns(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width)
        return;
    gaussianLine(tid.x * params.channels, params.width * params.channels, params.height);
}

// The horizontal recursion, one thread per row.
[shader("compute")]
[numthreads(64, 1, 1)]
void eigf_gauss_rows(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.height)
        return;
    gaussianLine(tid.x * params.width * params.channels, params.channels, params.width);
}

// The variance analysis inputs into outBuf: guide (inBuf), its square, mask (maskBuf) and mask x
// guide.
[shader("compute")]
[numthreads(16, 16, 1)]
void eigf_prep_mask(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const float g = inBuf[k];
    const float m = maskBuf[k];
    precise float gg = g * g;
    precise float mg = m * g;
    outBuf[4 * k] = g;
    outBuf[4 * k + 1] = gg;
    outBuf[4 * k + 2] = m;
    outBuf[4 * k + 3] = mg;
}

// The unmasked variance analysis inputs into outBuf: guide (inBuf) and its square.
[shader("compute")]
[numthreads(16, 16, 1)]
void eigf_prep(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const float g = inBuf[k];
    precise float gg = g * g;
    outBuf[2 * k] = g;
    outBuf[2 * k + 1] = gg;
}

// The blurred moments in outBuf turned into variance (and covariance with 4 channels), in place.
[shader("compute")]
[numthreads(16, 16, 1)]
void eigf_variance(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    if (params.channels == 4)
    {
        const float avg = outBuf[4 * k];
        precise float v = outBuf[4 * k + 1] - avg * avg;
        precise float cv = outBuf[4 * k + 3] - avg * outBuf[4 * k + 2];
        outBuf[4 * k + 1] = v;
        outBuf[4 * k + 3] = cv;
    }
    else
    {
        const float avg = outBuf[2 * k];
        precise float v = outBuf[2 * k + 1] - avg * avg;
        outBuf[2 * k + 1] = v;
    }
}

float blend(float pixel, float a, float b)
{
    precise float lin = pixel * a + b;
    const float clipped = fmaxCpu(lin, kMinFloat);
    if ((params.mode & kBlendGeomean) != 0)
    {
        precise float prod = pixel * clipped;
        return sqrt(prod);
    }
    return clipped;
}

// In place on outBuf.
[shader("compute")]
[numthreads(16, 16, 1)]
void eigf_blend_mask(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const float pixel = outBuf[k];
    const float avg_g = avBuf[4 * k];
    const float var_g = avBuf[4 * k + 1];
    const float avg_m = avBuf[4 * k + 2];
    const float covar_mg = avBuf[4 * k + 3];
    precise float pg = avg_g * pixel;
    precise float pm = avg_m * maskBuf[k];
    const float norm_g = fmaxCpu(pg, 1e-6f);
    const float norm_m = fmaxCpu(pm, 1e-6f);
    const float normalized_var_guide = var_g / norm_g;
    precise float nn = norm_g * norm_m;
    const float normalized_covar = covar_mg / sqrt(nn);
    precise float denom = normalized_var_guide + params.feathering;
    const float a = normalized_covar / denom;
    precise float b = avg_m - a * avg_g;
    outBuf[k] = blend(pixel, a, b);
}

// In place on outBuf.
[shader("compute")]
[numthreads(16, 16, 1)]
void eigf_blend(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = tid.y * params.width + tid.x;
    const float pixel = outBuf[k];
    const float avg_g = avBuf[2 * k];
    const float var_g = avBuf[2 * k + 1];
    precise float pg = avg_g * pixel;
    const float norm_g = fmaxCpu(pg, 1e-6f);
    const float normalized_var_guide = var_g / norm_g;
    precise float denom = normalized_var_guide + params.feathering;
    const float a = normalized_var_guide / denom;
    precise float b = avg_g - a * avg_g;
    outBuf[k] = blend(pixel, a, b);
}
