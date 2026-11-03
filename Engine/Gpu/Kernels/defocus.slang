// Defocus's passes, after the workers of Engine/Nodes/Filter/Defocus.cpp and the gather of
// Engine/Nodes/Filter/BokehKernel.cpp. The work buffer covers the CPU's scratch rect, the domain
// of domWidth x domHeight pixels from (domX1, domY1): the boosted source, `channels` floats per
// pixel at 0; from localOffset, per channel and row, domWidth + 1 block prefixes; from
// totalOffset, per channel and row, nBlocks block totals.
//
// The CPU sums each row in a double prefix table and differences its entries, which a float
// table over a whole row cannot match: the sums would grow with the row. Here a row is cut into
// blocks of kBlock pixels, each summed from its own start, so that a run's sum is a difference of
// two prefixes within a block, or the end of one block, the totals of the blocks in between and
// the start of another. Samples that are not finite read as 0, as on the CPU.
//
// DefocusGpuParams in Engine/Nodes/Filter/Defocus.cpp mirrors DefocusParams.

// load: the source image.
StructuredBuffer<uint> srcBuf;
// The kernel's runs, four uints each: dy, dx0, length and the weight's bits. With kLens, the
// runs of every channel group's kernel in every cell of the output, the cell table from
// cellTable, two uints per kernel, by group, cell row and cell column: the index of its first run's
// first uint and its run count; and from columnMap and rowMap, the cell column of each output
// column and the cell row of each output row, when there are more than one.
StructuredBuffer<uint> runBuf;
RWStructuredBuffer<float> workBuf;
// gather: outWidth x outHeight pixels at (outX1, outY1), the first at dstOffset.
RWStructuredBuffer<float> outBuf;

struct DefocusParams
{
    int domX1;
    int domY1;
    uint domWidth;
    uint domHeight;
    uint channels;
    uint nBlocks;
    uint localOffset;
    uint totalOffset;
    uint runCount;
    // kBoost while the highlight boost is on.
    uint flags;
    float threshold;
    float gain;
    // load: the source pixels that exist, the first at srcOffset; empty when there are none.
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
    uint cellTable;
    uint columnMap;
    uint rowMap;
    uint cellColumns;
    uint cellRows;
    // With kLens, two bits per channel, its group; from bit 8, the number of groups.
    uint channelGroups;
};
[[vk::push_constant]] ConstantBuffer<DefocusParams> params;

// Mirrored in Engine/Nodes/Filter/Defocus.cpp.
static const uint kBlock = 256;
static const uint kBoost = 1;
static const uint kLens = 2;

bool isFiniteValue(float v)
{
    return !isnan(v) && !isinf(v);
}

uint localIndex(uint c, uint y, uint x)
{
    return params.localOffset + (c * params.domHeight + y) * (params.domWidth + 1) + x;
}

uint totalIndex(uint c, uint y, uint k)
{
    return params.totalOffset + (c * params.domHeight + y) * params.nBlocks + k;
}

// boostWorker(): the source with its highlights boosted, 0 outside the source.
[shader("compute")]
[numthreads(8, 8, 1)]
void defocus_load(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.domWidth || tid.y >= params.domHeight)
        return;
    const uint n = params.channels;
    const uint dst = (tid.y * params.domWidth + tid.x) * n;
    const int x = params.domX1 + int(tid.x);
    const int y = params.domY1 + int(tid.y);
    if (x < params.srcX1 || x >= params.srcX2 || y < params.srcY1 || y >= params.srcY2) {
        for (uint c = 0; c < n && c < 4; ++c)
            workBuf[dst + c] = 0.0;
        return;
    }
    const uint src = params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * n;
    const bool boost = (params.flags & kBoost) != 0u;
    for (uint c = 0; c < n && c < 4; ++c) {
        const float v = asfloat(srcBuf[src + c]);
        workBuf[dst + c] = (boost && v > params.threshold) ? params.threshold + (v - params.threshold) * params.gain : v;
    }
}

