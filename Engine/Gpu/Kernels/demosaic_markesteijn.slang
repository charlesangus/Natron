/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/demosaicing/xtrans.c: xtrans_markesteijn_interpolate(), Frank Markesteijn's X-Trans
 * algorithm as darktable adapted it from dcraw 9.20 by Dave Coffin, Copyright (C) 2010-2026
 * darktable developers, distributed under the GNU GPL version 3 or later, with the arithmetic of
 * the CPU port in Engine/Nodes/Raw/RawDemosaicMarkesteijn.cpp.
 */

import rawlayout;

StructuredBuffer<float> srcBuf;
// The buffer's RGBA photosites: vng_linear's interpolation, then Markesteijn's result inside the
// border it leaves to that interpolation.
RWStructuredBuffer<float> plane;
// tileCount tiles of TS x TS photosites, each as RawKernels::markesteijn() lays one out, with
// separate rather than shared storage for what it overlaps: the ndir RGB directions, the green
// minima and maxima, the ndir derivatives, and the ndir homogeneity maps.
RWStructuredBuffer<float> scratch;
RWStructuredBuffer<float> dstBuf;

// RawMarkesteijnGpuPush in Engine/Nodes/Raw/RawGpuParams.h mirrors it. The pattern holds the
// buffer's 6x6 colours from its top-left photosite, 2 bits each, row by row, 16 to a word. The
// source and output rects are in buffer rows and columns, as VngParams's.
struct MarkesteijnParams
{
    uint pattern0;
    uint pattern1;
    uint pattern2;
    uint passes;
    uint width;
    uint height;
    uint srcOffset;
    uint srcStride;
    uint srcComps;
    int srcCol1;
    int srcRow1;
    int srcCol2;
    int srcRow2;
    uint firstTile;
    uint nLefts;
    uint tileCount;
    uint pass;
    uint dstOffset;
    uint dstStride;
    int outCol;
    int outRow;
    uint outWidth;
    uint outHeight;
    uint padding0;
    uint padding1;
    uint padding2;
    uint padding3;
    uint padding4;
};
[[vk::push_constant]] ConstantBuffer<MarkesteijnParams> pc;

static const int TS = 122;
static const int kTilePixels = TS * TS;

static const int kOrth[12] = { 1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1 };
static const int kPatt[32] = { 0, 1, 0, -1, 2, 0, -1, 0, 1, 1, 1, -1, 0, 0, 0, 0,
                               0, 1, 0, -2, 1, 0, -2, 0, 1, 1, -2, -2, 1, -1, -1, 1 };
static const int kDir[4] = { 1, TS, TS + 1, TS - 1 };

// The remainder and quotient of C's integer division, which truncates towards zero, on uints:
// the driver bug rawlayout.slang's wrapCell() avoids makes a negative OpSRem unreliable.
int cRem3(int v)
{
    return v >= 0 ? int(uint(v) % 3u) : -int(uint(-v) % 3u);
}

int cDiv3(int v)
{
    return v >= 0 ? int(uint(v) / 3u) : -int(uint(-v) / 3u);
}

// fcXTrans(): the colour of buffer photosite (row, col), which may be as far as 20 outside it.
int fc(int row, int col)
{
    const uint i = (uint(row + 600) % 6u) * 6u + uint(col + 600) % 6u;
    const uint word = i < 16u ? pc.pattern0 : (i < 32u ? pc.pattern1 : pc.pattern2);
    return int((word >> ((i & 15u) * 2u)) & 3u);
}

uint ndir()
{
    return pc.passes > 1u ? 8u : 4u;
}

int padTile()
{
    return pc.passes == 1u ? 12 : 17;
}

// The pattern position of the solitary green photosites, as the CPU finds it.
int2 solitaryGreen()
{
    int2 sg = int2(0, 0);
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            int ng = 0;
            for (int d = 0; d < 10; d += 2)
            {
                if (fc(row + kOrth[d], col + kOrth[d + 2]) == 1)
                    ng = 0;
                else
                    ++ng;
                if (ng == 4)
                    sg = int2(row, col);
            }
        }
    }
    return sg;
}

