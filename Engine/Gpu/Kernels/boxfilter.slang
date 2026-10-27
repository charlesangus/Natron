/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/common/box_filters.cc
 * and data/kernels/guided_filter.cl, Copyright (C) 2009-2026 darktable developers, distributed under
 * the GNU GPL version 3 or later, with the arithmetic of the CPU port in
 * Engine/Nodes/Filter/EdgeAware/BoxFilter.cpp.
 *
 * Images are tightly packed, top-down, channels interleaved. Each thread runs one line's moving sum
 * in the CPU's order, so the means differ from the CPU's only by the division's rounding; the
 * compensated sums are precise so a compiler cannot fold the compensation away.
 */

RWStructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;
// box_meanvar: the mean, then the variance.
RWStructuredBuffer<float> auxBuf;
RWStructuredBuffer<float> aux2Buf;

// GpuBoxParams in Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.cpp mirrors it. box_copy covers
// width x height floats, width being a row's float count.
struct BoxParams
{
    uint width;
    uint height;
    uint channels;
    uint radius;
    uint kahan;
    uint mode;
};
[[vk::push_constant]] ConstantBuffer<BoxParams> params;

// box_copy's modes.
static const uint kCopyPlain = 0;
// Every non-finite sample as 0.
static const uint kCopyFinite = 1;
// Only srcBuf's non-finite samples, putting back what kCopyTake took out.
static const uint kCopyNonFinite = 2;
// srcBuf into dstBuf, then srcBuf's non-finite samples set to 0, in place.
static const uint kCopyTake = 3;
// Zeros into dstBuf.
static const uint kCopyZero = 4;

bool isFiniteBits(float v)
{
    return (asuint(v) & 0x7f800000u) != 0x7f800000u;
}

float finiteOrZero(float v)
{
    return isFiniteBits(v) ? v : 0.0f;
}

// std::fmax(a, 0) as the CPU computes it: a NaN gives 0.
float fmaxZero(float a)
{
    if (isnan(a))
        return 0.0f;
    return a > 0.0f ? a : 0.0f;
}

struct Sum
{
    float value;
    float comp;
};

void add(inout Sum s, float v, bool kahan)
{
    if (kahan)
    {
        precise float t1 = v - s.comp;
        precise float t2 = s.value + t1;
        precise float c = (t2 - s.value) - t1;
        s.comp = c;
        s.value = t2;
    }
    else
    {
        precise float t = s.value + v;
        s.value = t;
    }
}

void sub(inout Sum s, float v, bool kahan)
{
    if (kahan)
    {
        add(s, -v, true);
    }
    else
    {
        precise float t = s.value - v;
        s.value = t;
    }
}

// _blur_horizontal() and _blur_vertical(): count samples stride floats apart from base, from srcBuf
// into dstBuf, the window shrinking at both ends.
void boxLine(uint base, uint stride, uint count, uint radius, bool kahan)
{
    Sum s = { 0.0f, 0.0f };
    uint hits = 0;
    uint x = 0;
    for (; x < min(radius, count); ++x)
    {
        ++hits;
        add(s, srcBuf[base + x * stride], kahan);
    }
    for (x = 0; x <= radius && x + radius < count; ++x)
    {
        ++hits;
        add(s, srcBuf[base + (x + radius) * stride], kahan);
        dstBuf[base + x * stride] = s.value / float(hits);
    }
    for (; x <= radius && x < count; ++x)
        dstBuf[base + x * stride] = s.value / float(hits);
    for (; x + radius < count; ++x)
    {
        sub(s, srcBuf[base + (x - radius - 1) * stride], kahan);
        add(s, srcBuf[base + (x + radius) * stride], kahan);
        dstBuf[base + x * stride] = s.value / float(hits);
    }
    for (; x < count; ++x)
    {
        --hits;
        sub(s, srcBuf[base + (x - radius - 1) * stride], kahan);
        dstBuf[base + x * stride] = s.value / float(hits);
    }
}

// One thread per (row, channel); the host caps radius at the line length.
[shader("compute")]
[numthreads(64, 1, 1)]
void box_rows(uint3 tid : SV_DispatchThreadID)
{
    const uint line = tid.x;
    if (line >= params.height * params.channels)
        return;
    const uint row = line / params.channels;
    const uint c = line % params.channels;
    boxLine(row * params.width * params.channels + c, params.channels, params.width, params.radius, params.kahan != 0);
}

// One thread per float column.
[shader("compute")]
[numthreads(64, 1, 1)]
void box_columns(uint3 tid : SV_DispatchThreadID)
{
    const uint stride = params.width * params.channels;
    if (tid.x >= stride)
        return;
    boxLine(tid.x, stride, params.height, params.radius, params.kahan != 0);
}

// srcBuf into dstBuf, as params.mode says.
[shader("compute")]
[numthreads(16, 16, 1)]
void box_copy(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint k = tid.y * params.width + tid.x;
    switch (params.mode)
    {
    case kCopyFinite:
        dstBuf[k] = finiteOrZero(srcBuf[k]);
        break;
    case kCopyNonFinite:
    {
        const float v = srcBuf[k];
        if (!isFiniteBits(v))
            dstBuf[k] = v;
        break;
    }
    case kCopyTake:
    {
        const float v = srcBuf[k];
        dstBuf[k] = v;
        if (!isFiniteBits(v))
            srcBuf[k] = 0.0f;
        break;
    }
    case kCopyZero:
        dstBuf[k] = 0.0f;
        break;
    default:
        dstBuf[k] = srcBuf[k];
        break;
    }
}

// The single-channel srcBuf's samples and their squares, interleaved in dstBuf.
[shader("compute")]
[numthreads(16, 16, 1)]
void box_moments(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint k = tid.y * params.width + tid.x;
    const float v = finiteOrZero(srcBuf[k]);
    precise float sq = v * v;
    dstBuf[2 * k] = v;
    dstBuf[2 * k + 1] = sq;
}

// The mean and variance of srcBuf from the box means of its moments in dstBuf.
[shader("compute")]
[numthreads(16, 16, 1)]
void box_meanvar(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint k = tid.y * params.width + tid.x;
    const float v = srcBuf[k];
    if (!isFiniteBits(v))
    {
        auxBuf[k] = v;
        aux2Buf[k] = 0.0f;
        return;
    }
    const float m = dstBuf[2 * k];
    precise float m2 = m * m;
    precise float d = dstBuf[2 * k + 1] - m2;
    auxBuf[k] = m;
    aux2Buf[k] = fmaxZero(d);
}
