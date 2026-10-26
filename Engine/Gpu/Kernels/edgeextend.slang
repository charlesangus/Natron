// EdgeExtend's own passes, after the workers of Engine/Nodes/Filter/EdgeExtend.cpp: the source fill
// with the nearest pixel repeated and the running result started from the solid pixels, the
// running result repeated past the CPU's buffer for a slice's blur, the merge of the result over
// its blur, and the extended colour into the output. The blur of each slice is blur.slang's. The running result,
// the extended and the blurred buffers are tightly packed, `channels` floats per pixel.
// EdgeExtendGpuParams in Engine/Nodes/Filter/EdgeExtend.cpp mirrors EdgeExtendParams.

// extend: the running result; finish: the running result.
StructuredBuffer<uint> inBuf;
// over: the blurred result; finish: the source image; fill and extend: bound but unread.
StructuredBuffer<uint> blurBuf;
// fill: the running result; extend: the extended result; over: the running result, read and
// written in place; finish: the rect of outWidth x outHeight pixels at (outX1, outY1), whose first
// pixel is at dstOffset.
RWStructuredBuffer<uint> outBuf;

struct EdgeExtendParams
{
    // The running result's rect, the CPU's buffer.
    int bufX1;
    int bufY1;
    uint bufWidth;
    uint bufHeight;
    // fill and extend: the rect written.
    int extX1;
    int extY1;
    uint extWidth;
    uint extHeight;
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint dstOffset;
    uint dstStride;
    uint channels;
    // The four channels are processed: without them every pass leaves the source as it is.
    uint active;
    // The source pixels that exist, the first at srcOffset; empty when there are none.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    // There is a slice: fill starts the running result and finish ends it; without one both copy.
    uint extend;
    // The source's colour is divided by its alpha, where that exceeds epsilon, when it is solid.
    uint sourcePremultiplied;
    // The extended colour is multiplied by the source's alpha.
    uint premultiplyOutput;
    float epsilon;
    // The source's alpha at or above which a pixel is solid.
    float threshold;
};
[[vk::push_constant]] ConstantBuffer<EdgeExtendParams> params;

bool hasSource()
{
    return params.srcX1 < params.srcX2 && params.srcY1 < params.srcY2;
}

// The float index of the source pixel nearest to (x, y), which hasSource() must allow.
uint sourceIndex(int x, int y)
{
    const int cx = clamp(x, params.srcX1, params.srcX2 - 1);
    const int cy = clamp(y, params.srcY1, params.srcY2 - 1);
    return params.srcOffset + uint(cy - params.srcY1) * params.srcStride + uint(cx - params.srcX1) * params.channels;
}

// fillBuffer() with the nearest-pixel boundary, then prepareWorker(): the straight colour with an
// alpha of 1 where the source is solid, zero elsewhere; zero when the source has no pixel.
[shader("compute")]
[numthreads(8, 8, 1)]
void edgeextend_fill(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.extWidth || tid.y >= params.extHeight)
        return;
    const uint n = params.channels;
    const uint dst = (tid.y * params.extWidth + tid.x) * n;
    if (!hasSource())
    {
        for (uint c = 0; c < n && c < 4; ++c)
            outBuf[dst + c] = 0u;
        return;
    }
    const uint src = sourceIndex(params.extX1 + int(tid.x), params.extY1 + int(tid.y));
    if (params.active != 0 && params.extend != 0)
    {
        const float a = asfloat(inBuf[src + 3]);
        const bool solid = a >= params.threshold;
        for (uint c = 0; c < 3; ++c)
        {
            float v = asfloat(inBuf[src + c]);
            if (!solid)
                v = 0.0;
            else if (params.sourcePremultiplied != 0 && a > params.epsilon)
                v = v / a;
            outBuf[dst + c] = asuint(v);
        }
        outBuf[dst + 3] = asuint(solid ? 1.0 : 0.0);
        return;
    }
    for (uint c = 0; c < n && c < 4; ++c)
        outBuf[dst + c] = inBuf[src + c];
}

// The running result over the extended rect, each pixel the nearest of the CPU's buffer: the
// blur then reads the line ends the CPU's Neumann filter repeats.
[shader("compute")]
[numthreads(8, 8, 1)]
void edgeextend_extend(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.extWidth || tid.y >= params.extHeight)
        return;
    const uint n = params.channels;
    const int x = clamp(params.extX1 + int(tid.x), params.bufX1, params.bufX1 + int(params.bufWidth) - 1);
    const int y = clamp(params.extY1 + int(tid.y), params.bufY1, params.bufY1 + int(params.bufHeight) - 1);
    const uint src = (uint(y - params.bufY1) * params.bufWidth + uint(x - params.bufX1)) * n;
    const uint dst = (tid.y * params.extWidth + tid.x) * n;
    for (uint c = 0; c < n && c < 4; ++c)
        outBuf[dst + c] = inBuf[src + c];
}

// overWorker(): the running result over its blur, a + b * (1 - alpha a), in place.
[shader("compute")]
[numthreads(8, 8, 1)]
void edgeextend_over(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.bufWidth || tid.y >= params.bufHeight || params.active == 0)
        return;
    const uint k = (tid.y * params.bufWidth + tid.x) * params.channels;
    const float t = 1.0 - asfloat(outBuf[k + 3]);
    for (uint c = 0; c < 4; ++c)
        outBuf[k + c] = asuint(asfloat(outBuf[k + c]) + asfloat(blurBuf[k + c]) * t);
}

// finishWorker() into the output: the colour divided by the grown alpha, multiplied by the source's
// alpha with premultiplyOutput, and the source's alpha; zero outside the CPU's buffer.
[shader("compute")]
[numthreads(8, 8, 1)]
void edgeextend_finish(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = min(params.channels, 4u);
    const uint dst = params.dstOffset + tid.y * params.dstStride + tid.x * params.channels;
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    if (x < params.bufX1 || x >= params.bufX1 + int(params.bufWidth) || y < params.bufY1 || y >= params.bufY1 + int(params.bufHeight))
    {
        for (uint c = 0; c < n; ++c)
            outBuf[dst + c] = 0u;
        return;
    }
    const uint k = (uint(y - params.bufY1) * params.bufWidth + uint(x - params.bufX1)) * params.channels;
    if (params.active == 0 || params.extend == 0)
    {
        for (uint c = 0; c < n; ++c)
            outBuf[dst + c] = inBuf[k + c];
        return;
    }
    const float a = asfloat(inBuf[k + 3]);
    const float sourceAlpha = hasSource() ? asfloat(blurBuf[sourceIndex(x, y) + 3]) : 0.0;
    for (uint c = 0; c < 3; ++c)
    {
        float v = asfloat(inBuf[k + c]);
        if (a > params.epsilon)
            v = v / a;
        outBuf[dst + c] = asuint(params.premultiplyOutput != 0 ? v * sourceAlpha : v);
    }
    outBuf[dst + 3] = asuint(sourceAlpha);
}
