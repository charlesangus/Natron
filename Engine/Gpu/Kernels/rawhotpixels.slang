/*
 * The maths are a port of darktable's hotpixels module, DARKTABLE_REF release-5.6.2 (commit
 * 20891f6c6fa7), src/iop/hotpixels.c, C path only, Copyright (C) 2011-2026 darktable
 * developers, distributed under the GNU GPL version 3 or later, kept in step with
 * RawHotPixels.cpp.
 */

import rawlayout;

// fill: the source image; detect: the filled domain buffer.
StructuredBuffer<uint> inBuf;
// fill: the domain buffer, domWidth x domHeight pixels of `channels` floats; detect: the result.
RWStructuredBuffer<uint> outBuf;

// RawHotPixelsGpuParams in Engine/Nodes/Raw/RawGpuParams.h mirrors it. The layout is RawLayoutGpu:
// period, originX, originY and a reserved word, then the 36 cell colours in nine words and three
// of padding, held as vectors so that the push block's array rules do not matter.
struct HotPixelsParams
{
    int4 head;
    uint4 cells0;
    uint4 cells1;
    uint4 cells2;
    float threshold;
    float multiplier;
    uint minNeighbours;
    uint channels;
    // The photosites the passes run over: the CPU's buffer.
    int domX1;
    int domY1;
    int domX2;
    int domY2;
    // fill: the source pixels that exist, the first at offset; empty when there are none.
    // detect: the rect written, whose first pixel is at offset.
    int rectX1;
    int rectY1;
    int rectX2;
    int rectY2;
    uint offset;
    uint stride;
    uint padding0;
    uint padding1;
};
[[vk::push_constant]] ConstantBuffer<HotPixelsParams> params;

// darktable's search order for the nearest photosites of the same colour on an X-Trans sensor, as
// (column, row) offsets with rows counted downwards.
static const int2 kXTransSearch[20] = {
    int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
    int2(-1, -1), int2(-1, 1), int2(1, -1), int2(1, 1),
    int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2),
    int2(-2, -1), int2(-2, 1), int2(2, -1), int2(2, 1),
    int2(-1, -2), int2(1, -2), int2(-1, 2), int2(1, 2),
};

RawLayout loadLayout()
{
    RawLayout l;
    l.period = params.head.x;
    l.originX = params.head.y;
    l.originY = params.head.z;
    l.cells[0] = params.cells0.x;
    l.cells[1] = params.cells0.y;
    l.cells[2] = params.cells0.z;
    l.cells[3] = params.cells0.w;
    l.cells[4] = params.cells1.x;
    l.cells[5] = params.cells1.y;
    l.cells[6] = params.cells1.z;
    l.cells[7] = params.cells1.w;
    l.cells[8] = params.cells2.x;
    return l;
}

// The source divided or not, copied over the domain and zero outside the pixels that exist.
[shader("compute")]
[numthreads(8, 8, 1)]
void rawhotpixels_fill(uint3 tid : SV_DispatchThreadID)
{
    const int w = params.domX2 - params.domX1;
    const int h = params.domY2 - params.domY1;
    if (tid.x >= uint(max(w, 0)) || tid.y >= uint(max(h, 0)))
        return;
    const uint n = params.channels;
    const uint dst = (tid.y * uint(w) + tid.x) * n;
    const int x = params.domX1 + int(tid.x);
    const int y = params.domY1 + int(tid.y);
    const bool inside = x >= params.rectX1 && x < params.rectX2 && y >= params.rectY1 && y < params.rectY2;
    const uint src = params.offset + uint(y - params.rectY1) * params.stride + uint(x - params.rectX1) * n;
    for (uint c = 0; c < n && c < 4; ++c)
        outBuf[dst + c] = inside ? inBuf[src + c] : 0u;
}

// hotPixelsWorker(): one photosite of one channel. The domain's outermost photosites, two on a
// mosaic and one without, are never examined and keep their value.
[shader("compute")]
[numthreads(8, 8, 1)]
void rawhotpixels_detect(uint3 tid : SV_DispatchThreadID)
{
    const int outW = params.rectX2 - params.rectX1;
    const int outH = params.rectY2 - params.rectY1;
    if (tid.x >= uint(max(outW, 0)) || tid.y >= uint(max(outH, 0)))
        return;
    const RawLayout layout = loadLayout();
    const int period = layout.period;
    const int border = period > 0 ? 2 : 1;
    const int width = params.domX2 - params.domX1;
    const int height = params.domY2 - params.domY1;
    const uint n = params.channels;
    const int x = params.rectX1 + int(tid.x);
    const int y = params.rectY1 + int(tid.y);
    const int i = x - params.domX1;
    const int row = y - params.domY1;
    const uint dst = params.offset + tid.y * params.stride + tid.x * n;
    const bool inDomain = i >= 0 && i < width && row >= 0 && row < height;
    const bool examined = i >= border && i < width - border && row >= border && row < height - border;

    for (uint c = 0; c < n && c < 4; ++c)
    {
        uint result = 0u;
        if (inDomain)
        {
            result = inBuf[(uint(row) * uint(width) + uint(i)) * n + c];
            if (examined)
            {
                const float value = asfloat(result);
                if (value > params.threshold)
                {
                    precise float mid = value * params.multiplier;
                    uint count = 0;
                    float maxin = 0.0;
                    if (period == 6)
                    {
                        const uint colour = layout.colourAt(x, y);
                        int found = 0;
                        [loop]
                        for (int s = 0; s < 20 && found < 4; ++s)
                        {
                            const int dx = kXTransSearch[s].x;
                            const int dy = -kXTransSearch[s].y;
                            if (layout.colourAt(x + dx, y + dy) != colour)
                                continue;
                            ++found;
                            const float other = asfloat(inBuf[(uint(row + dy) * uint(width) + uint(i + dx)) * n + c]);
                            if (mid > other)
                            {
                                ++count;
                                if (other > maxin)
                                    maxin = other;
                            }
                        }
                    }
                    else
                    {
                        const int d = period == 2 ? 2 : 1;
                        [ForceUnroll]
                        for (int k = 0; k < 4; ++k)
                        {
                            const int dx = k == 0 ? -d : (k == 2 ? d : 0);
                            const int dy = k == 1 ? -d : (k == 3 ? d : 0);
                            const float other = asfloat(inBuf[(uint(row + dy) * uint(width) + uint(i + dx)) * n + c]);
                            if (mid > other)
                            {
                                ++count;
                                if (other > maxin)
                                    maxin = other;
                            }
                        }
                    }
                    if (count >= params.minNeighbours)
                        result = asuint(maxin);
                }
            }
        }
        outBuf[dst + c] = result;
    }
}