// allhex[row % 3][col % 3] of the CPU: offsets in tile photosites of the green hexagon around a
// non-green photosite, or of the non-green one around a green.
void hexmap(int row, int col, out int hex[8])
{
    [ForceUnroll]
    for (int k = 0; k < 8; ++k)
        hex[k] = 0;
    const int r = int(uint(row + 600) % 3u);
    const int c = int(uint(col + 600) % 3u);
    const int g = fc(r, c) == 1 ? 1 : 0;
    int ng = 0;
    for (int d = 0; d < 10; d += 2)
    {
        if (fc(r + kOrth[d], c + kOrth[d + 2]) == 1)
            ng = 0;
        else
            ++ng;
        if (ng == g + 1)
        {
            for (int k = 0; k < 8; ++k)
            {
                const int v = kOrth[d] * kPatt[g * 16 + k * 2] + kOrth[d + 1] * kPatt[g * 16 + k * 2 + 1];
                const int h = kOrth[d + 2] * kPatt[g * 16 + k * 2] + kOrth[d + 3] * kPatt[g * 16 + k * 2 + 1];
                hex[k ^ ((g * 2) & d)] = h + v * TS;
            }
        }
    }
}

struct Tile
{
    int top;
    int left;
    int mrow;
    int mcol;
    uint base;
    bool live;
}

// Tile firstTile + slot of the buffer, as the CPU numbers them; a tile it skips is not live.
Tile tileAt(uint slot)
{
    const int pad = padTile();
    const int step = TS - 2 * pad;
    const uint t = pc.firstTile + slot;
    const int width = int(pc.width);
    const int height = int(pc.height);
    Tile tile;
    tile.top = -pad + int(t / pc.nLefts) * step;
    tile.left = -pad + int(t % pc.nLefts) * step;
    tile.mrow = min(tile.top + TS, height + pad);
    tile.mcol = min(tile.left + TS, width + pad);
    tile.base = slot * uint(kTilePixels) * (5u * ndir() + 2u);
    tile.live = (slot < pc.tileCount) && (tile.top < height - pad) && (tile.left < width - pad);
    return tile;
}

uint rgbAt(Tile tile, uint d, int i, int c)
{
    return tile.base + (d * uint(kTilePixels) + uint(i)) * 3u + uint(c);
}

float rgb(Tile tile, uint d, int i, int c)
{
    return scratch[rgbAt(tile, d, i, c)];
}

uint gminAt(Tile tile, int i)
{
    return tile.base + 3u * ndir() * uint(kTilePixels) + uint(i);
}

uint gmaxAt(Tile tile, int i)
{
    return gminAt(tile, i) + uint(kTilePixels);
}

uint drvAt(Tile tile, uint d, int i)
{
    return gminAt(tile, 0) + (2u + d) * uint(kTilePixels) + uint(i);
}

uint homoAt(Tile tile, uint d, int i)
{
    return drvAt(tile, ndir() + d, i);
}

// Photosite (row, col) of the buffer, zero outside the source image.
float mosaic(int row, int col)
{
    if (col < pc.srcCol1 || col >= pc.srcCol2 || row < pc.srcRow1 || row >= pc.srcRow2)
        return 0.0;
    return srcBuf[pc.srcOffset + uint(pc.srcRow2 - 1 - row) * pc.srcStride + uint(col - pc.srcCol1) * pc.srcComps];
}

int translate(int n, int size)
{
    return (n >= size) ? (2 * size - n - 2) : abs(n);
}

float clamps(float a, float l, float h)
{
    return (a > l) ? ((a < h) ? a : h) : l;
}

float sqrf(float a)
{
    precise float s = a * a;
    return s;
}

// Each kernel but markesteijn_minmax and markesteijn_write runs a thread per tile photosite,
// (col, row) from the tile's top-left, a tile per z.