// Block tid.x of row tid.y: each pixel's prefix from the block's start, and the block's total.
// The last block also writes the prefix at domWidth, 0 when it starts a block of its own.
[shader("compute")]
[numthreads(64, 1, 1)]
void defocus_blocks(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.nBlocks || tid.y >= params.domHeight)
        return;
    const uint n = params.channels;
    const uint y = tid.y;
    const uint x0 = tid.x * kBlock;
    const uint x1 = min(x0 + kBlock, params.domWidth);
    const bool last = (tid.x == params.nBlocks - 1);
    for (uint c = 0; c < n && c < 4; ++c) {
        float sum = 0.0;
        for (uint x = x0; x < x1; ++x) {
            workBuf[localIndex(c, y, x)] = sum;
            const float v = workBuf[(y * params.domWidth + x) * n + c];
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

// Adds to acc, for the channels of mask, the runs of count from runBuf[first] gathered at domain
// pixel (x, y).
void gatherRuns(uint first, uint count, uint mask, int x, int y, inout float acc[4])
{
    const uint n = params.channels;
    const int w = int(params.domWidth);
    const int h = int(params.domHeight);
    for (uint r = 0; r < count; ++r) {
        const uint run = first + 4 * r;
        const int dy = int(runBuf[run]);
        const int dx0 = int(runBuf[run + 1]);
        const int length = int(runBuf[run + 2]);
        const float weight = asfloat(runBuf[run + 3]);
        const int sy = y - dy;
        if (sy < 0 || sy >= h)
            continue;
        // Pixel x sums the source over [x - dx0 - length + 1, x - dx0 + 1).
        const int a = clamp(x - dx0 - length + 1, 0, w);
        const int b = clamp(x - dx0 + 1, 0, w);
        if (a >= b)
            continue;
        for (uint c = 0; c < n && c < 4; ++c) {
            if ((mask & (1u << c)) != 0u)
                acc[c] += weight * rangeSum(c, uint(sy), uint(a), uint(b));
        }
    }
}

// gatherWorker() and lensGatherWorker(), and unboostWorker(): every output pixel, 0 outside the
// domain; with kLens each channel gathers with its group's kernel in the output pixel's cell.
[shader("compute")]
[numthreads(8, 8, 1)]
void defocus_gather(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = params.channels;
    const uint dst = params.dstOffset + tid.y * params.dstStride + tid.x * n;
    const int x = params.outX1 + int(tid.x) - params.domX1;
    const int y = params.outY1 + int(tid.y) - params.domY1;
    const int w = int(params.domWidth);
    const int h = int(params.domHeight);
    if (x < 0 || y < 0 || x >= w || y >= h) {
        for (uint c = 0; c < n && c < 4; ++c)
            outBuf[dst + c] = 0.0;
        return;
    }
    float acc[4] = { 0.0, 0.0, 0.0, 0.0 };
    if ((params.flags & kLens) != 0u) {
        const uint col = (params.cellColumns > 1u) ? runBuf[params.columnMap + tid.x] : 0u;
        const uint row = (params.cellRows > 1u) ? runBuf[params.rowMap + tid.y] : 0u;
        const uint groups = params.channelGroups >> 8;
        for (uint g = 0; g < groups; ++g) {
            uint mask = 0u;
            for (uint c = 0; c < n && c < 4; ++c) {
                if (((params.channelGroups >> (2 * c)) & 3u) == g)
                    mask |= 1u << c;
            }
            if (mask == 0u)
                continue;
            const uint entry = params.cellTable + 2 * ((g * params.cellRows + row) * params.cellColumns + col);
            gatherRuns(runBuf[entry], runBuf[entry + 1], mask, x, y, acc);
        }
    } else {
        gatherRuns(0, params.runCount, 15u, x, y, acc);
    }
    const bool boost = (params.flags & kBoost) != 0u;
    for (uint c = 0; c < n && c < 4; ++c) {
        const float v = acc[c];
        outBuf[dst + c] = (boost && v > params.threshold) ? params.threshold + (v - params.threshold) / params.gain : v;
    }
}
