// Separable erosion and dilation matching ErodeDilateKernels::filterLine bit for bit. Values travel
// as raw bits and are ordered as integers, since devices may flush denormals in a float compare
// while the CPU orders them exactly. GpuMorphParams in Engine/Gpu/GpuMorphology.h mirrors
// MorphParams.

StructuredBuffer<uint> srcBuf;
RWStructuredBuffer<uint> dstBuf;
// [0, 4): the bounding box (minX, minY, maxX, maxY) of the mask pixels the CPU's shrinkToMask()
// keeps, reduced by morph_mask_bbox. [4, 8): the rect (x1, y1, x2, y2) the CPU's filter buffer
// covers, which bounds every filtered line.
RWStructuredBuffer<int> windowBuf;

// A pass writes the rect of width x height pixels at (outX, outY) of the destination. The source
// rect [srcX1, srcX2) x [srcY1, srcY2) reads zero outside. morph_mask_bbox reads the mask from the
// source fields, over the output rect, with `channels` its component count; morph_window resolves
// the buffer rect from the box, the source fields then holding the mask's bounds.
struct MorphParams
{
    uint width;
    uint height;
    uint channels;
    uint half;
    uint vertical;
    uint takeMax;
    int outX;
    int outY;
    uint dstOffset;
    uint dstStride;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    int rodX1;
    int rodY1;
    int rodX2;
    int rodY2;
    int haloX;
    int haloY;
    uint maskChannel;
    uint maskInvert;
};
[[vk::push_constant]] ConstantBuffer<MorphParams> params;

// kGroup, kColumns, kDirectMaxRadius and kMaxRadius are mirrored in Engine/Gpu/GpuMorphology.cpp.
static const uint kGroup = 128;
static const uint kColumns = 32;
static const uint kDirectMaxRadius = 16;
static const uint kMaxRadius = 1024;
static const uint kDirectTile = kGroup + 2 * kDirectMaxRadius;
static const uint kWideTile = kGroup + 2 * kMaxRadius;

static const int kIntMax = 0x7fffffff;
static const int kIntMin = int(0x80000000u);
static const uint kPlusInf = 0x7f800000u;
static const uint kMinusInf = 0xff800000u;
static const uint kOne = 0x3f800000u;

bool isNaNBits(uint u)
{
    return (u & 0x7fffffffu) > kPlusInf;
}

// -0 and +0 share key 0, as they compare equal on the CPU.
int orderKey(uint u)
{
    return (u & 0x80000000u) != 0 ? -int(u & 0x7fffffffu) : int(u);
}

// The CPU's reduce(a, b): b only when it is strictly beyond a, so a NaN on either side keeps a.
uint reduce(uint a, uint b)
{
    if (isNaNBits(a) || isNaNBits(b))
        return a;
    const int ka = orderKey(a);
    const int kb = orderKey(b);
    if (params.takeMax != 0)
        return kb > ka ? b : a;
    return kb < ka ? b : a;
}

uint neutral()
{
    return params.takeMax != 0 ? kMinusInf : kPlusInf;
}

struct Extent
{
    int lo;
    int hi;
};

Extent axisExtent()
{
    Extent e;
    e.lo = windowBuf[params.vertical != 0 ? 5 : 4];
    e.hi = windowBuf[params.vertical != 0 ? 7 : 6];
    return e;
}

Extent lineExtent()
{
    Extent e;
    e.lo = windowBuf[params.vertical != 0 ? 4 : 5];
    e.hi = windowBuf[params.vertical != 0 ? 6 : 7];
    return e;
}

int axisOrigin()
{
    return params.vertical != 0 ? params.outY : params.outX;
}

int lineOrigin()
{
    return params.vertical != 0 ? params.outX : params.outY;
}

uint sampleAt(int a, int line, uint c)
{
    const int x = params.vertical != 0 ? line : a;
    const int y = params.vertical != 0 ? a : line;
    if (x < params.srcX1 || x >= params.srcX2 || y < params.srcY1 || y >= params.srcY2)
        return 0u;
    return srcBuf[params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.channels + c];
}

