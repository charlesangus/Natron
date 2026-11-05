/*
 * The CPU's poissonHealMultigrid() in Engine/Nodes/Filter/EdgeAware/PoissonHeal.cpp, a solver of
 * the equation darktable's dt_heal() (DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/common/heal.c, Copyright (C) 2009-2026 darktable developers, distributed under the GNU GPL
 * version 3 or later) relaxes: conjugate gradients preconditioned by a multigrid V-cycle over the
 * hierarchy buildHealGrid() makes on the host. Only sums run in another order than the CPU's, in
 * float rather than double.
 *
 * Images are 4 interleaved channels, tightly packed, top-down. Vectors hold 4 floats a cell of
 * the hierarchy. Every kernel after heal_scalars' first returns at once once the solve has
 * stopped, so that the host can record more cycles than the solve takes.
 */

RWStructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> destBuf;
// One float a pixel: masked where finite and not 0.
RWStructuredBuffer<float> maskBuf;
// Per cell: the diagonal, its inverse, the coupling to the east cell, to the south cell.
RWStructuredBuffer<float> coefBuf;
// The active cells of each level by their index in it: all, colour 0, colour 1.
RWStructuredBuffer<uint> cellBuf;
// Per level, kLevelWords words: see the kLevel constants.
RWStructuredBuffer<uint> levelBuf;
// The finest level's solution and search direction.
RWStructuredBuffer<float> uBuf;
RWStructuredBuffer<float> pBuf;
// Every level's right-hand side, correction and scratch; on the finest level the residual r, the
// preconditioned residual z and A p. heal_tail alone reads, within a dispatch, what other invocations
// of its group wrote, and a barrier reaches such writes only on Coherent memory: without it RADV
// reads through a cache its other waves do not update, and the result changes from run to run. The
// other kernels read across dispatch barriers, so they keep the cache.
#ifdef HEAL_READS_WITHIN_DISPATCH
globallycoherent RWStructuredBuffer<float> bBuf;
globallycoherent RWStructuredBuffer<float> eBuf;
globallycoherent RWStructuredBuffer<float> tBuf;
#else
RWStructuredBuffer<float> bBuf;
RWStructuredBuffer<float> eBuf;
RWStructuredBuffer<float> tBuf;
#endif
// Each group's sums, 8 floats a group.
RWStructuredBuffer<float> partialBuf;
// The solve's scalars: see the kState constants.
RWStructuredBuffer<float> stateBuf;

// GpuHealParams in Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.cpp mirrors it.
struct HealParams
{
    // The image, in pixels.
    uint width;
    uint height;
    uint level;
    // 0 all the level's active cells, 1 those of colour 0, 2 those of colour 1.
    uint list;
    uint mode;
    // The groups of the dispatch that wrote partialBuf, or of this one.
    uint groups;
    uint maxCycles;
    uint levels;
    // The floats heal_clear zeroes in each of b, e and t.
    uint count;
};
[[vk::push_constant]] ConstantBuffer<HealParams> params;

static const uint kGroup = 256;
// A list dispatch covers kRow items along x and rows of them along y.
static const uint kRow = 65536;

static const uint kLevelWords = 10;
static const uint kLevelCellOffset = 0;
static const uint kLevelStride = 1;
static const uint kLevelListOffset = 4;
static const uint kLevelListCount = 7;

static const uint kStateRz = 0;
static const uint kStateAlpha = 4;
static const uint kStateBeta = 8;
static const uint kStateTarget = 12;
static const uint kStateDone = 16;
static const uint kStateStop = 20;
static const uint kStateCycles = 21;

static const uint kScalarsInit = 0;
static const uint kScalarsStart = 1;
static const uint kScalarsAlpha = 2;
static const uint kScalarsCheck = 3;
static const uint kScalarsBeta = 4;

static const uint kDotRz = 0;
static const uint kDotStart = 1;

static const float kTolerance = 1e-5f;
static const uint kSmoothPairs = 2;
static const float kCoarseWeight = 1.8f;
static const uint kCoarsestSweeps = 16;

groupshared float4 sumsA[kGroup];
groupshared float4 sumsB[kGroup];

bool isFiniteBits(float v)
{
    return (asuint(v) & 0x7f800000u) != 0x7f800000u;
}

bool masked(uint k)
{
    const float m = maskBuf[k];
    return isFiniteBits(m) && m != 0.0f;
}

bool stopped()
{
    return stateBuf[kStateStop] != 0.0f;
}

struct Level
{
    uint offset;
    uint stride;
};

