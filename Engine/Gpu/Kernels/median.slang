// The window median of Engine/Nodes/Filter/Median.cpp, bit for bit: the samples are the CPU's
// (NaN and -0 read as +0, coordinates clamped to the domain, zero where no source pixel exists),
// and both kernels select the exact middle value rather than approximating it.

// The source image.
StructuredBuffer<uint> inBuf;
// The result, written over the output rect.
RWStructuredBuffer<uint> outBuf;

// MedianGpuParams in Engine/Nodes/Filter/Median.cpp mirrors it.
struct MedianParams
{
    // The source pixels that exist, the first at srcOffset; empty when there are none.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    // The CPU's buffer: output grown by the radius, clipped to the region of definition.
    int domX1;
    int domY1;
    int domX2;
    int domY2;
    // The rect written, whose first pixel is at dstOffset.
    int outX1;
    int outY1;
    int outX2;
    int outY2;
    uint dstOffset;
    uint dstStride;
    uint channels;
    int radius;
    float threshold;
    uint padding0;
};

[[vk::push_constant]] ConstantBuffer<MedianParams> params;

static const int kDirectMaxRadius = 2;
static const int kDirectMaxCount = (2 * kDirectMaxRadius + 1) * (2 * kDirectMaxRadius + 1);
static const int kGroup = 16;
static const int kTiledMaxRadius = 16;
static const int kMaxTile = (kGroup + 2 * kTiledMaxRadius) * (kGroup + 2 * kTiledMaxRadius);

bool inDomain(int x, int y)
{
    return x >= params.domX1 && x < params.domX2 && y >= params.domY1 && y < params.domY2;
}

// The source value at (x, y) as the CPU's buffer holds it: zero where no source pixel exists.
float sourceAt(int x, int y, uint c)
{
    if (x < params.srcX1 || x >= params.srcX2 || y < params.srcY1 || y >= params.srcY2)
        return 0.0;
    const uint index = params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.channels + c;
    return asfloat(inBuf[index]);
}

// A window sample at (x, y), clamped to the domain: NaN and either zero read as +0.
float windowSample(int x, int y, uint c)
{
    const int cx = clamp(x, params.domX1, params.domX2 - 1);
    const int cy = clamp(y, params.domY1, params.domY2 - 1);
    const uint u = asuint(sourceAt(cx, cy, c));
    // Bit tests, so that a device flushing denormals still keeps them apart from zero.
    const uint magnitude = u & 0x7fffffffu;
    return (magnitude == 0u || magnitude > 0x7f800000u) ? 0.0 : asfloat(u);
}

// Median::applyThreshold.
float applyThreshold(float v, float m)
{
    const float t = params.threshold;
    if (t <= 0.0 || (asuint(v) & 0x7fffffffu) > 0x7f800000u)
        return m;
    precise const float limit = t * max(abs(m), 1e-6f);
    precise const float diff = abs(v - m);
    return diff <= limit ? v : m;
}

// An unsigned key in the order of the float, for a float that is not NaN.
uint orderKey(float f)
{
    const uint u = asuint(f);
    return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u);
}

float keyValue(uint k)
{
    return asfloat((k & 0x80000000u) != 0u ? (k & 0x7fffffffu) : ~k);
}

uint outputIndex(int x, int y, uint c)
{
    return params.dstOffset + uint(y - params.outY1) * params.dstStride + uint(x - params.outX1) * params.channels + c;
}

// Radius 1 and 2: each thread holds the order keys of its window and selects the middle by
// partial selection sort; integer keys keep denormals ordered on a device that flushes them.
[shader("compute")]
[numthreads(8, 8, 1)]
void median_direct(uint3 tid : SV_DispatchThreadID)
{
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    if (x >= params.outX2 || y >= params.outY2)
        return;
    const uint nc = params.channels;
    if (!inDomain(x, y))
    {
        for (uint c = 0; c < nc; ++c)
            outBuf[outputIndex(x, y, c)] = 0u;
        return;
    }
    const int r = params.radius;
    const int count = (2 * r + 1) * (2 * r + 1);
    const int mid = count / 2;
    uint v[kDirectMaxCount];
    for (uint c = 0; c < nc; ++c)
    {
        int k = 0;
        for (int dy = -r; dy <= r; ++dy)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                v[k] = orderKey(windowSample(x + dx, y + dy, c));
                ++k;
            }
        }
        for (int i = 0; i <= mid; ++i)
        {
            int best = i;
            for (int j = i + 1; j < count; ++j)
            {
                if (v[j] < v[best])
                    best = j;
            }
            const uint t = v[i];
            v[i] = v[best];
            v[best] = t;
        }
        outBuf[outputIndex(x, y, c)] = asuint(applyThreshold(sourceAt(x, y, c), keyValue(v[mid])));
    }
}

groupshared uint tile[kMaxTile];

// Radius 3 to kTiledMaxRadius: a group of kGroup x kGroup pixels loads the order keys of its
// window, one channel at a time, and each thread finds its middle key bit by bit, from the top:
// the k-th smallest key is the largest value with at most k keys below it.
[shader("compute")]
[numthreads(16, 16, 1)]
void median_tiled(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    const int r = params.radius;
    const int side = 2 * r + 1;
    const int tileSide = kGroup + 2 * r;
    const int ox = params.outX1 + int(gid.x) * kGroup;
    const int oy = params.outY1 + int(gid.y) * kGroup;
    const int x = ox + int(lid.x);
    const int y = oy + int(lid.y);
    const bool active = x < params.outX2 && y < params.outY2;
    const bool inside = active && inDomain(x, y);
    const uint mid = uint(side * side / 2);
    const uint nc = params.channels;
    const int lane = int(lid.y) * kGroup + int(lid.x);

    for (uint c = 0; c < nc; ++c)
    {
        GroupMemoryBarrierWithGroupSync();
        for (int i = lane; i < tileSide * tileSide; i += kGroup * kGroup)
        {
            const int tx = i % tileSide;
            const int ty = i / tileSide;
            tile[i] = orderKey(windowSample(ox - r + tx, oy - r + ty, c));
        }
        GroupMemoryBarrierWithGroupSync();
        if (!active)
            continue;
        if (!inside)
        {
            outBuf[outputIndex(x, y, c)] = 0u;
            continue;
        }
        uint result = 0u;
        for (int bit = 31; bit >= 0; --bit)
        {
            const uint candidate = result | (1u << uint(bit));
            uint below = 0u;
            for (int dy = 0; dy < side; ++dy)
            {
                const int rowStart = (int(lid.y) + dy) * tileSide + int(lid.x);
                for (int dx = 0; dx < side; ++dx)
                    below += tile[rowStart + dx] < candidate ? 1u : 0u;
            }
            if (below <= mid)
                result = candidate;
        }
        outBuf[outputIndex(x, y, c)] = asuint(applyThreshold(sourceAt(x, y, c), keyValue(result)));
    }
}
