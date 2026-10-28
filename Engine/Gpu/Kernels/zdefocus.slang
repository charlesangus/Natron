// ZDefocus's passes, after the workers of Engine/Nodes/Filter/ZDefocus.cpp and the block-prefix
// gather of Kernels/defocus.slang. The work buffer covers the CPU's scratch rect, the domain of
// domWidth x domHeight pixels from (domX1, domY1):
// - at 0, the boosted source, `channels` floats per pixel;
// - from cocOffset, per pixel, its nearer slice and the weight of the next one;
// - from localOffset, per channel and row, domWidth + 1 block prefixes of the current slice's
//   layer, its weights being channel `channels`;
// - from totalOffset, per channel and row, nBlocks block totals;
// - from accOffset, per output pixel, the layers composited so far and their coverage, one per
//   channel group.
//
// metaBuf holds, as uints, the depth image's valid bounds, the depth-of-field model, the channel
// groups, the slices' circles of confusion and, from runOffset, every slice's kernel runs, four
// uints each: dy, dx0, length and the weight's bits. With kLens, runOffset is instead the slice's
// cell table, two uints per kernel by group, cell row and cell column: the index of its first
// run's first uint and its run count; the cell column of each output column and the cell row of
// each output row are from kMetaColumnMap and kMetaRowMap, when there are more than one, and
// from kMetaSharp, per slice, a bit per group whose kernel leaves its channels sharp. The model works in float where the CPU works in double, and the
// slices are laid over each other in order, so that a pixel's result depends on the rounding of
// every layer before it.
//
// ZDefocusGpuParams in Engine/Nodes/Filter/ZDefocus.cpp mirrors ZDefocusParams.

StructuredBuffer<uint> srcBuf;
StructuredBuffer<uint> depthBuf;
StructuredBuffer<uint> metaBuf;
RWStructuredBuffer<float> workBuf;
// composite and finish: outWidth x outHeight pixels at (outX1, outY1), the first at dstOffset.
RWStructuredBuffer<float> outBuf;

struct ZDefocusParams
{
    int domX1;
    int domY1;
    uint domWidth;
    uint domHeight;
    uint channels;
    uint nBlocks;
    uint cocOffset;
    uint localOffset;
    uint totalOffset;
    uint accOffset;
    uint runOffset;
    uint runCount;
    uint slice;
    uint flags;
    float threshold;
    float gain;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint dstOffset;
    uint dstStride;
    uint depthOffset;
    uint depthStride;
    uint depthComps;
    uint depthComponent;
};
[[vk::push_constant]] ConstantBuffer<ZDefocusParams> params;

// Mirrored in Engine/Nodes/Filter/ZDefocus.cpp.
static const uint kBlock = 256;
static const uint kBoost = 1;
static const uint kSharp = 2;
static const uint kFirst = 4;
static const uint kInverseDistance = 8;
static const uint kHasDepth = 16;
static const uint kLens = 32;
static const uint kMetaDepthX1 = 0;
static const uint kMetaDepthY1 = 1;
static const uint kMetaDepthX2 = 2;
static const uint kMetaDepthY2 = 3;
static const uint kMetaNear = 4;
static const uint kMetaFar = 5;
static const uint kMetaScale = 6;
static const uint kMetaMinDistance = 7;
static const uint kMetaMaxSize = 8;
static const uint kMetaSnapX = 9;
static const uint kMetaSnapY = 10;
static const uint kMetaSliceCount = 11;
// The largest channel's circle of confusion scale.
static const uint kMetaCocScale = 12;
// Two bits per channel, its group; from bit 8, the number of groups.
static const uint kMetaGroups = 13;
static const uint kMetaCellColumns = 14;
static const uint kMetaCellRows = 15;
static const uint kMetaColumnMap = 16;
static const uint kMetaRowMap = 17;
static const uint kMetaSharp = 18;
static const uint kMetaCentres = 19;

bool isFiniteValue(float v)
{
    return !isnan(v) && !isinf(v);
}

float metaFloat(uint i)
{
    return asfloat(metaBuf[i]);
}

uint groupCount()
{
    return metaBuf[kMetaGroups] >> 8;
}

uint groupOf(uint c)
{
    return (metaBuf[kMetaGroups] >> (2 * c)) & 3u;
}

float inverseOf(float distance)
{
    return isinf(distance) ? 0.0 : 1.0 / distance;
}