Level levelAt(uint l)
{
    Level v;
    v.offset = levelBuf[l * kLevelWords + kLevelCellOffset];
    v.stride = levelBuf[l * kLevelWords + kLevelStride];
    return v;
}

uint listOffset(uint l, uint list)
{
    return levelBuf[l * kLevelWords + kLevelListOffset + list];
}

uint listCount(uint l, uint list)
{
    return levelBuf[l * kLevelWords + kLevelListCount + list];
}

// Macros because Slang rejects a globallycoherent buffer as a function argument.
#define load4(buf, g) float4(buf[4 * (g)], buf[4 * (g) + 1], buf[4 * (g) + 2], buf[4 * (g) + 3])

#define store4(buf, g, v)            \
    do {                             \
        const float4 stored = (v);   \
        buf[4 * (g)] = stored.x;     \
        buf[4 * (g) + 1] = stored.y; \
        buf[4 * (g) + 2] = stored.z; \
        buf[4 * (g) + 3] = stored.w; \
    } while (false)

uint itemIndex(uint3 tid)
{
    return tid.y * kRow + tid.x;
}

uint groupIndex(uint3 gid)
{
    return gid.y * (kRow / kGroup) + gid.x;
}

// Sums sumsA[] and sumsB[] into their first entries; every thread of the group must call it.
void reduceGroup(uint index)
{
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kGroup / 2; stride > 0; stride >>= 1)
    {
        if (index < stride)
        {
            precise float4 a = sumsA[index] + sumsA[index + stride];
            precise float4 b = sumsB[index] + sumsB[index + stride];
            sumsA[index] = a;
            sumsB[index] = b;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

void writePartials(uint index, uint group, float4 a, float4 b)
{
    sumsA[index] = a;
    sumsB[index] = b;
    reduceGroup(index);
    if (index == 0 && group < params.groups)
    {
        store4(partialBuf, 2 * group, sumsA[0]);
        store4(partialBuf, 2 * group + 1, sumsB[0]);
    }
}

float4 applyStencil(uint s, uint g, float4 west, float4 east, float4 north, float4 south, float4 centre)
{
    const float cw = coefBuf[4 * (g - 1) + 2];
    const float ce = coefBuf[4 * g + 2];
    const float cn = coefBuf[4 * (g - s) + 3];
    const float cs = coefBuf[4 * g + 3];
    const float d = coefBuf[4 * g];
    precise float4 around = cw * west + ce * east + cn * north + cs * south;
    precise float4 out = d * centre - around;
    return out;
}

#define applyAt(buf, s, g) applyStencil(s, g, load4(buf, (g) - 1), load4(buf, (g) + 1), load4(buf, (g) - (s)), load4(buf, (g) + (s)), load4(buf, g))

void relaxAt(Level level, uint g)
{
    const uint s = level.stride;
    const float cw = coefBuf[4 * (g - 1) + 2];
    const float ce = coefBuf[4 * g + 2];
    const float cn = coefBuf[4 * (g - s) + 3];
    const float cs = coefBuf[4 * g + 3];
    const float inv = coefBuf[4 * g + 1];
    precise float4 v = (load4(bBuf, g) + cw * load4(eBuf, g - 1) + ce * load4(eBuf, g + 1) + cn * load4(eBuf, g - s) + cs * load4(eBuf, g + s)) * inv;
    store4(eBuf, g, v);
}

uint colourOf(uint k, uint stride)
{
    return ((k % stride) + (k / stride)) & 1;
}

// With e = 0 a colour-0 cell has no neighbour terms.
void relaxFromZeroAt(Level level, uint k)
{
    const uint g = level.offset + k;
    if (colourOf(k, level.stride) == 0)
    {
        precise float4 v = load4(bBuf, g) * coefBuf[4 * g + 1];
        store4(eBuf, g, v);
    }
    else
    {
        store4(eBuf, g, float4(0.0f));
    }
}

// Colour 1 was swept last, so its residual is 0 and only colour 0 needs A e.
void residualAt(Level level, uint k)
{
    const uint g = level.offset + k;
    if (colourOf(k, level.stride) != 0)
    {
        store4(tBuf, g, float4(0.0f));
        return;
    }
    precise float4 v = load4(bBuf, g) - applyAt(eBuf, level.stride, g);
    store4(tBuf, g, v);
}

void restrictAt(Level fine, Level coarse, uint kc)
{
    const uint x = kc % coarse.stride - 1;
    const uint y = kc / coarse.stride - 1;
    const uint k00 = fine.offset + (2 * y + 1) * fine.stride + 2 * x + 1;
    const uint k10 = k00 + fine.stride;
    precise float4 v = load4(tBuf, k00) + load4(tBuf, k00 + 1) + load4(tBuf, k10) + load4(tBuf, k10 + 1);
    store4(bBuf, coarse.offset + kc, v);
}

void prolongAt(Level fine, Level coarse, uint k)
{
    const uint x = k % fine.stride - 1;
    const uint y = k / fine.stride - 1;
    const uint gc = coarse.offset + (y / 2 + 1) * coarse.stride + x / 2 + 1;
    const uint g = fine.offset + k;
    precise float4 v = load4(eBuf, g) + kCoarseWeight * load4(eBuf, gc);
    store4(eBuf, g, v);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_clear(uint3 tid : SV_DispatchThreadID)
{
    const uint i = itemIndex(tid);
    if (i >= params.count)
        return;
    bBuf[i] = 0.0f;
    eBuf[i] = 0.0f;
    tBuf[i] = 0.0f;
    if (i < 4 * (params.width + 2) * (params.height + 2))
    {
        uBuf[i] = 0.0f;
        pBuf[i] = 0.0f;
    }
}

[shader("compute")]
[numthreads(16, 16, 1)]
void heal_setup(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint k = tid.y * params.width + tid.x;
    const uint g = (tid.y + 1) * (params.width + 2) + tid.x + 1;
    for (uint c = 0; c < 4; ++c)
    {
        const float d = destBuf[4 * k + c];
        const float s = srcBuf[4 * k + c];
        precise float diff = d - s;
        uBuf[4 * g + c] = (isFiniteBits(d) && isFiniteBits(s)) ? diff : 0.0f;
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_initial(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint index : SV_GroupIndex)
{
    const uint i = itemIndex(tid);
    float4 boundary2 = float4(0.0f);
    float4 r2 = float4(0.0f);
    if (i < listCount(0, 0))
    {
        const Level level = levelAt(0);
        const uint s = level.stride;
        const uint g = cellBuf[listOffset(0, 0) + i];
        const uint nbr[4] = { g - 1, g + 1, g - s, g + s };
        // A neighbour is active where its coupling to this active cell is.
        const float coupling[4] = { coefBuf[4 * (g - 1) + 2], coefBuf[4 * g + 2], coefBuf[4 * (g - s) + 3], coefBuf[4 * g + 3] };
        precise float4 sum = float4(0.0f);
        precise float4 boundary = float4(0.0f);
        for (uint m = 0; m < 4; ++m)
        {
            const float4 v = load4(uBuf, nbr[m]);
            sum = sum + v;
            boundary = boundary + ((coupling[m] != 0.0f) ? float4(0.0f) : v);
        }
        precise float4 r = sum - coefBuf[4 * g] * load4(uBuf, g);
        store4(bBuf, g, r);
        precise float4 b2 = boundary * boundary;
        precise float4 rr = r * r;
        boundary2 = b2;
        r2 = rr;
    }
    writePartials(index, groupIndex(gid), boundary2, r2);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_dot(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint index : SV_GroupIndex)
{
    if (stopped())
        return;
    const uint i = itemIndex(tid);
    float4 rz = float4(0.0f);
    if (i < listCount(0, 0))
    {
        const uint g = cellBuf[listOffset(0, 0) + i];
        const float4 z = load4(eBuf, g);
        if (params.mode == kDotStart)
            store4(pBuf, g, z);
        precise float4 v = load4(bBuf, g) * z;
        rz = v;
    }
    writePartials(index, groupIndex(gid), rz, float4(0.0f));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_apply(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint index : SV_GroupIndex)
{
    if (stopped())
        return;
    const uint i = itemIndex(tid);
    float4 pq = float4(0.0f);
    if (i < listCount(0, 0))
    {
        const uint g = cellBuf[listOffset(0, 0) + i];
        const uint s = levelAt(0).stride;
        const float4 q = applyAt(pBuf, s, g);
        store4(tBuf, g, q);
        precise float4 v = load4(pBuf, g) * q;
        pq = v;
    }
    writePartials(index, groupIndex(gid), pq, float4(0.0f));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_update(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint index : SV_GroupIndex)
{
    if (stopped())
        return;
    const uint i = itemIndex(tid);
    float4 r2 = float4(0.0f);
    if (i < listCount(0, 0))
    {
        const uint g = cellBuf[listOffset(0, 0) + i];
        const float4 alpha = load4(stateBuf, kStateAlpha / 4);
        precise float4 u = load4(uBuf, g) + alpha * load4(pBuf, g);
        precise float4 r = load4(bBuf, g) - alpha * load4(tBuf, g);
        store4(uBuf, g, u);
        store4(bBuf, g, r);
        precise float4 rr = r * r;
        r2 = rr;
    }
    writePartials(index, groupIndex(gid), r2, float4(0.0f));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_direction(uint3 tid : SV_DispatchThreadID)
{
    if (stopped())
        return;
    const uint i = itemIndex(tid);
    if (i >= listCount(0, 0))
        return;
    const uint g = cellBuf[listOffset(0, 0) + i];
    const float4 beta = load4(stateBuf, kStateBeta / 4);
    precise float4 p = load4(eBuf, g) + beta * load4(pBuf, g);
    store4(pBuf, g, p);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_scalars(uint3 tid : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (params.mode != kScalarsInit && stopped())
        return;
    precise float4 a = float4(0.0f);
    precise float4 b = float4(0.0f);
    for (uint i = index; i < params.groups; i += kGroup)
    {
        a = a + load4(partialBuf, 2 * i);
        b = b + load4(partialBuf, 2 * i + 1);
    }
    sumsA[index] = a;
    sumsB[index] = b;
    reduceGroup(index);
    if (index != 0)
        return;
    const float4 sumA = sumsA[0];
    const float4 sumB = sumsB[0];
    float4 done = load4(stateBuf, kStateDone / 4);
    const float4 rz = load4(stateBuf, kStateRz / 4);
    if (params.mode == kScalarsInit)
    {
        // sumA holds the squared boundary terms, sumB r^2.
        precise float4 target = (kTolerance * kTolerance) * max(sumA, sumB);
        store4(stateBuf, kStateTarget / 4, target);
        for (uint c = 0; c < 4; ++c)
            done[c] = (sumB[c] <= target[c]) ? 1.0f : 0.0f;
        store4(stateBuf, kStateRz / 4, float4(0.0f));
        store4(stateBuf, kStateAlpha / 4, float4(0.0f));
        store4(stateBuf, kStateBeta / 4, float4(0.0f));
        stateBuf[kStateCycles] = 0.0f;
    }
    else if (params.mode == kScalarsStart)
    {
        store4(stateBuf, kStateRz / 4, sumA);
        return;
    }
    else if (params.mode == kScalarsAlpha)
    {
        float4 alpha;
        for (uint c = 0; c < 4; ++c)
        {
            const bool usable = done[c] == 0.0f && sumA[c] > 0.0f && rz[c] > 0.0f;
            alpha[c] = usable ? rz[c] / sumA[c] : 0.0f;
            if (!usable)
                done[c] = 1.0f;
        }
        store4(stateBuf, kStateAlpha / 4, alpha);
        store4(stateBuf, kStateDone / 4, done);
        return;
    }
    else if (params.mode == kScalarsCheck)
    {
        const float4 target = load4(stateBuf, kStateTarget / 4);
        for (uint c = 0; c < 4; ++c)
        {
            if (sumA[c] <= target[c])
                done[c] = 1.0f;
        }
        const float cycles = stateBuf[kStateCycles] + 1.0f;
        stateBuf[kStateCycles] = cycles;
        if (cycles >= float(params.maxCycles))
            stateBuf[kStateStop] = 1.0f;
    }
    else if (params.mode == kScalarsBeta)
    {
        float4 beta;
        for (uint c = 0; c < 4; ++c)
            beta[c] = (done[c] == 0.0f && rz[c] > 0.0f) ? sumA[c] / rz[c] : 0.0f;
        store4(stateBuf, kStateBeta / 4, beta);
        store4(stateBuf, kStateRz / 4, sumA);
        return;
    }
    store4(stateBuf, kStateDone / 4, done);
    const bool allDone = done.x != 0.0f && done.y != 0.0f && done.z != 0.0f && done.w != 0.0f;
    if (params.mode == kScalarsInit)
        stateBuf[kStateStop] = (allDone || params.maxCycles == 0) ? 1.0f : 0.0f;
    else if (allDone)
        stateBuf[kStateStop] = 1.0f;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_relax(uint3 tid : SV_DispatchThreadID)
{
    if (stopped())
        return;
    const uint i = itemIndex(tid);
    const uint list = (params.mode == 1) ? 0 : params.list;
    if (i >= listCount(params.level, list))
        return;
    const Level level = levelAt(params.level);
    const uint k = cellBuf[listOffset(params.level, list) + i];
    if (params.mode == 1)
        relaxFromZeroAt(level, k);
    else
        relaxAt(level, level.offset + k);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_residual(uint3 tid : SV_DispatchThreadID)
{
    if (stopped())
        return;
    const uint i = itemIndex(tid);
    if (i >= listCount(params.level, 0))
        return;
    residualAt(levelAt(params.level), cellBuf[listOffset(params.level, 0) + i]);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_restrict(uint3 tid : SV_DispatchThreadID)
{
    if (stopped())
        return;
    const uint i = itemIndex(tid);
    const uint l = params.level + 1;
    if (i >= listCount(l, 0))
        return;
    restrictAt(levelAt(params.level), levelAt(l), cellBuf[listOffset(l, 0) + i]);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void heal_prolong(uint3 tid : SV_DispatchThreadID)
{
    if (stopped())
        return;
    const uint i = itemIndex(tid);
    if (i >= listCount(params.level, 0))
        return;
    prolongAt(levelAt(params.level), levelAt(params.level + 1), cellBuf[listOffset(params.level, 0) + i]);
}

// The tail kernel's steps: each runs over a list of one level split among the group's threads,
// and ends at a barrier that makes its writes visible to the next.
void tailRelax(uint l, uint list, uint index)
{
    const Level level = levelAt(l);
    const uint offset = listOffset(l, list);
    const uint count = listCount(l, list);
    for (uint i = index; i < count; i += kGroup)
        relaxAt(level, level.offset + cellBuf[offset + i]);
    AllMemoryBarrierWithGroupSync();
}

void tailRelaxFromZero(uint l, uint index)
{
    const Level level = levelAt(l);
    const uint offset = listOffset(l, 0);
    const uint count = listCount(l, 0);
    for (uint i = index; i < count; i += kGroup)
        relaxFromZeroAt(level, cellBuf[offset + i]);
    AllMemoryBarrierWithGroupSync();
}

// The V-cycle from level params.level down, in one group: the levels it covers are small.
[shader("compute")]
[numthreads(256, 1, 1)]
void heal_tail(uint3 tid : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (stopped())
        return;
    const uint coarsest = params.levels - 1;
    for (uint l = params.level; l < coarsest; ++l)
    {
        tailRelaxFromZero(l, index);
        tailRelax(l, 2, index);
        for (uint i = 1; i < kSmoothPairs; ++i)
        {
            tailRelax(l, 1, index);
            tailRelax(l, 2, index);
        }
        const Level level = levelAt(l);
        for (uint i = index; i < listCount(l, 0); i += kGroup)
            residualAt(level, cellBuf[listOffset(l, 0) + i]);
        AllMemoryBarrierWithGroupSync();
        const Level coarse = levelAt(l + 1);
        for (uint i = index; i < listCount(l + 1, 0); i += kGroup)
            restrictAt(level, coarse, cellBuf[listOffset(l + 1, 0) + i]);
        AllMemoryBarrierWithGroupSync();
    }
    // An odd palindrome of sweeps, so that the cycle stays symmetric.
    tailRelaxFromZero(coarsest, index);
    tailRelax(coarsest, 2, index);
    for (uint i = 1; i < kCoarsestSweeps; ++i)
    {
        tailRelax(coarsest, 1, index);
        tailRelax(coarsest, 2, index);
    }
    tailRelax(coarsest, 1, index);
    for (uint l = coarsest; l > params.level; --l)
    {
        const Level level = levelAt(l - 1);
        const Level coarse = levelAt(l);
        for (uint i = index; i < listCount(l - 1, 0); i += kGroup)
            prolongAt(level, coarse, cellBuf[listOffset(l - 1, 0) + i]);
        AllMemoryBarrierWithGroupSync();
        for (uint i = 0; i < kSmoothPairs; ++i)
        {
            tailRelax(l - 1, 2, index);
            tailRelax(l - 1, 1, index);
        }
    }
}

[shader("compute")]
[numthreads(16, 16, 1)]
void heal_finish(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint k = tid.y * params.width + tid.x;
    if (!masked(k))
        return;
    const uint g = (tid.y + 1) * (params.width + 2) + tid.x + 1;
    for (uint c = 0; c < 4; ++c)
    {
        const float d = destBuf[4 * k + c];
        const float s = srcBuf[4 * k + c];
        if (isFiniteBits(d) && isFiniteBits(s))
        {
            precise float v = uBuf[4 * g + c] + s;
            destBuf[4 * k + c] = v;
        }
    }
}
