// The source image.
StructuredBuffer<uint> inBuf;
// The filtered image the pass list writes.
RWStructuredBuffer<uint> outBuf;

// AdjustBBoxGpuParams in Engine/Nodes/Transform/AdjustBBox.cpp mirrors it.
struct AdjustBBoxParams
{
    // The source pixels that exist, the first at srcOffset; empty when there are none.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    // The rect written, its first pixel at dstOffset.
    int dstX1;
    int dstY1;
    int dstX2;
    int dstY2;
    uint srcOffset;
    uint srcStride;
    uint dstOffset;
    uint dstStride;
    uint channels;
    uint padding0;
    uint padding1;
    uint padding2;
};
[[vk::push_constant]] ConstantBuffer<AdjustBBoxParams> params;

// Each pixel of the rect written takes the nearest source pixel that exists, bit for bit, or zero
// when none does.
[shader("compute")]
[numthreads(8, 8, 1)]
void adjustbbox_nearest(uint3 tid : SV_DispatchThreadID)
{
    const int w = params.dstX2 - params.dstX1;
    const int h = params.dstY2 - params.dstY1;
    if (tid.x >= uint(max(w, 0)) || tid.y >= uint(max(h, 0)))
        return;
    const uint n = params.channels;
    const uint dst = params.dstOffset + tid.y * params.dstStride + tid.x * n;
    if (params.srcX2 <= params.srcX1 || params.srcY2 <= params.srcY1)
    {
        for (uint c = 0; c < n && c < 4; ++c)
            outBuf[dst + c] = 0u;
        return;
    }
    const int x = clamp(params.dstX1 + int(tid.x), params.srcX1, params.srcX2 - 1);
    const int y = clamp(params.dstY1 + int(tid.y), params.srcY1, params.srcY2 - 1);
    const uint src = params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * n;
    for (uint c = 0; c < n && c < 4; ++c)
        outBuf[dst + c] = inBuf[src + c];
}
