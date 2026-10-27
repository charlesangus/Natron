// Sharpen's own passes, after the workers of Engine/Nodes/Filter/Sharpen.cpp: the source fill with
// the nearest pixel repeated, and the combination of the source with its blur. The blur between
// them is blur.slang's or blur_lines.slang's. The fill and blurred buffers are tightly packed,
// `channels` floats per pixel. SharpenGpuParams in Engine/Nodes/Filter/Sharpen.cpp mirrors
// SharpenParams.

// fill: the source image; combine: the filled source.
StructuredBuffer<uint> inBuf;
// combine: the blurred source; fill: bound but unread.
StructuredBuffer<uint> blurBuf;
// fill: the filled buffer; combine: the rect of outWidth x outHeight pixels at (outX1, outY1),
// whose first pixel is at dstOffset.
RWStructuredBuffer<uint> outBuf;

struct SharpenParams
{
    // The filled buffer's rect.
    int bufX1;
    int bufY1;
    uint bufWidth;
    uint bufHeight;
    // The blurred buffer's origin and width.
    int blurX1;
    int blurY1;
    uint blurWidth;
    uint channels;
    // The CPU's buffer: the combination writes zero outside it.
    int domX1;
    int domY1;
    int domX2;
    int domY2;
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint dstOffset;
    uint dstStride;
    // fill: the region of definition, to which a pixel is clamped before the source.
    int rodX1;
    int rodY1;
    int rodX2;
    int rodY2;
    // fill: the source pixels that exist, the first at srcOffset; empty when there are none.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    // Mirrors SharpenModeEnum.
    uint mode;
    float amount;
    float threshold;
    float epsilon;
};
[[vk::push_constant]] ConstantBuffer<SharpenParams> params;

static const uint kModeSoften = 1;
static const uint kModeLaplacian = 2;

// fillBuffer() with the nearest-pixel boundary, of a buffer that may reach past the region of
// definition: the pixel is clamped to that first, then to the source. Zero when the source has
// none.
[shader("compute")]
[numthreads(8, 8, 1)]
void sharpen_fill(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.bufWidth || tid.y >= params.bufHeight)
        return;
    const uint n = params.channels;
    const uint dst = (tid.y * params.bufWidth + tid.x) * n;
    if (params.srcX1 >= params.srcX2 || params.srcY1 >= params.srcY2)
    {
        for (uint c = 0; c < n && c < 4; ++c)
            outBuf[dst + c] = 0u;
        return;
    }
    const int rx = clamp(params.bufX1 + int(tid.x), params.rodX1, params.rodX2 - 1);
    const int ry = clamp(params.bufY1 + int(tid.y), params.rodY1, params.rodY2 - 1);
    const int cx = clamp(rx, params.srcX1, params.srcX2 - 1);
    const int cy = clamp(ry, params.srcY1, params.srcY2 - 1);
    const uint src = params.srcOffset + uint(cy - params.srcY1) * params.srcStride + uint(cx - params.srcX1) * n;
    for (uint c = 0; c < n && c < 4; ++c)
        outBuf[dst + c] = inBuf[src + c];
}

// Sharpen::thresholdWeight().
float thresholdWeight(float d, float b)
{
    const float t = params.threshold;
    if (t <= 0.0)
        return 1.0;
    const float r = abs(d) / max(abs(b), params.epsilon);
    return min(1.0, max(0.0, (r - t) / t));
}

// combineSample() of each channel; zero outside the CPU's buffer.
[shader("compute")]
[numthreads(8, 8, 1)]
void sharpen_combine(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = min(params.channels, 4u);
    const uint dst = params.dstOffset + tid.y * params.dstStride + tid.x * params.channels;
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    if (x < params.domX1 || x >= params.domX2 || y < params.domY1 || y >= params.domY2)
    {
        for (uint c = 0; c < n; ++c)
            outBuf[dst + c] = 0u;
        return;
    }
    const uint k = (uint(y - params.bufY1) * params.bufWidth + uint(x - params.bufX1)) * params.channels;
    const uint kb = (uint(y - params.blurY1) * params.blurWidth + uint(x - params.blurX1)) * params.channels;
    for (uint c = 0; c < n; ++c)
    {
        const float v = asfloat(inBuf[k + c]);
        const float b = asfloat(blurBuf[kb + c]);
        const float d = v - b;
        float o;
        if (params.mode == kModeLaplacian)
        {
            o = params.amount * d;
        }
        else
        {
            const float wd = params.amount * thresholdWeight(d, b) * d;
            o = (params.mode == kModeSoften) ? (v - wd) : (v + wd);
        }
        outBuf[dst + c] = asuint(o);
    }
}
