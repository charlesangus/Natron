/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/iop/retouch.c,
 * Copyright (C) 2017-2026 darktable developers, distributed under the GNU GPL version 3 or later,
 * with the arithmetic of the CPU port in Engine/Nodes/Filter/RetouchAlgorithms.cpp. The wavelets,
 * the heal and the gaussian blur run on Kernels/wavelets.slang, heal.slang and eigf.slang.
 *
 * imgBuf is 4 interleaved channels, tightly packed and top-down, width x height, the source's
 * region from (fullX1, fullY2 - 1) at the top-left. auxBuf and maskBuf cover a shape's region of
 * maskW x maskH pixels from (maskX, maskY) in imgBuf, auxBuf with 4 channels. srcBuf and dstBuf
 * are image views, rows bottom-up.
 */

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> imgBuf;
RWStructuredBuffer<float> auxBuf;
StructuredBuffer<float> maskBuf;
// The result over the output rect, nComps floats a pixel, rows bottom-up.
RWStructuredBuffer<float> dstBuf;

// RetouchGpuParams in Engine/Nodes/Filter/Retouch.cpp mirrors it.
struct RtParams
{
    uint width;
    uint height;
    int fullX1;
    int fullY2;
    uint srcOffset;
    uint srcStride;
    uint srcComps;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    int srcIndex0;
    int srcIndex1;
    int srcIndex2;
    int srcIndex3;
    uint nComps;
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    int maskX;
    int maskY;
    uint maskW;
    uint maskH;
    int dx;
    int dy;
    float opacity;
    uint fill;
    float fill0;
    float fill1;
    float fill2;
    uint padding0;
};
[[vk::push_constant]] ConstantBuffer<RtParams> pc;

int srcIndex(uint c)
{
    if (c == 0)
        return pc.srcIndex0;
    if (c == 1)
        return pc.srcIndex1;
    if (c == 2)
        return pc.srcIndex2;
    return pc.srcIndex3;
}

// Channel c of the plane at pixel (x, y), zero outside the source view.
float srcPixel(int x, int y, uint c)
{
    if (x < pc.srcX1 || x >= pc.srcX2 || y < pc.srcY1 || y >= pc.srcY2)
        return 0.0f;
    const int idx = srcIndex(c);
    if (idx < 0)
        return 0.0f;
    return srcBuf[pc.srcOffset + uint(y - pc.srcY1) * pc.srcStride + uint(x - pc.srcX1) * pc.srcComps + uint(idx)];
}

bool isFiniteCpu(float v)
{
    return !isnan(v) && !isinf(v);
}

// The source over the region in 4 channels, one channel going to all three colours; a non-finite
// sample is 0, as the decomposition takes it.
[shader("compute")]
[numthreads(16, 16, 1)]
void retouch_load(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.width || tid.y >= pc.height)
        return;
    const int x = pc.fullX1 + int(tid.x);
    const int y = pc.fullY2 - 1 - int(tid.y);
    const uint base = (tid.y * pc.width + tid.x) * 4;
    for (uint c = 0; c < 4; ++c)
    {
        float v = 0.0f;
        if (pc.nComps == 1)
            v = c < 3 ? srcPixel(x, y, 0) : 0.0f;
        else if (c < pc.nComps)
            v = srcPixel(x, y, c);
        imgBuf[base + c] = isFiniteCpu(v) ? v : 0.0f;
    }
}

// rt_copy_in_to_out(): auxBuf from imgBuf shifted by (dx, dy).
[shader("compute")]
[numthreads(16, 16, 1)]
void retouch_crop(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.maskW || tid.y >= pc.maskH)
        return;
    const uint sx = uint(pc.maskX + int(tid.x) - pc.dx);
    const uint sy = uint(pc.maskY + int(tid.y) - pc.dy);
    const uint from = (sy * pc.width + sx) * 4;
    const uint to = (tid.y * pc.maskW + tid.x) * 4;
    for (uint c = 0; c < 4; ++c)
        auxBuf[to + c] = imgBuf[from + c];
}

// rt_copy_image_masked() of auxBuf, or with fill set retouch_fill() of the fill colour.
[shader("compute")]
[numthreads(16, 16, 1)]
void retouch_blend(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.maskW || tid.y >= pc.maskH)
        return;
    const uint k = tid.y * pc.maskW + tid.x;
    const uint d = (uint(pc.maskY + int(tid.y)) * pc.width + uint(pc.maskX + int(tid.x))) * 4;
    const float f = maskBuf[k] * pc.opacity;
    const float f1 = 1.0f - f;
    const float fillColour[4] = { pc.fill0, pc.fill1, pc.fill2, 0.0f };
    for (uint c = 0; c < 4; ++c)
    {
        const float s = pc.fill != 0 ? fillColour[c] : auxBuf[4 * k + c];
        imgBuf[d + c] = imgBuf[d + c] * f1 + s * f;
    }
}

// The output rect: imgBuf inside the region, the source outside it and where it is not finite.
[shader("compute")]
[numthreads(16, 16, 1)]
void retouch_store(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.outWidth || tid.y >= pc.outHeight)
        return;
    const int x = pc.outX1 + int(tid.x);
    const int y = pc.outY1 + int(tid.y);
    const uint nComps = pc.nComps;
    const uint base = (tid.y * pc.outWidth + tid.x) * nComps;
    const int col = x - pc.fullX1;
    const int row = pc.fullY2 - 1 - y;
    const bool inside = col >= 0 && row >= 0 && col < int(pc.width) && row < int(pc.height);
    for (uint c = 0; c < nComps; ++c)
    {
        const float s = srcPixel(x, y, c);
        if (!inside || c >= 4 || !isFiniteCpu(s))
            dstBuf[base + c] = s;
        else
            dstBuf[base + c] = imgBuf[(uint(row) * pc.width + uint(col)) * 4 + c];
    }
}