// The tile's first four directions: the mosaic inside the image, mirrored photosites or the mean
// of the mirrored 3x3 neighbours of the same colour outside it, zero past the tile's last row and
// column.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_fill(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int row = tile.top + int(tid.y);
    const int col = tile.left + int(tid.x);
    const int width = int(pc.width);
    const int height = int(pc.height);
    float3 pix = float3(0.0, 0.0, 0.0);
    if (row < tile.mrow && col < tile.mcol)
    {
        const int c = fc(row, col);
        if (col >= 0 && row >= 0 && col < width && row < height)
            pix[c] = rawFmax(0.0, mosaic(row, col));
        else
        {
            const int cy = translate(row, height);
            const int cx = translate(col, width);
            if (c == fc(cy, cx))
                pix[c] = rawFmax(0.0, mosaic(cy, cx));
            else
            {
                float sum = 0.0;
                uint count = 0;
                for (int y = row - 1; y <= row + 1; ++y)
                {
                    for (int x = col - 1; x <= col + 1; ++x)
                    {
                        const int yy = translate(y, height);
                        const int xx = translate(x, width);
                        if (fc(yy, xx) == c)
                        {
                            precise float s = sum + rawFmax(0.0, mosaic(yy, xx));
                            sum = s;
                            ++count;
                        }
                    }
                }
                // The CPU divides 0 by 0 when no neighbour has the colour; a device's division
                // by 0 is undefined.
                if (count != 0u)
                {
                    precise float mean = sum / float(count);
                    pix[c] = mean;
                }
                else
                    pix[c] = asfloat(0x7fc00000u);
            }
        }
    }
    const int i = int(tid.y) * TS + int(tid.x);
    [ForceUnroll]
    for (uint d = 0; d < 4; ++d)
    {
        [ForceUnroll]
        for (int c = 0; c < 3; ++c)
            scratch[rgbAt(tile, d, i, c)] = pix[c];
    }
}

// The green minimum and maximum around each pair of red and blue photosites, walking the tile's
// rows from `row` as the CPU does until the walk reaches the last column; the row it ends on.
// Which photosite of a pair reads the hexagon depends on where the walk enters the pair, and the
// walk hops between rows.
int minmaxWalk(Tile tile, int sgrow, int row)
{
    const float fltMax = asfloat(0x7f7fffffu);
    float lo = fltMax;
    float hi = 0.0;
    int col = tile.left + 3;
    while (col < tile.mcol - 3)
    {
        if (fc(row, col) == 1)
        {
            lo = fltMax;
            hi = 0.0;
            ++col;
            continue;
        }
        const int i = (row - tile.top) * TS + (col - tile.left);
        if (hi == 0.0)
        {
            int hex[8];
            hexmap(row, col, hex);
            for (int c = 0; c < 6; ++c)
            {
                const float val = rgb(tile, 0, i + hex[c], 1);
                if (lo > val)
                    lo = val;
                if (hi < val)
                    hi = val;
            }
        }
        scratch[gminAt(tile, i)] = lo;
        scratch[gmaxAt(tile, i)] = hi;
        const int m = cRem3(row - sgrow);
        if (m == 1)
        {
            if (row < tile.mrow - 4)
            {
                ++row;
                --col;
            }
        }
        else if (m == 2)
        {
            lo = fltMax;
            hi = 0.0;
            col += 2;
            if (col < tile.mcol - 4 && row > tile.top + 3)
                --row;
        }
        ++col;
    }
    return row;
}

// A thread a tile row, (row, tile) from the tile's top. The CPU's walk starts each row it enters
// from the left with nothing read, so each walk from a row is independent of the others. A walk
// from a row of vertical pairs above the last hops down to the lower photosite of every pair and
// ends there, since the last pair is too near the right edge to hop back up: the next walk starts
// two rows down, and no two walks write the same photosite.
[shader("compute")]
[numthreads(64, 1, 1)]
void markesteijn_minmax(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS))
        return;
    const int sgrow = solitaryGreen().x;
    const int start = tile.top + int(tid.x);
    int row = tile.top + 3;
    while (row < start)
        row += (cRem3(row - sgrow) == 1 && row < tile.mrow - 4) ? 2 : 1;
    if (row != start || row >= tile.mrow - 3)
        return;
    minmaxWalk(tile, sgrow, row);
}

