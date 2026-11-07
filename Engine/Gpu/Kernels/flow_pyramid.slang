// The image pyramid of the optical-flow solvers. A level holds, per pixel and channel, the matched
// value and its two gradients, so a solver binds one buffer for a whole frame. FlowPyramidPush in
// Engine/Gpu/GpuFlow.cpp mirrors the push block.

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;

struct FlowPyramidParams
{
    uint srcOffset;
    uint srcRowStride;
    uint srcPixelStride;
    int srcX0;
    int srcY0;
    int srcWidth;
    int srcHeight;
    uint width;
    uint height;
    uint dstOffset;
    uint dstPixelStride;
    uint channels;
    uint channelStride;
    uint luminance;
    uint matchingCurve;
    float w0;
    float w1;
    float w2;
    float w3;
    float value;
};

[[vk::push_constant]] ConstantBuffer<FlowPyramidParams> params;

float finiteOrZero(float value)
{
    return ((asuint(value) & 0x7f800000u) == 0x7f800000u) ? 0.f : value;
}

// Accurate below the rounding of 1 + x, where the curve's slope near black is what is matched.
float logOnePlus(float x)
{
    precise float u = 1.f + x;
    if (u == 1.f) {
        return x;
    }
    precise float scaled = log(u) * x;
    precise float denominator = u - 1.f;

    return scaled / denominator;
}

// vectorGeneratorMatchingValue() in Engine/Nodes/Time/VectorGenerator.h, whose denominator is log1p(20).
float matchingValue(float value)
{
    if (((asuint(value) & 0x7f800000u) == 0x7f800000u) || (value <= 0.f)) {
        return 0.f;
    }
    precise float scaled = min(value, 1e30f) / 0.05f;
    precise float curved = logOnePlus(scaled);

    return curved / 3.04452244f;
}

float sourceComponent(int x, int y, uint c)
{
    if ((x < params.srcX0) || (y < params.srcY0) || (x >= params.srcX0 + params.srcWidth) || (y >= params.srcY0 + params.srcHeight)) {
        return 0.f;
    }
    uint index = params.srcOffset + uint(y - params.srcY0) * params.srcRowStride + uint(x - params.srcX0) * params.srcPixelStride + c;
    float value = srcBuf[index];

    return (params.matchingCurve != 0) ? matchingValue(value) : finiteOrZero(value);
}

float dotWeights(float centre, float near1, float near2, float near3)
{
    precise float sum = params.w0 * centre;
    sum += params.w1 * near1;
    sum += params.w2 * near2;
    sum += params.w3 * near3;

    return sum;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_extract(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    int x = int(tid.x);
    int y = int(tid.y);
    uint pixel = params.dstOffset + (tid.y * params.width + tid.x) * params.dstPixelStride;
    if (params.luminance != 0) {
        precise float luma = 0.2126f * sourceComponent(x, y, 0);
        luma += 0.7152f * sourceComponent(x, y, 1);
        luma += 0.0722f * sourceComponent(x, y, 2);
        dstBuf[pixel] = luma;

        return;
    }
    for (uint c = 0; c < params.channels; ++c) {
        dstBuf[pixel + c * params.channelStride] = sourceComponent(x, y, c);
    }
}

float rowTap(uint y, int x, uint c)
{
    int clamped = clamp(x, 0, int(params.width) - 1);

    return srcBuf[params.srcOffset + y * params.srcRowStride + uint(clamped) * params.srcPixelStride + c * params.channelStride];
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_blur_rows(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    int x = int(tid.x);
    for (uint c = 0; c < params.channels; ++c) {
        precise float near1 = rowTap(tid.y, x - 1, c) + rowTap(tid.y, x + 1, c);
        precise float near2 = rowTap(tid.y, x - 2, c) + rowTap(tid.y, x + 2, c);
        precise float near3 = rowTap(tid.y, x - 3, c) + rowTap(tid.y, x + 3, c);
        dstBuf[params.dstOffset + (tid.y * params.width + tid.x) * params.channels + c] = dotWeights(rowTap(tid.y, x, c), near1, near2, near3);
    }
}

float columnTap(uint x, int y, uint c)
{
    int clamped = clamp(y, 0, params.srcHeight - 1);

    return srcBuf[params.srcOffset + (uint(clamped) * uint(params.srcWidth) + x) * params.channels + c];
}

// The columns are only blurred at the rows the next level keeps, which gives the samples blurring
// every row and then decimating would.
[shader("compute")]
[numthreads(8, 8, 1)]
void flow_blur_columns(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint srcX = 2 * tid.x;
    int y = int(2 * tid.y);
    uint pixel = params.dstOffset + (tid.y * params.width + tid.x) * params.dstPixelStride;
    for (uint c = 0; c < params.channels; ++c) {
        precise float near1 = columnTap(srcX, y - 1, c) + columnTap(srcX, y + 1, c);
        precise float near2 = columnTap(srcX, y - 2, c) + columnTap(srcX, y + 2, c);
        precise float near3 = columnTap(srcX, y - 3, c) + columnTap(srcX, y + 3, c);
        dstBuf[pixel + c * params.channelStride] = dotWeights(columnTap(srcX, y, c), near1, near2, near3);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_gradients(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint x = tid.x;
    uint y = tid.y;
    uint left = (x > 0) ? x - 1 : 0;
    uint right = min(x + 1, params.width - 1);
    uint above = (y > 0) ? y - 1 : 0;
    uint below = min(y + 1, params.height - 1);
    float xScale = ((x == 0) || (x == params.width - 1)) ? 1.f : 0.5f;
    float yScale = ((y == 0) || (y == params.height - 1)) ? 1.f : 0.5f;
    uint stride = params.dstPixelStride;
    uint base = params.dstOffset;
    uint w = params.width;
    for (uint c = 0; c < params.channels; ++c) {
        uint channel = c * params.channelStride;
        precise float dx = dstBuf[base + (y * w + right) * stride + channel] - dstBuf[base + (y * w + left) * stride + channel];
        precise float dy = dstBuf[base + (below * w + x) * stride + channel] - dstBuf[base + (above * w + x) * stride + channel];
        uint pixel = base + (y * w + x) * stride + channel;
        dstBuf[pixel + 1] = xScale * dx;
        dstBuf[pixel + 2] = yScale * dy;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_fill(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint pixel = params.dstOffset + (tid.y * params.width + tid.x) * params.dstPixelStride;
    for (uint c = 0; c < params.dstPixelStride; ++c) {
        dstBuf[pixel + c] = params.value;
    }
}
