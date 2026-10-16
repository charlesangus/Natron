// Separable erosion and dilation matching ErodeDilateKernels::filterLine bit for bit. Values travel
// as raw bits and are ordered as integers, since devices may flush denormals in a float compare
// while the CPU orders them exactly. GpuMorphParams in Engine/Gpu/GpuMorphology.h mirrors
// MorphParams.

StructuredBuffer<uint> srcBuf;
RWStructuredBuffer<uint> dstBuf;
// The rect (x1, y1, x2, y2) the CPU's filter buffer covers, which bounds every filtered line.
RWStructuredBuffer<int> windowBuf;

// A pass writes the rect of width x height pixels at (outX, outY) of the destination. The source
// rect [srcX1, srcX2) x [srcY1, srcY2) reads zero outside.
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
};
[[vk::push_constant]] ConstantBuffer<MorphParams> params;

// kGroup, kColumns, kChunk and kDirectMaxRadius are mirrored in Engine/Gpu/GpuMorphology.cpp.
static const uint kGroup = 128;
static const uint kColumns = 32;
static const uint kChunk = 16;
static const uint kDirectMaxRadius = 16;
static const uint kDirectTile = kGroup + 2 * kDirectMaxRadius;

static const uint kPlusInf = 0x7f800000u;
static const uint kMinusInf = 0xff800000u;

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
    e.lo = windowBuf[params.vertical != 0 ? 1 : 0];
    e.hi = windowBuf[params.vertical != 0 ? 3 : 2];
    return e;
}

Extent lineExtent()
{
    Extent e;
    e.lo = windowBuf[params.vertical != 0 ? 0 : 1];
    e.hi = windowBuf[params.vertical != 0 ? 2 : 3];
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

// Dispatch shape from dispatchPass in Engine/Gpu/GpuMorphology.cpp, which sends here only
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

uint4 reduce4(uint4 a, uint4 b)
{
    return uint4(reduce(a.x, b.x), reduce(a.y, b.y), reduce(a.z, b.z), reduce(a.w, b.w));
}

// Every channel of padded index j of the line, the channels past params.channels left zero.
uint4 paddedPixel(int j, Extent ax, int line)
{
    const int a = ax.lo + j - int(params.half);
    if (a < ax.lo || a >= ax.hi)
        return uint4(neutral());
    const int x = params.vertical != 0 ? line : a;
    const int y = params.vertical != 0 ? a : line;
    uint4 v = uint4(0u);
    if (x < params.srcX1 || x >= params.srcX2 || y < params.srcY1 || y >= params.srcY2)
        return v;
    const uint idx = params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.channels;
    const uint nc = params.channels;
    v.x = srcBuf[idx];
    if (nc > 1)
        v.y = srcBuf[idx + 1];
    if (nc > 2)
        v.z = srcBuf[idx + 2];
    if (nc > 3)
        v.w = srcBuf[idx + 3];
    return v;
}

// van Herk / Gil-Werman, each thread filtering kChunk consecutive values of one line: it folds
// their suffixes backwards, kept in registers, then their prefixes forwards, each in the CPU's
// order from the end or start of the block, or of the padded line, the value lies in.
void wideLine(uint line, int p0)
{
    const int len = int(params.vertical != 0 ? params.height : params.width);
    const int lines = int(params.vertical != 0 ? params.width : params.height);
    if (int(line) >= lines || p0 >= len)
        return;
    const int h = int(params.half);
    const int w = 2 * h + 1;
    const uint nc = params.channels;
    const Extent ax = axisExtent();
    const Extent ln = lineExtent();
    const int l = lineOrigin() + int(line);
    const int n = (ax.hi - ax.lo) + 2 * h;
    const int s0 = axisOrigin() + p0 - ax.lo;
    const int sLo = max(s0, 0);
    const int sHi = (l >= ln.lo && l < ln.hi) ? min(s0 + min(int(kChunk), len - p0), ax.hi - ax.lo) : sLo;
    const bool shortLine_ = isShort(ax);

    uint4 res[kChunk];
    [ForceUnroll]
    for (int i = 0; i < int(kChunk); ++i)
        res[i] = uint4(0u);

    if (sLo < sHi && shortLine_)
    {
        uint4 v = uint4(0u);
        for (uint c = 0; c < nc; ++c)
            v[c] = shortLine(ax, l, c);
        [ForceUnroll]
        for (int i = 0; i < int(kChunk); ++i)
        {
            if (s0 + i >= sLo && s0 + i < sHi)
                res[i] = v;
        }
    }
    else if (sLo < sHi)
    {
        const int top = min(n - 1, (sHi - 1) - (sHi - 1) % w + w - 1);
        uint4 acc = uint4(0u);
        int r = top % w;
        for (int j = top; j >= sHi; --j)
        {
            const uint4 v = paddedPixel(j, ax, l);
            acc = (j == top || r == w - 1) ? v : reduce4(acc, v);
            r = r == 0 ? w - 1 : r - 1;
        }
        [ForceUnroll]
        for (int i = int(kChunk) - 1; i >= 0; --i)
        {
            const int j = s0 + i;
            if (j >= sLo && j < sHi)
            {
                const uint4 v = paddedPixel(j, ax, l);
                acc = (j == top || r == w - 1) ? v : reduce4(acc, v);
                r = r == 0 ? w - 1 : r - 1;
                res[i] = acc;
            }
        }

        const int eLo = sLo + 2 * h;
        r = 0;
        for (int e = eLo - eLo % w; e < eLo; ++e)
        {
            const uint4 v = paddedPixel(e, ax, l);
            acc = r == 0 ? v : reduce4(acc, v);
            r = r == w - 1 ? 0 : r + 1;
        }
        [ForceUnroll]
        for (int i = 0; i < int(kChunk); ++i)
        {
            const int j = s0 + i;
            if (j >= sLo && j < sHi)
            {
                const uint4 v = paddedPixel(j + 2 * h, ax, l);
                acc = r == 0 ? v : reduce4(acc, v);
                r = r == w - 1 ? 0 : r + 1;
                res[i] = reduce4(res[i], acc);
            }
        }
    }

    [ForceUnroll]
    for (int i = 0; i < int(kChunk); ++i)
    {
        const int pos = p0 + i;
        if (pos < len)
        {
            const uint o = outputIndex(line, uint(pos));
            for (uint c = 0; c < nc; ++c)
                dstBuf[o + c] = res[i][c];
        }
    }
}

// Dispatch shape from dispatchPass in Engine/Gpu/GpuMorphology.cpp. On the vertical pass a
// group's threads take adjacent columns, so a wave's loads are contiguous row segments: threads
// walking down one column would stride by whole rows, which at power-of-two row pitches puts every
// load on the same memory channel. On the horizontal pass group.y is the row and the threads take
// consecutive chunks of it.
[shader("compute")]
[numthreads(128, 1, 1)]
void morph_wide(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    if (params.vertical != 0)
        wideLine(gid.x * kGroup + tid.x, int(gid.y * kChunk));
    else
        wideLine(gid.y, int((gid.x * kGroup + tid.x) * kChunk));
}