// markesteijn_minmax as the CPU runs it, one thread a tile walking it whole: the reference the
// tests hold the parallel walks to.
[shader("compute")]
[numthreads(64, 1, 1)]
void markesteijn_minmax_serial(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.x);
    if (!tile.live)
        return;
    const int sgrow = solitaryGreen().x;
    int row = tile.top + 3;
    while (row < tile.mrow - 3)
        row = minmaxWalk(tile, sgrow, row) + 1;
}

// Green of the red and blue photosites in the four directions, clamped to the pair's range.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_green(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int row = tile.top + int(tid.y);
    const int col = tile.left + int(tid.x);
    if (row < tile.top + 3 || row >= tile.mrow - 3 || col < tile.left + 3 || col >= tile.mcol - 3)
        return;
    const int f = fc(row, col);
    if (f == 1)
        return;
    const int i = int(tid.y) * TS + int(tid.x);
    int hex[8];
    hexmap(row, col, hex);
    float color[4];
    {
        precise float v = 0.6796875 * (rgb(tile, 0, i + hex[1], 1) + rgb(tile, 0, i + hex[0], 1))
            - 0.1796875 * (rgb(tile, 0, i + 2 * hex[1], 1) + rgb(tile, 0, i + 2 * hex[0], 1));
        color[0] = v;
    }
    {
        precise float v = 0.87109375 * rgb(tile, 0, i + hex[3], 1) + rgb(tile, 0, i + hex[2], 1) * 0.13
            + 0.359375 * (rgb(tile, 0, i, f) - rgb(tile, 0, i - hex[2], f));
        color[1] = v;
    }
    [ForceUnroll]
    for (int c = 0; c < 2; ++c)
    {
        precise float v = 0.640625 * rgb(tile, 0, i + hex[4 + c], 1) + 0.359375 * rgb(tile, 0, i - 2 * hex[4 + c], 1)
            + 0.12890625 * (2.0 * rgb(tile, 0, i, f) - rgb(tile, 0, i + 3 * hex[4 + c], f) - rgb(tile, 0, i - 3 * hex[4 + c], f));
        color[2 + c] = v;
    }
    const uint flip = cRem3(row - solitaryGreen().x) == 0 ? 1u : 0u;
    const float lo = scratch[gminAt(tile, i)];
    const float hi = scratch[gmaxAt(tile, i)];
    [ForceUnroll]
    for (uint c = 0; c < 4; ++c)
        scratch[rgbAt(tile, c ^ flip, i, 1)] = clamps(color[c], lo, hi);
}

// The second pass's copy of the first four directions into the last four.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_copy(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int i = int(tid.y) * TS + int(tid.x);
    [ForceUnroll]
    for (uint d = 0; d < 4; ++d)
    {
        [ForceUnroll]
        for (int c = 0; c < 3; ++c)
            scratch[rgbAt(tile, d + 4, i, c)] = scratch[rgbAt(tile, d, i, c)];
    }
}

// The directions a pass works on: the first four in the first pass, the last four after.
uint passBase()
{
    return pc.pass > 0u ? 4u : 0u;
}

// Green recalculated from the interpolated values of closer photosites, in every pass but the
// first.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_recalc(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int row = tile.top + int(tid.y);
    const int col = tile.left + int(tid.x);
    if (row < tile.top + 6 || row >= tile.mrow - 6 || col < tile.left + 6 || col >= tile.mcol - 6)
        return;
    const int f = fc(row, col);
    if (f == 1)
        return;
    const int i = int(tid.y) * TS + int(tid.x);
    int hex[8];
    hexmap(row, col, hex);
    const uint flip = cRem3(row - solitaryGreen().x) == 0 ? 1u : 0u;
    const float lo = scratch[gminAt(tile, i)];
    const float hi = scratch[gmaxAt(tile, i)];
    for (int d = 3; d < 6; ++d)
    {
        const uint dd = passBase() + (uint(d - 2) ^ flip);
        precise float val = rgb(tile, dd, i - 2 * hex[d], 1) + 2.0 * rgb(tile, dd, i + hex[d], 1) - rgb(tile, dd, i - 2 * hex[d], f)
            - 2.0 * rgb(tile, dd, i + hex[d], f) + 3.0 * rgb(tile, dd, i, f);
        precise float third = val / 3.0;
        scratch[rgbAt(tile, dd, i, 1)] = clamps(third, lo, hi);
    }
}