// The raw depth at pixel (x, y), 0 outside the depth image, as readChannelRow() fills it.
float rawDepth(int x, int y)
{
    if ((params.flags & kHasDepth) == 0u)
        return 0.0;
    const int x1 = int(metaBuf[kMetaDepthX1]);
    const int y1 = int(metaBuf[kMetaDepthY1]);
    if (x < x1 || y < y1 || x >= int(metaBuf[kMetaDepthX2]) || y >= int(metaBuf[kMetaDepthY2]))
        return 0.0;
    return asfloat(depthBuf[params.depthOffset + uint(y - y1) * params.depthStride + uint(x - x1) * params.depthComps + params.depthComponent]);
}

// DepthOfField::coc(): the signed circle of confusion, in canonical pixels, of a raw depth.
float circleOfConfusion(float raw)
{
    float distance = asfloat(0x7f800000u);
    if (isFiniteValue(raw) && raw > 0.0) {
        distance = ((params.flags & kInverseDistance) != 0u) ? 1.0 / raw : raw;
        if (!isFiniteValue(distance))
            distance = asfloat(0x7f800000u);
    }
    const float nearEdge = metaFloat(kMetaNear);
    const float farEdge = metaFloat(kMetaFar);
    const float scale = metaFloat(kMetaScale);
    const float maxSize = metaFloat(kMetaMaxSize);
    if (scale <= 0.0)
        return 0.0;
    if (distance < nearEdge) {
        if (distance < metaFloat(kMetaMinDistance) || isinf(scale))
            return -maxSize;
        return -min(maxSize, scale * (1.0 / distance - inverseOf(nearEdge)));
    }
    if (distance > farEdge) {
        if (isinf(scale))
            return maxSize;
        return min(maxSize, scale * (1.0 / farEdge - inverseOf(distance)));
    }
    return 0.0;
}

float sourceValue(int x, int y, uint c)
{
    if (x < params.srcX1 || x >= params.srcX2 || y < params.srcY1 || y >= params.srcY2)
        return 0.0;
    return asfloat(srcBuf[params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.channels + c]);
}

float boost(float v)
{
    return ((params.flags & kBoost) != 0u && v > params.threshold) ? params.threshold + (v - params.threshold) * params.gain : v;
}

float unboost(float v)
{
    return ((params.flags & kBoost) != 0u && v > params.threshold) ? params.threshold + (v - params.threshold) / params.gain : v;
}

// boostWorker() and the slices of renderOp(): the boosted source, and each pixel's nearer slice
// and the weight of the next one, a tent between the two.
[shader("compute")]
[numthreads(8, 8, 1)]
void zdefocus_load(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.domWidth || tid.y >= params.domHeight)
        return;
    const uint n = params.channels;
    const uint pixel = tid.y * params.domWidth + tid.x;
    const int x = params.domX1 + int(tid.x);
    const int y = params.domY1 + int(tid.y);
    for (uint c = 0; c < n && c < 4; ++c)
        workBuf[pixel * n + c] = boost(sourceValue(x, y, c));

    float coc = circleOfConfusion(rawDepth(x, y));
    // pixelCoc(): a kernel of the largest channel's scale that leaves the pixel unchanged is no blur.
    const float cocScale = metaFloat(kMetaCocScale);
    const float size = (cocScale == 1.0) ? abs(coc) : min(abs(coc) * cocScale, max(0.0, metaFloat(kMetaMaxSize)));
    if (!(size * metaFloat(kMetaSnapX) >= 0.5) && !(size * metaFloat(kMetaSnapY) >= 0.5))
        coc = 0.0;
    const uint count = metaBuf[kMetaSliceCount];
    uint k = 0;
    while (k < count && metaFloat(kMetaCentres + k) <= coc)
        ++k;
    float t = 0.0;
    if (k == 0) {
        k = 0;
    } else if (k >= count) {
        k = count - 1;
    } else {
        --k;
        const float lo = metaFloat(kMetaCentres + k);
        t = (coc - lo) / (metaFloat(kMetaCentres + k + 1) - lo);
    }
    workBuf[params.cocOffset + 2 * pixel] = float(k);
    workBuf[params.cocOffset + 2 * pixel + 1] = t;
}