// Index j of the CPU's padded line: halfWidth neutral values, the line, halfWidth neutral values.
uint paddedAt(int j, Extent ax, int line, uint c)
{
    const int a = ax.lo + j - int(params.half);
    if (a < ax.lo || a >= ax.hi)
        return neutral();
    return sampleAt(a, line, c);
}

// A line no longer than halfWidth + 2 values becomes its own extremum, folded from its start.
uint shortLine(Extent ax, int line, uint c)
{
    uint acc = sampleAt(ax.lo, line, c);
    for (int a = ax.lo + 1; a < ax.hi; ++a)
        acc = reduce(acc, sampleAt(a, line, c));
    return acc;
}

bool isShort(Extent ax)
{
    return ax.hi - ax.lo <= int(params.half) + 2;
}

uint outputIndex(uint line, uint pos)
{
    return params.vertical != 0 ? params.dstOffset + pos * params.dstStride + line * params.channels
                                : params.dstOffset + line * params.dstStride + pos * params.channels;
}

bool insideExtents(int a, int line, Extent ax, Extent ln)
{
    return a >= ax.lo && a < ax.hi && line >= ln.lo && line < ln.hi;
}

groupshared uint directTile[kDirectTile * 4];

// The window [s, s + 2 * halfWidth] of the padded line as the CPU combines it: the suffix of the
// block holding s, folded down from that block's end, with the prefix of the block holding the
// window's end, folded up from that block's start. fromTile reads the padded values from
// directTile, whose entry 0 is padded index tileStart.
uint directWindow(int s, Extent ax, int line, uint c, bool fromTile, int tileStart)
{
    const int h = int(params.half);
    const int w = 2 * h + 1;
    const int n = (ax.hi - ax.lo) + 2 * h;
    const int e = s + 2 * h;
    const uint nc = params.channels;
    const int be = min(n - 1, s - s % w + w - 1);
    uint suffix = fromTile ? directTile[uint(be - tileStart) * nc + c] : paddedAt(be, ax, line, c);
    for (int j = be - 1; j >= s; --j)
        suffix = reduce(suffix, fromTile ? directTile[uint(j - tileStart) * nc + c] : paddedAt(j, ax, line, c));
    const int bs = e - e % w;
    uint prefix = fromTile ? directTile[uint(bs - tileStart) * nc + c] : paddedAt(bs, ax, line, c);
    for (int j = bs + 1; j <= e; ++j)
        prefix = reduce(prefix, fromTile ? directTile[uint(j - tileStart) * nc + c] : paddedAt(j, ax, line, c));
    return reduce(suffix, prefix);
}

// Every value of the pass: zero outside the buffer rect, else the filtered value.
void writeDirect(uint line, uint pos, Extent ax, Extent ln, bool fromTile, int tileStart)
{
    const int a = axisOrigin() + int(pos);
    const int l = lineOrigin() + int(line);
    const uint nc = params.channels;
    const uint o = outputIndex(line, pos);
    if (!insideExtents(a, l, ax, ln))
    {
        for (uint c = 0; c < nc; ++c)
            dstBuf[o + c] = 0u;
        return;
    }
    const bool shortLine_ = isShort(ax);
    for (uint c = 0; c < nc; ++c)
        dstBuf[o + c] = shortLine_ ? shortLine(ax, l, c) : directWindow(a - ax.lo, ax, l, c, fromTile, tileStart);
}