// Red and blue of the solitary green photosites: six interpolations alternating horizontal and
// vertical, the last two directions taking the better of each pair.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_solitary(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int row = tile.top + int(tid.y);
    const int col = tile.left + int(tid.x);
    const int2 sg = solitaryGreen();
    const int pad = pc.passes == 1u ? 6 : 5;
    const int row0 = cDiv3(tile.top - sg.x + pad + 2) * 3 + sg.x;
    const int col0 = cDiv3(tile.left - sg.y + pad + 2) * 3 + sg.y;
    if (row < row0 || row >= tile.mrow - pad || col < col0 || col >= tile.mcol - pad)
        return;
    if (uint(row - row0) % 3u != 0u || uint(col - col0) % 3u != 0u)
        return;
    const int i = int(tid.y) * TS + int(tid.x);
    uint dd = passBase();
    int h = fc(row, col + 1);
    float diff[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    float color[12] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    int step = 1;
    for (int d = 0; d < 6; ++d)
    {
        for (int c = 0; c < 2; ++c)
        {
            const int o = step << c;
            precise float g = 2.0 * rgb(tile, dd, i, 1) - rgb(tile, dd, i + o, 1) - rgb(tile, dd, i - o, 1);
            precise float sum = g + rgb(tile, dd, i + o, h) + rgb(tile, dd, i - o, h);
            color[(h != 0 ? 6 : 0) + d] = sum;
            if (d > 1)
            {
                precise float e = rgb(tile, dd, i + o, 1) - rgb(tile, dd, i - o, 1) - rgb(tile, dd, i + o, h) + rgb(tile, dd, i - o, h);
                precise float acc = diff[d] + (sqrf(e) + sqrf(g));
                diff[d] = acc;
            }
            h ^= 2;
        }
        if (d < 2 || (d & 1) != 0)
        {
            precise float before = diff[max(d - 1, 0)];
            precise float here = diff[d];
            const int dOut = d - ((d > 1 && before < here) ? 1 : 0);
            precise float red = color[dOut] / 2.0;
            precise float blue = color[6 + dOut] / 2.0;
            scratch[rgbAt(tile, dd, i, 0)] = red;
            scratch[rgbAt(tile, dd, i, 2)] = blue;
            ++dd;
        }
        step ^= TS ^ 1;
        h ^= 2;
    }
}

// Red for the blue photosites and blue for the red, along the pair or across it, whichever is
// smoother in green.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_rb(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int row = tile.top + int(tid.y);
    const int col = tile.left + int(tid.x);
    const int pad = pc.passes == 1u ? 6 : 5;
    if (row < tile.top + pad || row >= tile.mrow - pad || col < tile.left + pad || col >= tile.mcol - pad)
        return;
    const int f = 2 - fc(row, col);
    if (f == 1)
        return;
    const int i = int(tid.y) * TS + int(tid.x);
    const int c = cRem3(row - solitaryGreen().x) != 0 ? TS : 1;
    const int h = 3 * (c ^ TS ^ 1);
    for (uint d = 0; d < 4; ++d)
    {
        const uint dd = passBase() + d;
        const float g0 = rgb(tile, dd, i, 1);
        bool along = (d > 1u) || (((int(d) ^ c) & 1) != 0);
        if (!along)
        {
            precise float near = abs(g0 - rgb(tile, dd, i + c, 1)) + abs(g0 - rgb(tile, dd, i - c, 1));
            precise float across = 2.0 * (abs(g0 - rgb(tile, dd, i + h, 1)) + abs(g0 - rgb(tile, dd, i - h, 1)));
            along = near < across;
        }
        const int o = along ? c : h;
        precise float v = (rgb(tile, dd, i + o, f) + rgb(tile, dd, i - o, f) + 2.0 * g0 - rgb(tile, dd, i + o, 1) - rgb(tile, dd, i - o, 1)) / 2.0;
        scratch[rgbAt(tile, dd, i, f)] = v;
    }
}

