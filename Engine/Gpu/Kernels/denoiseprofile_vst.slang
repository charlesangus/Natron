/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/iop/denoiseprofile.c
 * and data/kernels/denoiseprofile.cl, Copyright (C) 2011-2026 darktable developers, distributed
 * under the GNU GPL version 3 or later, with the arithmetic of the CPU port in
 * Engine/Nodes/Filter/DenoiseProfiledKernels.cpp.
 *
 * DenoiseProfiled's variance-stabilising transforms and their inverses. The work buffer holds three
 * floats a pixel, tightly packed over its rect, rows bottom-up as the image's. paramBuf holds the
 * constants the host derives from the profile in the CPU's float arithmetic, at the offsets below.
 */

// The divided source.
RWStructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> workBuf;
// The filtered image the stages finish from.
RWStructuredBuffer<float> dstBuf;
RWStructuredBuffer<float> paramBuf;

// DenoiseProfiledVstGpuParams in Engine/Nodes/Filter/DenoiseProfiledGpu.cpp mirrors it.
struct VstParams
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
    // The work buffer's rect; denoiseprofile_write copies the source outside it.
    int workX1;
    int workY1;
    uint workWidth;
    uint workHeight;
    // 0 the generalised Anscombe transform per channel, 1 followed by the Y0U0V0 rotation.
    uint transform;
};
[[vk::push_constant]] ConstantBuffer<VstParams> params;

// Offsets in paramBuf; kDenoiseProfiledGpu* in Engine/Nodes/Filter/DenoiseProfiledGpu.cpp mirror them.
static const uint kWb = 0;
static const uint kB = 3;
static const uint kPreExpon = 4;
static const uint kPreDenom = 7;
static const uint kPreScale = 10;
static const uint kToY0U0V0 = 13;
static const uint kBackExpon = 22;
static const uint kBackDenom = 25;
static const uint kBias = 28;
static const uint kBackScale = 29;
static const uint kBiasWb = 32;
static const uint kToRgb = 35;

// std::max() and std::min(), which return the first argument when either is NaN.
float stdMax(float a, float b)
{
    return (a < b) ? b : a;
}

float stdMin(float a, float b)
{
    return (b < a) ? b : a;
}

// pow() with 0 to a positive power, which SPIR-V leaves undefined, as 0.
float powZero(float x, float e)
{
    return (x == 0.0f && e > 0.0f) ? 0.0f : pow(x, e);
}

uint sourceAt(int x, int y)
{
    return params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.srcComps;
}

uint workAt(int x, int y)
{
    return 3 * (uint(y - params.workY1) * params.workWidth + uint(x - params.workX1));
}

// precondition_v2() or precondition_Y0U0V0() of the source's colour over the rect, into the work
// buffer.
[shader("compute")]
[numthreads(16, 16, 1)]
void denoiseprofile_precondition(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.rectWidth || tid.y >= params.rectHeight)
        return;
    const int x = params.rectX1 + int(tid.x);
    const int y = params.rectY1 + int(tid.y);
    const uint s = sourceAt(x, y);
    const uint w = workAt(x, y);
    const float b = paramBuf[kB];
    if (params.transform == 0)
    {
        for (uint c = 0; c < 3; ++c)
        {
            precise float scaled = stdMax(srcBuf[s + c] / paramBuf[kWb + c] + b, 0.0f);
            precise float v = 2.0f * powZero(scaled, paramBuf[kPreExpon + c]) / paramBuf[kPreDenom + c];
            workBuf[w + c] = v;
        }
        return;
    }
    float tmp[3];
    for (uint c = 0; c < 3; ++c)
    {
        precise float shifted = stdMax(srcBuf[s + c] + b, 0.0f);
        precise float t = powZero(shifted, paramBuf[kPreExpon + c]) * paramBuf[kPreScale + c];
        tmp[c] = t;
    }
    for (uint r = 0; r < 3; ++r)
    {
        const uint m = kToY0U0V0 + 3 * r;
        precise float v = paramBuf[m] * tmp[0] + paramBuf[m + 1] * tmp[1] + paramBuf[m + 2] * tmp[2];
        workBuf[w + r] = v;
    }
}

// backtransform_v2() or backtransform_Y0U0V0() of the work buffer into the colour of the filtered
// image where the rect meets the work buffer's rect; the source everywhere else and in the other
// channels.
[shader("compute")]
[numthreads(16, 16, 1)]
void denoiseprofile_write(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.rectWidth || tid.y >= params.rectHeight)
        return;
    const int x = params.rectX1 + int(tid.x);
    const int y = params.rectY1 + int(tid.y);
    const uint s = sourceAt(x, y);
    const uint d = params.dstOffset + uint(y - params.dstY1) * params.dstStride + uint(x - params.dstX1) * params.dstComps;
    const bool inWork = (x >= params.workX1) && (x - params.workX1 < int(params.workWidth)) && (y >= params.workY1) && (y - params.workY1 < int(params.workHeight));
    uint first = 0;
    if (inWork)
    {
        const uint w = workAt(x, y);
        const float b = paramBuf[kB];
        float v[3] = { workBuf[w], workBuf[w + 1], workBuf[w + 2] };
        if (params.transform == 0)
        {
            const float bias = paramBuf[kBias];
            for (uint c = 0; c < 3; ++c)
            {
                const float xc = stdMax(v[c], 0.0f);
                precise float delta = xc * xc + bias;
                precise float z1 = (xc + sqrt(stdMax(delta, 0.0f))) / paramBuf[kBackDenom + c];
                precise float out = paramBuf[kWb + c] * (powZero(z1, paramBuf[kBackExpon + c]) - b);
                dstBuf[d + c] = out;
            }
        }
        else
        {
            float rgb[3];
            for (uint r = 0; r < 3; ++r)
            {
                const uint m = kToRgb + 3 * r;
                precise float t = paramBuf[m] * v[0] + paramBuf[m + 1] * v[1] + paramBuf[m + 2] * v[2];
                rgb[r] = t;
            }
            for (uint c = 0; c < 3; ++c)
            {
                const float xc = stdMax(rgb[c], 0.0f);
                precise float delta = xc * xc + paramBuf[kBiasWb + c];
                precise float z1 = (xc + sqrt(stdMax(delta, 0.0f))) * paramBuf[kBackScale + c];
                precise float out = powZero(z1, paramBuf[kBackExpon + c]) - b;
                dstBuf[d + c] = out;
            }
        }
        first = 3;
    }
    for (uint c = first; c < params.dstComps; ++c)
        dstBuf[d + c] = srcBuf[s + c];
}