// The weight of params.slice at domain pixel (x, y).
float sliceWeight(uint x, uint y)
{
    const uint pixel = y * params.domWidth + x;
    const uint s = uint(workBuf[params.cocOffset + 2 * pixel]);
    const float t = workBuf[params.cocOffset + 2 * pixel + 1];
    if (s == params.slice)
        return 1.0 - t;
    if (s + 1 == params.slice)
        return t;
    return 0.0;
}

uint localIndex(uint c, uint y, uint x)
{
    return params.localOffset + (c * params.domHeight + y) * (params.domWidth + 1) + x;
}

uint totalIndex(uint c, uint y, uint k)
{
    return params.totalOffset + (c * params.domHeight + y) * params.nBlocks + k;
}

// prefixWorker(): block tid.x of row tid.y of the slice's layer, the boosted source times the
// slice's weights and, as channel `channels`, the weights. The last block also writes the
// prefix at domWidth, 0 when it starts a block of its own.
[shader("compute")]
[numthreads(64, 1, 1)]
void zdefocus_blocks(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.nBlocks || tid.y >= params.domHeight)
        return;
    const uint n = params.channels;
    const uint y = tid.y;
    const uint x0 = tid.x * kBlock;
    const uint x1 = min(x0 + kBlock, params.domWidth);
    const bool last = (tid.x == params.nBlocks - 1);
    for (uint c = 0; c <= n && c <= 4; ++c) {
        float sum = 0.0;
        for (uint x = x0; x < x1; ++x) {
            workBuf[localIndex(c, y, x)] = sum;
            const float w = sliceWeight(x, y);
            const float v = (c == n) ? w : w * workBuf[(y * params.domWidth + x) * n + c];
            if (isFiniteValue(v))
                sum += v;
        }
        workBuf[totalIndex(c, y, tid.x)] = sum;
        if (last)
            workBuf[localIndex(c, y, params.domWidth)] = ((params.domWidth % kBlock) == 0u) ? 0.0 : sum;
    }
}

// The sum of channel c of row y over columns [a, b), a < b.
float rangeSum(uint c, uint y, uint a, uint b)
{
    const uint ba = a / kBlock;
    const uint bb = b / kBlock;
    if (ba == bb)
        return workBuf[localIndex(c, y, b)] - workBuf[localIndex(c, y, a)];
    float sum = workBuf[totalIndex(c, y, ba)] - workBuf[localIndex(c, y, a)];
    for (uint k = ba + 1; k < bb; ++k)
        sum += workBuf[totalIndex(c, y, k)];
    return sum + workBuf[localIndex(c, y, b)];
}

// Adds to layer, for the channels of mask, channel `channels` being the weights, the runs of
// count from metaBuf[first] gathered at domain pixel (x, y).
void gatherRuns(uint first, uint count, uint mask, int x, int y, inout float layer[5])
{
    const uint n = params.channels;
    const int w = int(params.domWidth);
    const int h = int(params.domHeight);
    for (uint r = 0; r < count; ++r) {
        const uint run = first + 4 * r;
        const int dy = int(metaBuf[run]);
        const int dx0 = int(metaBuf[run + 1]);
        const int length = int(metaBuf[run + 2]);
        const float weight = asfloat(metaBuf[run + 3]);
        const int sy = y - dy;
        if (sy < 0 || sy >= h)
            continue;
        // Pixel x sums the layer over [x - dx0 - length + 1, x - dx0 + 1).
        const int a = clamp(x - dx0 - length + 1, 0, w);
        const int b = clamp(x - dx0 + 1, 0, w);
        if (a >= b)
            continue;
        for (uint c = 0; c <= n && c <= 4; ++c) {
            if ((mask & (1u << c)) != 0u)
                layer[c] += weight * rangeSum(c, uint(sy), uint(a), uint(b));
        }
    }
}