// Red and blue for the 2x2 blocks of green.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_twoxtwo(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int row = tile.top + int(tid.y);
    const int col = tile.left + int(tid.x);
    const int pad = pc.passes == 1u ? 8 : 4;
    if (row < tile.top + pad || row >= tile.mrow - pad || col < tile.left + pad || col >= tile.mcol - pad)
        return;
    const int2 sg = solitaryGreen();
    if (cRem3(row - sg.x) == 0 || cRem3(col - sg.y) == 0)
        return;
    const int i = int(tid.y) * TS + int(tid.x);
    int hex[8];
    hexmap(row, col, hex);
    uint dd = passBase();
    for (uint d = 0; d < ndir(); d += 2, ++dd)
    {
        const int a = hex[d];
        const int b = hex[d + 1];
        if (a + b != 0)
        {
            precise float g = 3.0 * rgb(tile, dd, i, 1) - 2.0 * rgb(tile, dd, i + a, 1) - rgb(tile, dd, i + b, 1);
            [ForceUnroll]
            for (int c = 0; c < 4; c += 2)
            {
                precise float v = (g + 2.0 * rgb(tile, dd, i + a, c) + rgb(tile, dd, i + b, c)) / 3.0;
                scratch[rgbAt(tile, dd, i, c)] = v;
            }
        }
        else
        {
            precise float g = 2.0 * rgb(tile, dd, i, 1) - rgb(tile, dd, i + a, 1) - rgb(tile, dd, i + b, 1);
            [ForceUnroll]
            for (int c = 0; c < 4; c += 2)
            {
                precise float v = (g + rgb(tile, dd, i + a, c) + rgb(tile, dd, i + b, c)) / 2.0;
                scratch[rgbAt(tile, dd, i, c)] = v;
            }
        }
    }
}

// YPbPr (ITU-R BT.2020) of direction d at tile photosite i, as the CPU stores it.
float3 ypbpr(Tile tile, uint d, int i)
{
    const float r = rgb(tile, d, i, 0);
    const float g = rgb(tile, d, i, 1);
    const float b = rgb(tile, d, i, 2);
    precise float y = 0.2627 * r + 0.6780 * g + 0.0593 * b;
    precise float pb = (b - y) * 0.56433;
    precise float pr = (r - y) * 0.67815;
    return float3(y, pb, pr);
}

// The derivatives of every direction in YPbPr, along the direction itself; the CPU's YPbPr tile
// of each direction is recomputed at the three photosites each derivative reads.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_derivatives(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int pad = pc.passes == 1u ? 9 : 14;
    const int row = int(tid.y);
    const int col = int(tid.x);
    if (row < pad || row >= tile.mrow - tile.top - pad || col < pad || col >= tile.mcol - tile.left - pad)
        return;
    const int i = row * TS + col;
    for (uint d = 0; d < ndir(); ++d)
    {
        const int f = kDir[d & 3u];
        const float3 y0 = ypbpr(tile, d, i);
        const float3 yp = ypbpr(tile, d, i + f);
        const float3 ym = ypbpr(tile, d, i - f);
        precise float e0 = 2.0 * y0.x - yp.x - ym.x;
        precise float e1 = 2.0 * y0.y - yp.y - ym.y;
        precise float e2 = 2.0 * y0.z - yp.z - ym.z;
        precise float v = sqrf(e0) + sqrf(e1) + sqrf(e2);
        scratch[drvAt(tile, d, i)] = v;
    }
}

