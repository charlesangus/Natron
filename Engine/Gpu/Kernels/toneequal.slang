/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/iop/toneequal.c
 * and src/common/luminance_mask.h, Copyright (C) 2018-2026 darktable developers, distributed under
 * the GNU GPL version 3 or later, with the arithmetic of the CPU port in
 * Engine/Nodes/Color/ToneEqualizerKernels.cpp. darktable has no OpenCL tone equalizer.
 *
 * The tone equalizer's luminance estimate and its correction. The mask buffer holds one float a
 * pixel, tightly packed over the frame, rows from the top as the CPU path's.
 */

// The divided source.
RWStructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> maskBuf;
// The filtered image the stages finish from.
RWStructuredBuffer<float> dstBuf;
// The correction table of correctionLut().
RWStructuredBuffer<float> lutBuf;

// ToneEqualizerGpuParams in Engine/Nodes/Color/ToneEqualizerGpu.cpp mirrors it.
struct ToneEqualParams
{
    // Pixel (x, y) of the source is at srcOffset + (y - srcY1) * srcStride + (x - srcX1) * srcComps.
    uint srcOffset;
    uint srcStride;
    uint srcComps;
    int srcX1;
    int srcY1;
    // The filtered image, likewise.
    uint dstOffset;
    uint dstStride;
    uint dstComps;
    int dstX1;
    int dstY1;
    // The pixels a dispatch covers.
    int rectX1;
    int rectY1;
    uint rectWidth;
    uint rectHeight;
    // The frame the mask covers.
    int frameX1;
    int frameY1;
    uint frameWidth;
    uint frameHeight;
    // EstimatorEnum, OutputEnum.
    uint estimator;
    uint output;
    // Linear factors.
    float exposureBoost;
    float fulcrum;
    float contrastBoost;
};

[[vk::push_constant]]
ConstantBuffer<ToneEqualParams> params;

static const float kMinFloat = 1.52587890625e-05; // 2^-16
static const float kMinEv = -8.0;
static const float kMaxEv = 0.0;
static const float kLutResolution = 10000.0;

// glib's MAX and MIN, which keep the second argument when a comparison fails.
float glibMax(float a, float b)
{
    return (a > b) ? a : b;
}

float glibMin(float a, float b)
{
    return (a < b) ? a : b;
}

// fmaxf(fminf(v, top), bottom), NaN giving top.
float fastClamp(float v, float bottom, float top)
{
    const float t = (v < top) ? v : top;
    return (t > bottom) ? t : bottom;
}

float linearContrast(float pixel)
{
    precise float v = (pixel - params.fulcrum) * params.contrastBoost + params.fulcrum;
    return glibMax(v, kMinFloat);
}

float estimate(float r, float g, float b)
{
    const float eb = params.exposureBoost;
    switch (params.estimator)
    {
    case 0:
    {
        precise float lum = r + g + b;
        precise float v = eb * lum / 3.0f;
        return linearContrast(v);
    }
    case 1:
    {
        const float maxRgb = glibMax(glibMax(r, g), b);
        const float minRgb = glibMin(glibMin(r, g), b);
        precise float v = eb * (maxRgb + minRgb) / 2.0f;
        return linearContrast(v);
    }
    case 2:
    {
        precise float v = eb * glibMax(glibMax(r, g), b);
        return linearContrast(v);
    }
    case 3:
    {
        precise float lum = abs(r) + abs(g) + abs(b);
        precise float v = eb * lum;
        return linearContrast(v);
    }
    case 4:
    {
        precise float sum = r * r + g * g + b * b;
        precise float v = eb * sqrt(sum);
        return linearContrast(v);
    }
    case 5:
    {
        precise float numerator = 0.0f;
        precise float denominator = 0.0f;
        const float rgb[3] = { r, g, b };
        for (uint c = 0; c < 3; ++c)
        {
            const float value = abs(rgb[c]);
            precise float square = value * value;
            precise float cubic = square * value;
            numerator += cubic;
            denominator += square;
        }
        precise float v = eb * numerator / denominator;
        return linearContrast(v);
    }
    default:
    {
        precise float lum = abs(r) * abs(g) * abs(b);
        // pow() of 0 is left undefined by SPIR-V.
        const float root = (lum == 0.0f) ? 0.0f : pow(lum, 1.0f / 3.0f);
        precise float v = eb * root;
        return linearContrast(v);
    }
    }
}

uint sourceAt(int x, int y)
{
    return params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.srcComps;
}

uint maskAt(int x, int y)
{
    return uint(params.frameY1 + int(params.frameHeight) - 1 - y) * params.frameWidth + uint(x - params.frameX1);
}

// The luminance estimate of the source's colour over the frame, into the mask buffer.
[shader("compute")]
[numthreads(16, 16, 1)]
void toneequal_mask(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.frameWidth || tid.y >= params.frameHeight)
        return;
    const int x = params.frameX1 + int(tid.x);
    const int y = params.frameY1 + int(tid.y);
    const uint s = sourceAt(x, y);
    maskBuf[maskAt(x, y)] = estimate(srcBuf[s], srcBuf[s + 1], srcBuf[s + 2]);
}

float displayMask(float luminance)
{
    precise float v = glibMax(luminance - 0.00390625f, 0.0f) / 0.99609375f;
    return sqrt(glibMin(v, 1.0f));
}

// The correction, or the mask display, in the colour of the filtered image where the rect meets
// the frame; the source everywhere else and in the other channels.
[shader("compute")]
[numthreads(16, 16, 1)]
void toneequal_apply(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.rectWidth || tid.y >= params.rectHeight)
        return;
    const int x = params.rectX1 + int(tid.x);
    const int y = params.rectY1 + int(tid.y);
    const uint s = sourceAt(x, y);
    const uint d = params.dstOffset + uint(y - params.dstY1) * params.dstStride + uint(x - params.dstX1) * params.dstComps;
    const bool inFrame = (x >= params.frameX1) && (x - params.frameX1 < int(params.frameWidth)) && (y >= params.frameY1) && (y - params.frameY1 < int(params.frameHeight));
    uint first = 0;
    if (inFrame)
    {
        const float luminance = maskBuf[maskAt(x, y)];
        const float exposure = fastClamp(log2(luminance), kMinEv, kMaxEv);
        if (params.output == 0)
        {
            precise float position = (exposure - kMinEv) * kLutResolution;
            const float correction = lutBuf[uint(floor(position + 0.5f))];
            for (uint c = 0; c < 3; ++c)
            {
                precise float v = correction * srcBuf[s + c];
                dstBuf[d + c] = v;
            }
        }
        else
        {
            float v;
            if (params.output == 1)
            {
                v = displayMask(luminance);
            }
            else
            {
                // lround(), halves away from zero, of an exposure in [-8, 0].
                const float band = -floor(-exposure + 0.5f);
                v = displayMask(exp2(band));
            }
            for (uint c = 0; c < 3; ++c)
                dstBuf[d + c] = v;
        }
        first = 3;
    }
    for (uint c = first; c < params.dstComps; ++c)
        dstBuf[d + c] = srcBuf[s + c];
}