// Dispatch shape from morphGroupCount in Engine/Gpu/GpuMorphology.cpp, which sends here only
// halfWidth <= kDirectMaxRadius. group.x is the tile along the axis; group.y is the line, or a
// block of kColumns columns on the vertical pass.
[shader("compute")]
[numthreads(128, 1, 1)]
void morph_direct(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const int len = int(params.vertical != 0 ? params.height : params.width);
    const int lines = int(params.vertical != 0 ? params.width : params.height);
    const int h = int(params.half);
    const uint nc = params.channels;
    const int base = int(gid.x * kGroup);
    const Extent ax = axisExtent();
    const Extent ln = lineExtent();

    if (params.vertical != 0)
    {
        // Threads of a wave take adjacent columns so every load is a contiguous row segment.
        const uint col = gid.y * kColumns + (tid.x % kColumns);
        if (col < params.width)
        {
            for (uint r = tid.x / kColumns; r < kGroup; r += kGroup / kColumns)
            {
                const int rowPos = base + int(r);
                if (rowPos < len)
                    writeDirect(col, uint(rowPos), ax, ln, false, 0);
            }
        }
        return;
    }

    const uint line = gid.y;
    const int l = lineOrigin() + int(line);
    // Padded index of the window start of the group's first output.
    const int tileStart = axisOrigin() + base - ax.lo;
    const int tileLen = int(kGroup) + 2 * h;
    for (int t = int(tid.x); t < tileLen; t += int(kGroup))
    {
        for (uint c = 0; c < nc; ++c)
            directTile[uint(t) * nc + c] = int(line) < lines ? paddedAt(tileStart + t, ax, l, c) : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    const int pos = base + int(tid.x);
    if (pos < len && int(line) < lines)
        writeDirect(line, uint(pos), ax, ln, true, tileStart);
}

groupshared uint wideRaw[kWideTile];
groupshared uint widePrefix[kWideTile];

// van Herk / Gil-Werman over a tile of kGroup + 2 * halfWidth padded values, one channel at a time:
// one thread per block folds the prefix, then the suffix in place over the raw values, each in
// the CPU's order. A window needs the prefix of a block starting at or after the tile start and
// the suffix of a block ending inside the tile, so the blocks the tile cuts are never read where
// they are incomplete.
[shader("compute")]
[numthreads(128, 1, 1)]
void morph_wide(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const int len = int(params.vertical != 0 ? params.height : params.width);
    const int lines = int(params.vertical != 0 ? params.width : params.height);
    const int h = int(params.half);
    const int w = 2 * h + 1;
    const uint nc = params.channels;
    const uint line = gid.y;
    const int base = int(gid.x * kGroup);
    const Extent ax = axisExtent();
    const Extent ln = lineExtent();
    const int l = lineOrigin() + int(line);
    const int n = (ax.hi - ax.lo) + 2 * h;
    const int pos = base + int(tid.x);
    const int a = axisOrigin() + pos;
    const bool active = pos < len && int(line) < lines;
    const bool inside = active && insideExtents(a, l, ax, ln);
    const bool shortLine_ = isShort(ax);

    const int tileStart = axisOrigin() + base - ax.lo;
    const int tileLen = int(kGroup) + 2 * h;
    const int validLo = max(tileStart, 0);
    const int validHi = min(tileStart + tileLen, n);
    const int firstBlock = validLo < validHi ? validLo / w : 0;
    const int nBlocks = validLo < validHi ? (validHi - 1) / w - firstBlock + 1 : 0;
    const bool scan = !shortLine_ && int(line) < lines;

    for (uint c = 0; c < nc; ++c)
    {
        for (int t = int(tid.x); t < tileLen; t += int(kGroup))
        {
            const int j = tileStart + t;
            wideRaw[t] = (scan && j >= 0 && j < n) ? paddedAt(j, ax, l, c) : 0u;
        }
        GroupMemoryBarrierWithGroupSync();
        if (scan && int(tid.x) < nBlocks)
        {
            const int k = firstBlock + int(tid.x);
            const int jLo = max(k * w, validLo);
            const int jHi = min((k + 1) * w, validHi);
            uint acc = wideRaw[jLo - tileStart];
            widePrefix[jLo - tileStart] = acc;
            for (int j = jLo + 1; j < jHi; ++j)
            {
                acc = reduce(acc, wideRaw[j - tileStart]);
                widePrefix[j - tileStart] = acc;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (scan && int(tid.x) < nBlocks)
        {
            const int k = firstBlock + int(tid.x);
            const int jLo = max(k * w, validLo);
            const int jHi = min((k + 1) * w, validHi);
            uint acc = wideRaw[jHi - 1 - tileStart];
            for (int j = jHi - 2; j >= jLo; --j)
            {
                acc = reduce(acc, wideRaw[j - tileStart]);
                wideRaw[j - tileStart] = acc;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (active)
        {
            uint v = 0u;
            if (inside)
            {
                if (shortLine_)
                    v = shortLine(ax, l, c);
                else
                {
                    const int s = a - ax.lo;
                    v = reduce(wideRaw[s - tileStart], widePrefix[s + 2 * h - tileStart]);
                }
            }
            dstBuf[outputIndex(line, uint(pos)) + c] = v;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

groupshared int boxMin[2];
groupshared int boxMax[2];

// Whether shrinkToMask() keeps pixel (x, y): the mask is not zero there, or with an inverted mask
// not one, a pixel outside the mask's bounds reading zero.
bool keptByMask(int x, int y)
{
    const bool inBounds = x >= params.srcX1 && x < params.srcX2 && y >= params.srcY1 && y < params.srcY2;
    uint v = 0u;
    if (inBounds)
        v = srcBuf[params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.channels + params.maskChannel];
    if (params.maskInvert != 0)
        return !inBounds || v != kOne;
    return inBounds && (v & 0x7fffffffu) != 0u;
}

// windowBuf[0, 4) must start as (INT_MAX, INT_MAX, INT_MIN, INT_MIN).
[shader("compute")]
[numthreads(8, 8, 1)]
void morph_mask_bbox(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0)
    {
        boxMin[0] = kIntMax;
        boxMin[1] = kIntMax;
        boxMax[0] = kIntMin;
        boxMax[1] = kIntMin;
    }
    GroupMemoryBarrierWithGroupSync();
    if (dtid.x < params.width && dtid.y < params.height)
    {
        const int x = params.outX + int(dtid.x);
        const int y = params.outY + int(dtid.y);
        if (keptByMask(x, y))
        {
            InterlockedMin(boxMin[0], x);
            InterlockedMin(boxMin[1], y);
            InterlockedMax(boxMax[0], x);
            InterlockedMax(boxMax[1], y);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && boxMin[0] <= boxMax[0])
    {
        InterlockedMin(windowBuf[0], boxMin[0]);
        InterlockedMin(windowBuf[1], boxMin[1]);
        InterlockedMax(windowBuf[2], boxMax[0]);
        InterlockedMax(windowBuf[3], boxMax[1]);
    }
}

// shrinkToMask() over the output rect from the box, including CImg's rule that an inverted mask
// only drops rows when the rect lies strictly inside the mask's bounds horizontally, and columns
// when the kept rows do vertically; then the filter buffer, that window plus the halo clipped to
// the region of definition, or empty.
[shader("compute")]
[numthreads(1, 1, 1)]
void morph_window(uint3 dtid : SV_DispatchThreadID)
{
    const int rx1 = params.outX;
    const int ry1 = params.outY;
    const int rx2 = params.outX + int(params.width);
    const int ry2 = params.outY + int(params.height);
    const bool inv = params.maskInvert != 0;
    const bool any = windowBuf[0] <= windowBuf[2];

    int y1 = 0;
    int y2 = 0;
    if (inv && (rx1 < params.srcX1 || rx2 >= params.srcX2))
    {
        y1 = ry1;
        y2 = ry2;
    }
    else if (any)
    {
        y1 = windowBuf[1];
        y2 = windowBuf[3] + 1;
    }
    int x1 = 0;
    int x2 = 0;
    if (y1 < y2)
    {
        if (inv && (y1 < params.srcY1 || y2 >= params.srcY2))
        {
            x1 = rx1;
            x2 = rx2;
        }
        else if (any)
        {
            x1 = windowBuf[0];
            x2 = windowBuf[2] + 1;
        }
    }
    int bx1 = 0;
    int by1 = 0;
    int bx2 = 0;
    int by2 = 0;
    if (x1 < x2 && y1 < y2)
    {
        bx1 = max(x1 - params.haloX, params.rodX1);
        by1 = max(y1 - params.haloY, params.rodY1);
        bx2 = min(x2 + params.haloX, params.rodX2);
        by2 = min(y2 + params.haloY, params.rodY2);
        if (bx1 >= bx2 || by1 >= by2)
        {
            bx1 = 0;
            by1 = 0;
            bx2 = 0;
            by2 = 0;
        }
    }
    windowBuf[4] = bx1;
    windowBuf[5] = by1;
    windowBuf[6] = bx2;
    windowBuf[7] = by2;
}
