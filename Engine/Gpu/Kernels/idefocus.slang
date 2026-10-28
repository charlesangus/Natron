import mapremap;

// IDefocus's gather, after the workers of Engine/Nodes/Filter/IDefocus.cpp. The work buffer covers
// the output grown by the halo, lvWidth x lvHeight pixels from (lvX1, lvY1), channels + 1 floats
// per pixel: the pixel's level, then its boosted source. Every output pixel visits each pixel of
// its apron and adds that pixel's own kernel weight at their offset, if any, so a pixel receives
// the bokeh of every neighbour that reaches it, divided by the sum of their weights.
//
// The table: a header of kHeader uints (MapHeader), then for levels 0 .. count three uints each,
// the index in the table of the level's weights, its haloX and its haloY, then the weights row by
// row from dy = -haloY, each row from dx = -haloX.
//
// IDefocusGpuParams and IDefocusGpuHeader in Engine/Nodes/Filter/IDefocus.cpp mirror these.

StructuredBuffer<uint> srcBuf;
StructuredBuffer<uint> mapBuf;
StructuredBuffer<uint> tableBuf;
RWStructuredBuffer<float> workBuf;
// gather: outWidth x outHeight pixels at (outX1, outY1), the first at dstOffset.
RWStructuredBuffer<float> outBuf;

struct IDefocusParams
{
    int lvX1;
    int lvY1;
    uint lvWidth;
    uint lvHeight;
    uint channels;
    // kBoost while the highlight boost is on.
    uint flags;
    float threshold;
    float gain;
    int haloX;
    int haloY;
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint dstOffset;
    uint dstStride;
    // The source pixels that exist, the first at srcOffset; empty when there are none.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    uint pad0;
    uint pad1;
};
[[vk::push_constant]] ConstantBuffer<IDefocusParams> params;

// Mirrored in Engine/Nodes/Filter/IDefocus.cpp.
static const uint kHeader = 16;
static const uint kBoost = 1;
static const uint kMapConnected = 1;
static const uint kMapRead = 2;
static const uint kMapInvert = 4;
static const uint kMapSmooth = 8;

// Header words.
static const uint kMapFlags = 0;
static const uint kMapChannel = 1;
static const uint kMapChannels = 2;
static const uint kMapOffset = 3;
static const uint kMapStride = 4;
static const uint kMapX1 = 5;
static const uint kMapY1 = 6;
static const uint kMapX2 = 7;
static const uint kMapY2 = 8;
static const uint kExponent = 9;
static const uint kCount = 10;

bool isFiniteValue(float v)
{
    return !isnan(v) && !isinf(v);
}

// IDefocus::levelOf().
uint levelOf(float m, uint count)
{
    precise float scaled = m * float(count);
    precise float rounded = scaled + 0.5;
    if (!(rounded > 0.0))
        return 0u;
    return min(count, uint(rounded));
}

// The level of pixel (x, y): the remapped Map, black outside it, or the Size with no Map.
uint mapLevel(int x, int y)
{
    const uint flags = tableBuf[kMapFlags];
    const uint count = tableBuf[kCount];
    if ((flags & kMapConnected) == 0u)
        return count;
    float raw = 0.0;
    if ((flags & kMapRead) != 0u && x >= int(tableBuf[kMapX1]) && x < int(tableBuf[kMapX2]) && y >= int(tableBuf[kMapY1])
        && y < int(tableBuf[kMapY2])) {
        const uint at = tableBuf[kMapOffset] + uint(y - int(tableBuf[kMapY1])) * tableBuf[kMapStride]
            + uint(x - int(tableBuf[kMapX1])) * tableBuf[kMapChannels] + tableBuf[kMapChannel];
        raw = asfloat(mapBuf[at]);
    }
    const float m = mapRemap(raw, (flags & kMapInvert) != 0u, (flags & kMapSmooth) != 0u, asfloat(tableBuf[kExponent]));
    return levelOf(m, count);
}

