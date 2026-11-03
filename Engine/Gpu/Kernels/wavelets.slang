/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/common/dwt.c,
 * src/common/bspline.h, data/kernels/dwt.cl and data/kernels/bspline.cl, Copyright (C) 2009-2026
 * darktable developers, distributed under the GNU GPL version 3 or later, with the arithmetic of
 * the CPU port in Engine/Nodes/Filter/EdgeAware/Wavelets.cpp.
 *
 * Images are 4 interleaved channels, tightly packed, top-down. Arithmetic is precise so it rounds
 * in the CPU's order, without contracted multiply-adds.
 */

RWStructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> tempBuf;
RWStructuredBuffer<float> outBuf;
// The detail band a decomposition step writes.
RWStructuredBuffer<float> hfBuf;
// The decomposition's input: a detail sample is 0 where it is not finite.
RWStructuredBuffer<float> inBuf;

// GpuWaveletParams in Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.cpp mirrors it.
struct WaveletParams
{
    uint width;
    uint height;
    // The tap spacing: the hat transform's, already capped, or the B-spline's mult.
    uint spacing;
    uint clip;
    uint mode;
};
[[vk::push_constant]] ConstantBuffer<WaveletParams> params;

// wav_bspline_h's modes.
// The low-pass into outBuf.
static const uint kBsplineBlur = 0;
// The low-pass into outBuf and srcBuf minus it into hfBuf.
static const uint kBsplineDecompose = 1;
// srcBuf minus the low-pass into hfBuf, 0 where inBuf is not finite, and the low-pass into srcBuf.
static const uint kBsplineStep = 2;

bool isFiniteBits(float v)
{
    return (asuint(v) & 0x7f800000u) != 0x7f800000u;
}

bool inImage(uint3 tid)
{
    return tid.x < params.width && tid.y < params.height;
}

// 2 centre + above + below into tempBuf, mirrored without repeating the edge.
[shader("compute")]
[numthreads(16, 16, 1)]
void wav_hat_v(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint row = tid.y;
    const uint vscale = params.spacing;
    const uint h = params.height;
    const uint above = row > vscale ? row - vscale : vscale - row;
    const uint below = row + vscale < h ? row + vscale : 2 * (h - 1) - (row + vscale);
    const uint stride = 4 * params.width;
    const uint col = 4 * tid.x;
    for (uint c = 0; c < 4; ++c)
    {
        precise float v = 2.0f * srcBuf[row * stride + col + c] + srcBuf[above * stride + col + c] + srcBuf[below * stride + col + c];
        tempBuf[row * stride + col + c] = v;
    }
}

// The hat low-pass of tempBuf's vertical sums replaces srcBuf, and srcBuf
// minus it goes to hfBuf, 0 where inBuf is not finite.
[shader("compute")]
[numthreads(16, 16, 1)]
void wav_hat_h(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const int w = int(params.width);
    const int col = int(tid.x);
    const int hscale = int(params.spacing);
    const uint rowStart = 4 * tid.y * params.width;
    const uint left = rowStart + 4 * uint(abs(col - hscale));
    const uint right = rowStart + 4 * uint(col < w - hscale ? col + hscale : 2 * w - 2 - (col + hscale));
    const uint centre = rowStart + 4 * uint(col);
    for (uint c = 0; c < 4; ++c)
    {
        precise float sum = 2.0f * tempBuf[centre + c] + tempBuf[left + c] + tempBuf[right + c];
        precise float hat = sum * 0.0625f;
        precise float detail = srcBuf[centre + c] - hat;
        hfBuf[centre + c] = isFiniteBits(inBuf[centre + c]) ? detail : 0.0f;
        srcBuf[centre + c] = hat;
    }
}

float clipped(float v)
{
    return (params.clip != 0 && 0.0f > v) ? 0.0f : v;
}

// The [1 4 6 4 1] / 16 filter over the five taps, as sparseScalarProduct().
float bspline(float a, float b, float c, float d, float e)
{
    precise float v = (1.0f / 16.0f) * a + (4.0f / 16.0f) * b + (6.0f / 16.0f) * c + (4.0f / 16.0f) * d + (1.0f / 16.0f) * e;
    return clipped(v);
}

// The vertical B-spline pass of srcBuf into tempBuf, clamped to the edge.
[shader("compute")]
[numthreads(16, 16, 1)]
void wav_bspline_v(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const int row = int(tid.y);
    const int mult = int(params.spacing);
    const uint last = params.height - 1;
    const uint stride = 4 * params.width;
    const uint r0 = uint(max(row - 2 * mult, 0)) * stride;
    const uint r1 = uint(max(row - mult, 0)) * stride;
    const uint r2 = uint(row) * stride;
    const uint r3 = min(uint(row) + uint(mult), last) * stride;
    const uint r4 = min(uint(row) + 2 * uint(mult), last) * stride;
    const uint col = 4 * tid.x;
    for (uint c = 0; c < 4; ++c)
    {
        const uint i = col + c;
        tempBuf[r2 + i] = bspline(srcBuf[r0 + i], srcBuf[r1 + i], srcBuf[r2 + i], srcBuf[r3 + i], srcBuf[r4 + i]);
    }
}

// The horizontal B-spline pass of tempBuf, as params.mode says.
[shader("compute")]
[numthreads(16, 16, 1)]
void wav_bspline_h(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const int col = int(tid.x);
    const int mult = int(params.spacing);
    const uint last = params.width - 1;
    const uint rowStart = 4 * tid.y * params.width;
    const uint c0 = rowStart + 4 * uint(max(col - 2 * mult, 0));
    const uint c1 = rowStart + 4 * uint(max(col - mult, 0));
    const uint c2 = rowStart + 4 * uint(col);
    const uint c3 = rowStart + 4 * min(uint(col) + uint(mult), last);
    const uint c4 = rowStart + 4 * min(uint(col) + 2 * uint(mult), last);
    for (uint c = 0; c < 4; ++c)
    {
        const float lf = bspline(tempBuf[c0 + c], tempBuf[c1 + c], tempBuf[c2 + c], tempBuf[c3 + c], tempBuf[c4 + c]);
        if (params.mode == kBsplineBlur)
        {
            outBuf[c2 + c] = lf;
        }
        else if (params.mode == kBsplineDecompose)
        {
            precise float hf = srcBuf[c2 + c] - lf;
            outBuf[c2 + c] = lf;
            hfBuf[c2 + c] = hf;
        }
        else
        {
            precise float hf = srcBuf[c2 + c] - lf;
            hfBuf[c2 + c] = isFiniteBits(inBuf[c2 + c]) ? hf : 0.0f;
            srcBuf[c2 + c] = lf;
        }
    }
}

// outBuf += srcBuf.
[shader("compute")]
[numthreads(16, 16, 1)]
void wav_add(uint3 tid : SV_DispatchThreadID)
{
    if (!inImage(tid))
        return;
    const uint k = 4 * (tid.y * params.width + tid.x);
    for (uint c = 0; c < 4; ++c)
    {
        precise float v = outBuf[k + c] + srcBuf[k + c];
        outBuf[k + c] = v;
    }
}