// compositeWorker() and lensCompositeWorker(): the slice's defocused layer over what the slices
// behind it left, and its coverage over theirs, each channel group's on its own; with kSharp the
// layer is its weighted source, as with kLens is that of each group the slice leaves sharp.
[shader("compute")]
[numthreads(8, 8, 1)]
void zdefocus_composite(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = params.channels;
    const uint groups = groupCount();
    const uint acc = params.accOffset + (tid.y * params.outWidth + tid.x) * (n + groups);
    const int x = params.outX1 + int(tid.x) - params.domX1;
    const int y = params.outY1 + int(tid.y) - params.domY1;
    const int w = int(params.domWidth);
    const int h = int(params.domHeight);
    float layer[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
    float cover[3] = { 0.0, 0.0, 0.0 };
    if (x >= 0 && y >= 0 && x < w && y < h) {
        const uint pixel = (uint(y) * params.domWidth + uint(x)) * n;
        if ((params.flags & kSharp) != 0u) {
            const float weight = sliceWeight(uint(x), uint(y));
            for (uint c = 0; c < n && c < 4; ++c)
                layer[c] = weight * workBuf[pixel + c];
            for (uint g = 0; g < groups && g < 3; ++g)
                cover[g] = weight;
        } else if ((params.flags & kLens) != 0u) {
            const uint columns = metaBuf[kMetaCellColumns];
            const uint rows = metaBuf[kMetaCellRows];
            const uint col = (columns > 1u) ? metaBuf[metaBuf[kMetaColumnMap] + tid.x] : 0u;
            const uint row = (rows > 1u) ? metaBuf[metaBuf[kMetaRowMap] + tid.y] : 0u;
            const uint sharp = metaBuf[metaBuf[kMetaSharp] + params.slice];
            for (uint g = 0; g < groups && g < 3; ++g) {
                uint mask = 0u;
                for (uint c = 0; c < n && c < 4; ++c) {
                    if (groupOf(c) == g)
                        mask |= 1u << c;
                }
                if ((sharp & (1u << g)) != 0u) {
                    const float weight = sliceWeight(uint(x), uint(y));
                    for (uint c = 0; c < n && c < 4; ++c) {
                        if ((mask & (1u << c)) != 0u)
                            layer[c] = weight * workBuf[pixel + c];
                    }
                    cover[g] = weight;
                    continue;
                }
                float part[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
                const uint entry = params.runOffset + 2 * ((g * rows + row) * columns + col);
                gatherRuns(metaBuf[entry], metaBuf[entry + 1], mask | (1u << n), x, y, part);
                for (uint c = 0; c < n && c < 4; ++c) {
                    if ((mask & (1u << c)) != 0u)
                        layer[c] = part[c];
                }
                cover[g] = part[n];
            }
        } else {
            gatherRuns(params.runOffset, params.runCount, 31u, x, y, layer);
            cover[0] = layer[n];
        }
    }
    const bool first = (params.flags & kFirst) != 0u;
    for (uint c = 0; c < n && c < 4; ++c) {
        const float behind = first ? 0.0 : workBuf[acc + c];
        workBuf[acc + c] = layer[c] + max(0.0, 1.0 - cover[groupOf(c)]) * behind;
    }
    for (uint g = 0; g < groups && g < 3; ++g) {
        const float behind = first ? 0.0 : workBuf[acc + n + g];
        workBuf[acc + n + g] = cover[g] + max(0.0, 1.0 - cover[g]) * behind;
    }
}

// finishWorker(): the composite divided by its channel group's coverage, and unboosted.
[shader("compute")]
[numthreads(8, 8, 1)]
void zdefocus_finish(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = params.channels;
    const uint acc = params.accOffset + (tid.y * params.outWidth + tid.x) * (n + groupCount());
    const uint dst = params.dstOffset + tid.y * params.dstStride + tid.x * n;
    for (uint c = 0; c < n && c < 4; ++c) {
        const float coverage = workBuf[acc + n + groupOf(c)];
        const float v = (coverage > 0.0) ? workBuf[acc + c] / coverage : 0.0;
        outBuf[dst + c] = unboost(v);
    }
}

// focalPlaneWorker(): the source halved, its colour channels' halves lit red in front of the
// focus, green in it and blue behind it; the alpha, or a lone channel, kept.
[shader("compute")]
[numthreads(8, 8, 1)]
void zdefocus_focal(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = params.channels;
    const uint dst = params.dstOffset + tid.y * params.dstStride + tid.x * n;
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    const float coc = circleOfConfusion(rawDepth(x, y));
    const uint lit = (abs(coc) < 1.0) ? 1u : ((coc < 0.0) ? 0u : 2u);
    for (uint c = 0; c < n && c < 4; ++c) {
        const uint bit = (n == 1u) ? 3u : c;
        const float v = sourceValue(x, y, c);
        outBuf[dst + c] = (bit > 2u) ? v : 0.5 * v + 0.5 * ((bit == lit) ? 1.0 : 0.0);
    }
}