// The homogeneity maps: how many of the 3x3 neighbours' derivatives in each direction are within
// 8 times the photosite's smallest, zero outside the region the CPU computes.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_homo(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int pad = pc.passes == 1u ? 10 : 15;
    const int row = int(tid.y);
    const int col = int(tid.x);
    const int i = row * TS + col;
    const bool inside = row >= pad && row < tile.mrow - tile.top - pad && col >= pad && col < tile.mcol - tile.left - pad;
    precise float tr = asfloat(0x7f7fffffu);
    if (inside)
    {
        for (uint d = 0; d < ndir(); ++d)
        {
            precise float v = scratch[drvAt(tile, d, i)];
            if (tr > v)
                tr = v;
        }
        precise float scaled = tr * 8.0;
        tr = scaled;
    }
    for (uint d = 0; d < ndir(); ++d)
    {
        uint count = 0;
        if (inside)
        {
            for (int v = -1; v <= 1; ++v)
            {
                for (int h = -1; h <= 1; ++h)
                {
                    precise float x = scratch[drvAt(tile, d, i + v * TS + h)];
                    if (x <= tr)
                        ++count;
                }
            }
        }
        scratch[homoAt(tile, d, i)] = float(count);
    }
}

// The 5x5 sums of the homogeneity maps, and the average of the most homogeneous directions,
// written to the buffer's photosites outside the border vng_linear keeps.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_final(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int pad = padTile();
    const int row = int(tid.y);
    const int col = int(tid.x);
    if (row < pad || row >= tile.mrow - tile.top - pad || col < pad || col >= tile.mcol - tile.left - pad)
        return;
    const int bufRow = row + tile.top;
    const int bufCol = col + tile.left;
    const int width = int(pc.width);
    const int height = int(pc.height);
    if (bufRow < pad || bufRow >= height - pad || bufCol < pad || bufCol >= width - pad)
        return;
    const int i = row * TS + col;
    const uint n = ndir();
    uint hm[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint maxval = 0;
    for (uint d = 0; d < n; ++d)
    {
        uint sum = 0;
        for (int v = -2; v <= 2; ++v)
        {
            for (int h = -2; h <= 2; ++h)
                sum += uint(scratch[homoAt(tile, d, i + v * TS + h)]);
        }
        hm[d] = sum;
        maxval = max(maxval, sum);
    }
    maxval -= maxval >> 3;
    for (uint d = 0; d + 4u < n; ++d)
    {
        if (hm[d] < hm[d + 4])
            hm[d] = 0;
        else if (hm[d] > hm[d + 4])
            hm[d + 4] = 0;
    }
    float avg[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (uint d = 0; d < n; ++d)
    {
        if (hm[d] >= maxval)
        {
            [ForceUnroll]
            for (int c = 0; c < 3; ++c)
            {
                precise float s = avg[c] + rgb(tile, d, i, c);
                avg[c] = s;
            }
            avg[3] += 1.0;
        }
    }
    const uint base = 4u * (uint(bufRow) * pc.width + uint(bufCol));
    [ForceUnroll]
    for (uint c = 0; c < 3; ++c)
    {
        precise float v = avg[c] / avg[3];
        plane[base + c] = rawFmax(0.0, v);
    }
}

// The output window, RGBA with an alpha of 1, black outside the buffer.
[shader("compute")]
[numthreads(8, 8, 1)]
void markesteijn_write(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.outWidth || tid.y >= pc.outHeight)
        return;
    const int col = pc.outCol + int(tid.x);
    const int row = pc.outRow - int(tid.y);
    float3 rgb = float3(0.0, 0.0, 0.0);
    if (row >= 0 && col >= 0 && row < int(pc.height) && col < int(pc.width))
    {
        const uint base = 4u * (uint(row) * pc.width + uint(col));
        rgb = float3(plane[base], plane[base + 1], plane[base + 2]);
    }
    const uint pixel = pc.dstOffset + tid.y * pc.dstStride + tid.x * 4;
    dstBuf[pixel] = rgb.x;
    dstBuf[pixel + 1] = rgb.y;
    dstBuf[pixel + 2] = rgb.z;
    dstBuf[pixel + 3] = 1.0;
}
