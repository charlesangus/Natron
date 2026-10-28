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
// The kernel's runs, four uints each: dy, dx0, length and the weight's bits.
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
};
[[vk::push_constant]] ConstantBuffer<DefocusParams> params;

// Mirrored in Engine/Nodes/Filter/Defocus.cpp.
static const uint kBlock = 256;
static const uint kBoost = 1;

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

// gatherWorker() and unboostWorker(): every output pixel, 0 outside the domain.
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
    for (uint r = 0; r < params.runCount; ++r) {
        const int dy = int(runBuf[4 * r]);
        const int dx0 = int(runBuf[4 * r + 1]);
        const int length = int(runBuf[4 * r + 2]);
        const float weight = asfloat(runBuf[4 * r + 3]);
        const int sy = y - dy;
        if (sy < 0 || sy >= h)
            continue;
        // Pixel x sums the source over [x - dx0 - length + 1, x - dx0 + 1).
        const int a = clamp(x - dx0 - length + 1, 0, w);
        const int b = clamp(x - dx0 + 1, 0, w);
        if (a >= b)
            continue;
        for (uint c = 0; c < n && c < 4; ++c)
            acc[c] += weight * rangeSum(c, uint(sy), uint(a), uint(b));
    }
    const bool boost = (params.flags & kBoost) != 0u;
    for (uint c = 0; c < n && c < 4; ++c) {
        const float v = acc[c];
        outBuf[dst + c] = (boost && v > params.threshold) ? params.threshold + (v - params.threshold) / params.gain : v;
    }
}