bool insideSource(int x, int y)
{
    return x >= params.srcX1 && x < params.srcX2 && y >= params.srcY1 && y < params.srcY2;
}

uint sourceIndex(int x, int y)
{
    return params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.channels;
}

// Each pixel of the output grown by the halo: its level, and boostWorker()'s source, 0 where it
// is not finite or outside the source.
[shader("compute")]
[numthreads(8, 8, 1)]
void idefocus_load(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.lvWidth || tid.y >= params.lvHeight)
        return;
    const uint n = params.channels;
    const uint dst = (tid.y * params.lvWidth + tid.x) * (n + 1);
    const int x = params.lvX1 + int(tid.x);
    const int y = params.lvY1 + int(tid.y);
    workBuf[dst] = float(mapLevel(x, y));
    if (!insideSource(x, y)) {
        for (uint c = 0; c < n && c < 4; ++c)
            workBuf[dst + 1 + c] = 0.0;
        return;
    }
    const uint src = sourceIndex(x, y);
    const bool boost = (params.flags & kBoost) != 0u;
    for (uint c = 0; c < n && c < 4; ++c) {
        const float v = asfloat(srcBuf[src + c]);
        const float b = (boost && v > params.threshold) ? params.threshold + (v - params.threshold) * params.gain : v;
        workBuf[dst + 1 + c] = isFiniteValue(b) ? b : 0.0;
    }
}

// gatherWorker(): every output pixel, the weighted sum of what reaches it over the weights, or
// the source where nothing does or only the pixel itself with the single tap.
[shader("compute")]
[numthreads(8, 8, 1)]
void idefocus_gather(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = params.channels;
    const uint stride = n + 1;
    const uint dst = params.dstOffset + tid.y * params.dstStride + tid.x * n;
    // The output pixel in the work buffer's coordinates.
    const int px = int(tid.x) + params.haloX;
    const int py = int(tid.y) + params.haloY;
    float acc[4] = { 0.0, 0.0, 0.0, 0.0 };
    float weight = 0.0;
    uint count = 0u;
    for (int dy = -params.haloY; dy <= params.haloY; ++dy) {
        const int sy = py - dy;
        for (int dx = -params.haloX; dx <= params.haloX; ++dx) {
            const int sx = px - dx;
            const uint at = (uint(sy) * params.lvWidth + uint(sx)) * stride;
            const uint level = uint(workBuf[at]);
            const uint info = kHeader + 3u * level;
            const int hx = int(tableBuf[info + 1]);
            const int hy = int(tableBuf[info + 2]);
            if (dx < -hx || dx > hx || dy < -hy || dy > hy)
                continue;
            const float w = asfloat(tableBuf[tableBuf[info] + uint(dy + hy) * uint(2 * hx + 1) + uint(dx + hx)]);
            if (w == 0.0)
                continue;
            weight += w;
            ++count;
            for (uint c = 0; c < n && c < 4; ++c)
                acc[c] += w * workBuf[at + 1 + c];
        }
    }
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    const uint own = uint(workBuf[(uint(py) * params.lvWidth + uint(px)) * stride]);
    const uint ownInfo = kHeader + 3u * own;
    const bool ownIdentity = tableBuf[ownInfo + 1] == 0u && tableBuf[ownInfo + 2] == 0u;
    const bool keep = count == 0u || !(weight > 0.0) || (count == 1u && ownIdentity);
    const bool boost = (params.flags & kBoost) != 0u;
    const bool inside = insideSource(x, y);
    const uint src = inside ? sourceIndex(x, y) : 0u;
    for (uint c = 0; c < n && c < 4; ++c) {
        float v;
        if (keep) {
            v = inside ? asfloat(srcBuf[src + c]) : 0.0;
        } else {
            v = acc[c] / weight;
            if (boost && v > params.threshold)
                v = params.threshold + (v - params.threshold) / params.gain;
        }
        outBuf[dst + c] = v;
    }
}
