/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/demosaicing/amaze.cc, C path only: AMaZE by Emil Martinec, optimized by Ingo Weyrich,
 * incorporating ideas of Luis Sanz Rodrigues and Paul Lee, Copyright (C) 2011-2024 darktable
 * developers, distributed under the GNU GPL version 3 or later, with the arithmetic of the CPU
 * port in Engine/Nodes/Raw/RawDemosaicAmaze.cpp.
 */

StructuredBuffer<float> srcBuf;
// The buffer's RGBA photosites, of which AMaZE writes RGB.
RWStructuredBuffer<float> plane;
// tileCount tiles, each kTileFloats floats: kPlanes planes of TS x TS floats, then the tile's
// Nyquist bounds.
RWStructuredBuffer<float> scratch;
RWStructuredBuffer<float> dstBuf;

// RawAmazeGpuPush in Engine/Nodes/Raw/RawGpuParams.h mirrors it. The kernels work over a top-down
// buffer of width x height photosites whose Bayer pattern, from its top-left photosite, is
// filters. The source and output rects are in buffer rows and columns, as VngParams's. A dispatch
// works on tileCount of the buffer's tiles from firstTile, numbered as RawKernels::amaze()
// numbers them over nLefts columns of tiles.
struct AmazeParams
{
    uint filters;
    float clipPt;
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
};
[[vk::push_constant]] ConstantBuffer<AmazeParams> pc;

static const int TS = 160;
static const int TSH = 80;
static const int STEP = TS - 32;
static const int NP = TS * TS;
static const int HALF = TS * TSH;
static const int kPlanes = 15;
static const uint kTileFloats = uint(kPlanes * NP + 16);

static const int v1 = TS;
static const int v2 = 2 * TS;
static const int v3 = 3 * TS;
static const int p1 = -TS + 1;
static const int p2 = -2 * TS + 2;
static const int p3 = -3 * TS + 3;
static const int m1 = TS + 1;
static const int m2 = 2 * TS + 2;
static const int m3 = 3 * TS + 3;

// The planes. The CPU shares buffers between values whose lifetimes do not overlap; a plane here
// holds what the CPU's buffer of the same name holds, so a read of a photosite no stage wrote
// sees what the CPU's does. The CPU's half-size buffers sit at a plane's start or at HALF.
static const int CFA = 0;
static const int RGBGREEN = 1;
static const int DELHVSQSUM = 2; // pmwt
static const int DIRWTS0 = 3;
static const int DIRWTS1 = 4;
static const int VCD = 5; // rbm, then rbp at HALF
static const int HCD = 6;
static const int VCDALT = 7; // Dgrb[0], then Dgrb[1] at HALF
static const int HCDALT = 8;
static const int CDDIFFSQ = 9; // delp, then delm and rbint at HALF
static const int DGINTV = 10; // Dgrb2, h and v interleaved
static const int DGINTH = 11;
static const int HVWT = 12; // nyqutest at HALF
static const int DGRBSQ1 = 13; // Dgrbsq1m, then Dgrbsq1p at HALF
static const int NYQUIST = 14; // nyquist2 at HALF, as the byte values in floats

// The CPU's float constants, by their bits: a Slang literal need not round its decimal as the
// C++ compiler rounds it.
static const float kEps = asfloat(0x3727c5acu);
static const float kEpssq = asfloat(0x2edbe6ffu);
static const float kArthresh = 0.75;
static const float kGaussOdd0 = asfloat(0x3e161d99u);
static const float kGaussOdd1 = asfloat(0x3dd4286bu);
static const float kGaussOdd2 = asfloat(0x3d95ebc7u);
static const float kGaussOdd3 = asfloat(0x3d15ba05u);
static const float kGaussGrad0 = asfloat(0x3d173b96u);
static const float kGaussGrad1 = asfloat(0x3cfe427bu);
static const float kGaussGrad2 = asfloat(0x3cd5bc9bu);
static const float kGaussGrad3 = asfloat(0x3c970965u);
static const float kGaussGrad4 = asfloat(0x3c7dee19u);
static const float kGaussGrad5 = asfloat(0x3c16d744u);
static const float kGaussEven0 = asfloat(0x3e0c7cd5u);
static const float kGaussEven1 = asfloat(0x3d670656u);
static const float kGquinc0 = asfloat(0x3e2dfeb9u);
static const float kGquinc1 = asfloat(0x3ddf1f9bu);
static const float kGquinc2 = asfloat(0x3d8f1023u);
static const float kGquinc3 = asfloat(0x3ceb426eu);
static const float kPoint8 = asfloat(0x3f4ccccdu);
static const float k1325 = asfloat(0x3fa9999au);
static const float k0175 = asfloat(0x3e333333u);
static const float k0075 = asfloat(0x3d99999au);

struct Tile
{
    int top;
    int left;
    int rr1;
    int cc1;
    int rrmin;
    int ccmin;
    int rrmax;
    int ccmax;
    uint base;
    bool live;
}

Tile tileAt(uint slot)
{
    const uint t = pc.firstTile + slot;
    const int width = int(pc.width);
    const int height = int(pc.height);
    Tile tile;
    tile.top = -16 + int(t / pc.nLefts) * STEP;
    tile.left = -16 + int(t % pc.nLefts) * STEP;
    const int bottom = min(tile.top + TS, height + 16);
    const int right = min(tile.left + TS, width + 16);
    tile.rr1 = bottom - tile.top;
    tile.cc1 = right - tile.left;
    tile.rrmin = tile.top < 0 ? 16 : 0;
    tile.ccmin = tile.left < 0 ? 16 : 0;
    tile.rrmax = bottom > height ? height - tile.top : tile.rr1;
    tile.ccmax = right > width ? width - tile.left : tile.cc1;
    tile.base = slot * kTileFloats;
    tile.live = slot < pc.tileCount;
    return tile;
}

uint at(Tile tile, int p, int i)
{
    return tile.base + uint(p * NP + i);
}

float ld(Tile tile, int p, int i)
{
    return scratch[at(tile, p, i)];
}

void st(Tile tile, int p, int i, float v)
{
    scratch[at(tile, p, i)] = v;
}

float cfa(Tile tile, int i)
{
    return ld(tile, CFA, i);
}

// fcBayer() of tile photosite (rr, cc): tiles start on multiples of 8, the pattern's period.
int fc(int rr, int cc)
{
    return int((pc.filters >> uint((((rr << 1) & 14) + (cc & 1)) << 1)) & 3u);
}

bool greenRow(int rr)
{
    return (fc(rr, 2) & 1) != 0;
}

// A red or blue photosite of rows and columns [lo, rr1 - lo) x [lo, cc1 - lo), lo being even.
bool rbSite(Tile tile, int rr, int cc, int lo)
{
    return rr >= lo && rr < tile.rr1 - lo && cc >= lo && cc < tile.cc1 - lo && (cc & 1) == (fc(rr, 2) & 1);
}

float sqrf(float a)
{
    precise float s = a * a;
    return s;
}

float addExponent(float d, int n)
{
    uint u = asuint(d);
    if ((u & 0x7FFFFFFFu) != 0u)
        u = u + uint(n << 23);
    return asfloat(u);
}

float xmul2f(float d)
{
    return addExponent(d, 1);
}

float xdiv2f(float d)
{
    return addExponent(d, -1);
}

float xdiv4f(float d)
{
    return addExponent(d, -2);
}

float amazeMin(float a, float b)
{
    return a < b ? a : b;
}

float amazeMax(float a, float b)
{
    return a > b ? a : b;
}

float lim(float x, float lo, float hi)
{
    return amazeMax(lo, amazeMin(x, hi));
}

float ulim(float x, float y, float z)
{
    return y < z ? lim(x, y, z) : lim(x, z, y);
}

float clampnan(float x)
{
    if (isinf(x))
        return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    if (isnan(x))
        return 0.5;
    return x;
}

// Photosite (row, col) of the buffer, zero outside the source image.
float mosaic(int row, int col)
{
    if (col < pc.srcCol1 || col >= pc.srcCol2 || row < pc.srcRow1 || row >= pc.srcRow2)
        return 0.0;
    return srcBuf[pc.srcOffset + uint(pc.srcRow2 - 1 - row) * pc.srcStride + uint(col - pc.srcCol1) * pc.srcComps];
}

// in[row * width + col] of the CPU, which reads on into the next or previous row where col is past
// the buffer's edge.
float flat(int row, int col)
{
    const int p = row * int(pc.width) + col;
    if (p < 0 || p >= int(pc.width * pc.height))
        return 0.0;
    return mosaic(int(uint(p) / pc.width), int(uint(p) % pc.width));
}

bool inFill(int L, int r0, int c0, int hr, int wc, out int i, out int j)
{
    const int d = L - (r0 * TS + c0);
    i = 0;
    j = 0;
    if (d < 0)
        return false;
    i = int(uint(d) / uint(TS));
    j = int(uint(d) % uint(TS));
    return i < hr && j < wc;
}

// What the CPU's fill leaves at float L of its cfa buffer, false where it writes nothing. A fill
// written later wins, so they are tried in reverse. Each walks its rows at TS floats a row, so a
// border wider than the space left in its row runs on into the next, and the lower border of a
// tile ending fewer than 16 rows short of TS rows runs on past the buffer.
bool fillAt(Tile tile, int L, out float v)
{
    const int width = int(pc.width);
    const int height = int(pc.height);
    const bool upper = tile.rrmin > 0;
    const bool lower = tile.rrmax < tile.rr1;
    const bool left = tile.ccmin > 0;
    const bool right = tile.ccmax < tile.cc1;
    const int hr = tile.rrmax - tile.rrmin;
    const int wc = tile.ccmax - tile.ccmin;
    int i;
    int j;
    v = 0.0;
    if (lower && left && inFill(L, tile.rrmax, 0, 16, 16, i, j))
        v = flat(height - i - 2, 32 - j);
    else if (upper && right && inFill(L, 0, tile.ccmax, 16, 16, i, j))
        v = flat(32 - i, width - j - 2);
    else if (lower && right && inFill(L, tile.rrmax, tile.ccmax, 16, 16, i, j))
        v = flat(height - i - 2, width - j - 2);
    else if (upper && left && inFill(L, 0, 0, 16, 16, i, j))
        v = flat(32 - i, 32 - j);
    else if (right && inFill(L, tile.rrmin, tile.ccmax, hr, 16, i, j))
        v = flat(tile.top + tile.rrmin + i, width - j - 2);
    else if (left && inFill(L, tile.rrmin, 0, hr, 16, i, j))
        v = flat(tile.rrmin + i + tile.top, 32 - j + tile.left);
    else if (lower && inFill(L, tile.rrmax, tile.ccmin, 16, wc, i, j))
        v = flat(height - i - 2, tile.left + tile.ccmin + j);
    else if (inFill(L, tile.rrmin, tile.ccmin, hr, wc, i, j))
        v = flat(tile.rrmin + i + tile.top, tile.ccmin + j + tile.left);
    else if (upper && inFill(L, 0, tile.ccmin, 16, wc, i, j))
        v = flat(32 - i + tile.top, tile.ccmin + j + tile.left);
    else
        return false;
    return true;
}

struct Nyquist
{
    int startRow;
    int endRow;
    int startCol;
    int endCol;
    bool on;
}

uint headerAt(Tile tile, int k)
{
    return tile.base + uint(kPlanes * NP + k);
}

Nyquist nyquistOf(Tile tile)
{
    Nyquist ny;
    ny.startRow = asint(scratch[headerAt(tile, 0)]);
    ny.endRow = asint(scratch[headerAt(tile, 1)]);
    ny.startCol = asint(scratch[headerAt(tile, 2)]);
    ny.endCol = asint(scratch[headerAt(tile, 3)]);
    ny.on = asint(scratch[headerAt(tile, 4)]) != 0;
    return ny;
}

bool nyquistSite(Nyquist ny, int rr, int cc)
{
    return ny.on && rr >= ny.startRow && rr < ny.endRow && cc >= ny.startCol && cc < ny.endCol && (cc & 1) == (fc(rr, 2) & 1);
}

float nyquist(Tile tile, int k)
{
    return ld(tile, NYQUIST, k);
}

// nyquist2[k] of the CPU: the bytes of cddiffsq, of which a Nyquist tile clears and votes on
// [4 * TSH, (TS - 4) * TSH).
bool nyquist2(Tile tile, Nyquist ny, int k)
{
    if (ny.on && k >= 4 * TSH && k < (TS - 4) * TSH)
        return ld(tile, NYQUIST, HALF + k) != 0.0;
    return ((asuint(ld(tile, CDDIFFSQ, k >> 2)) >> uint((k & 3) * 8)) & 0xFFu) != 0u;
}

float hvwt(Tile tile, int k)
{
    return ld(tile, HVWT, k);
}

float dgrb(Tile tile, int c, int k)
{
    return ld(tile, VCDALT, c * HALF + k);
}

// Each kernel but amaze_variance, amaze_nybounds, amaze_populate, amaze_pmwt and amaze_write runs
// a thread per tile photosite, (cc, rr) from the tile's top-left, a tile per z.

// The tile's cfa and rgbgreen, every other plane zero but the bytes the fill writes past cfa into
// nyquist.
[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_fill(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live || tid.x >= uint(TS) || tid.y >= uint(TS))
        return;
    const int L = int(tid.y) * TS + int(tid.x);
    for (int p = DELHVSQSUM; p < NYQUIST; ++p)
        st(tile, p, L, 0.0);
    float v;
    const float value = fillAt(tile, L, v) ? v : 0.0;
    st(tile, CFA, L, value);
    st(tile, RGBGREEN, L, value);
    float nyq = 0.0;
    if (L < HALF)
    {
        float past;
        if (fillAt(tile, NP + 32 + (L >> 2), past))
            nyq = float((asuint(past) >> uint((L & 3) * 8)) & 0xFFu);
    }
    st(tile, NYQUIST, L, nyq);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_gradients(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!tile.live || rr < 2 || rr >= tile.rr1 - 2 || cc < 2 || cc >= tile.cc1 - 2)
        return;
    const int i = rr * TS + cc;
    const float c0 = cfa(tile, i);
    precise float delh = abs(cfa(tile, i + 1) - cfa(tile, i - 1));
    precise float delv = abs(cfa(tile, i + v1) - cfa(tile, i - v1));
    precise float w0 = kEps + abs(cfa(tile, i + v2) - c0) + abs(c0 - cfa(tile, i - v2)) + delv;
    precise float w1 = kEps + abs(cfa(tile, i + 2) - c0) + abs(c0 - cfa(tile, i - 2)) + delh;
    precise float sum = sqrf(delh) + sqrf(delv);
    st(tile, DIRWTS0, i, w0);
    st(tile, DIRWTS1, i, w1);
    st(tile, DELHVSQSUM, i, sum);
}

// The colour ratio towards the photosite at offset o, whose weight is at 2 * o.
float ratio(Tile tile, int dirwts, int i, int o)
{
    const float wFar = ld(tile, dirwts, i + 2 * o);
    const float w = ld(tile, dirwts, i);
    precise float r = cfa(tile, i + o) * (wFar + w) / (wFar * (kEps + cfa(tile, i)) + w * (kEps + cfa(tile, i + 2 * o)));
    return r;
}

float hamiltonAdams(Tile tile, int i, int o)
{
    precise float g = cfa(tile, i + o) + xdiv2f(cfa(tile, i) - cfa(tile, i + 2 * o));
    return g;
}

float adaptive(Tile tile, int i, float cr, float ha)
{
    precise float distance = abs(1.0 - cr);
    if (distance < kArthresh)
    {
        precise float g = cfa(tile, i) * cr;
        return g;
    }
    return ha;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_cd(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!tile.live || rr < 4 || rr >= tile.rr1 - 4 || cc < 4 || cc >= tile.cc1 - 4)
        return;
    const int i = rr * TS + cc;
    const bool fcswitch = ((fc(rr, 4) & 1) ^ ((cc - 4) & 1)) != 0;
    const float c0 = cfa(tile, i);

    const float cru = ratio(tile, DIRWTS0, i, -v1);
    const float crd = ratio(tile, DIRWTS0, i, v1);
    const float crl = ratio(tile, DIRWTS1, i, -1);
    const float crr = ratio(tile, DIRWTS1, i, 1);

    const float guha = hamiltonAdams(tile, i, -v1);
    const float gdha = hamiltonAdams(tile, i, v1);
    const float glha = hamiltonAdams(tile, i, -1);
    const float grha = hamiltonAdams(tile, i, 1);

    float guar = adaptive(tile, i, cru, guha);
    float gdar = adaptive(tile, i, crd, gdha);
    float glar = adaptive(tile, i, crl, glha);
    float grar = adaptive(tile, i, crr, grha);

    const float wl = ld(tile, DIRWTS1, i - 1);
    const float wu = ld(tile, DIRWTS0, i - v1);
    precise float hwt = wl / (wl + ld(tile, DIRWTS1, i + 1));
    precise float vwt = wu / (ld(tile, DIRWTS0, i + v1) + wu);

    precise float gintvha = vwt * gdha + (1.0 - vwt) * guha;
    precise float ginthha = hwt * grha + (1.0 - hwt) * glha;

    precise float gintvar = vwt * gdar + (1.0 - vwt) * guar;
    precise float ginthar = hwt * grar + (1.0 - hwt) * glar;
    float vcd;
    float hcd;
    float vcdalt;
    float hcdalt;
    if (fcswitch)
    {
        precise float a = c0 - gintvar;
        precise float b = c0 - ginthar;
        precise float c = c0 - gintvha;
        precise float d = c0 - ginthha;
        vcd = a;
        hcd = b;
        vcdalt = c;
        hcdalt = d;
    }
    else
    {
        precise float a = gintvar - c0;
        precise float b = ginthar - c0;
        precise float c = gintvha - c0;
        precise float d = ginthha - c0;
        vcd = a;
        hcd = b;
        vcdalt = c;
        hcdalt = d;
    }

    const float clip8 = kPoint8 * pc.clipPt;
    if (c0 > clip8 || gintvha > clip8 || ginthha > clip8)
    {
        guar = guha;
        gdar = gdha;
        glar = glha;
        grar = grha;
        vcd = vcdalt;
        hcd = hcdalt;
    }
    precise float uvha = guha - gdha;
    precise float uvar = guar - gdar;
    precise float lrha = glha - grha;
    precise float lrar = glar - grar;
    st(tile, VCD, i, vcd);
    st(tile, HCD, i, hcd);
    st(tile, VCDALT, i, vcdalt);
    st(tile, HCDALT, i, hcdalt);
    st(tile, DGINTV, i, amazeMin(sqrf(uvha), sqrf(uvar)));
    st(tile, DGINTH, i, amazeMin(sqrf(lrha), sqrf(lrar)));
}

float variance3(Tile tile, int p, int i, int step)
{
    const float a = ld(tile, p, i - step);
    const float b = ld(tile, p, i);
    const float c = ld(tile, p, i + step);
    precise float v = 3.0 * (sqrf(a) + sqrf(b) + sqrf(c)) - sqrf(a + b + c);
    return v;
}

// One photosite of the CPU's variance pass over the colour differences in plane p, which runs
// along the direction of step, nb being the neighbour its bounds clamp to.
void boundDifference(Tile tile, int p, int alt, int i, int step, int nb, bool green)
{
    const float var = variance3(tile, p, i, step);
    const float altvar = variance3(tile, alt, i, step);
    float d = ld(tile, p, i);
    if (altvar < var)
        d = ld(tile, alt, i);
    const float c0 = cfa(tile, i);
    const float lo = cfa(tile, i - nb);
    const float hi = cfa(tile, i + nb);
    if (green)
    {
        precise float g = -d + c0;
        precise float bounded = -ulim(g, lo, hi) + c0;
        if (d > 0.0)
        {
            precise float d3 = 3.0 * d;
            precise float gc = g + c0;
            if (d3 > gc)
                d = bounded;
            else
            {
                precise float w = 1.0 - 3.0 * d / (kEps + g + c0);
                precise float blend = w * d + (1.0 - w) * bounded;
                d = blend;
            }
        }
        if (g > pc.clipPt)
            d = bounded;
    }
    else
    {
        precise float g = d + c0;
        precise float bounded = ulim(g, lo, hi) - c0;
        if (d < 0.0)
        {
            precise float d3 = 3.0 * d;
            precise float gc = -(g + c0);
            if (d3 < gc)
                d = bounded;
            else
            {
                precise float w = 1.0 + 3.0 * d / (kEps + g + c0);
                precise float blend = w * d + (1.0 - w) * bounded;
                d = blend;
            }
        }
        if (g > pc.clipPt)
            d = bounded;
    }
    st(tile, p, i, d);
}

// A thread per chain of photosites two apart, x being 2 * line + parity: tid.y 0 walks the
// horizontal differences of a row, tid.y 1 the vertical ones of a column. A photosite's variance
// reads its predecessor's bounded difference and its successor's unbounded one, and neither
// direction reads the other's.
[shader("compute")]
[numthreads(64, 1, 1)]
void amaze_variance(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live)
        return;
    const int line = 4 + int(tid.x >> 1);
    const int parity = int(tid.x & 1u);
    if (tid.y == 0u)
    {
        const int rr = line;
        if (rr >= tile.rr1 - 4)
            return;
        const bool green = ((fc(rr, 4) & 1) ^ parity) != 0;
        for (int cc = 4 + parity; cc < tile.cc1 - 4; cc += 2)
            boundDifference(tile, HCD, HCDALT, rr * TS + cc, 2, 1, green);
    }
    else if (tid.y == 1u)
    {
        const int cc = line;
        if (cc >= tile.cc1 - 4)
            return;
        for (int rr = 4 + parity; rr < tile.rr1 - 4; rr += 2)
        {
            const bool green = ((fc(rr, 4) & 1) ^ ((cc - 4) & 1)) != 0;
            boundDifference(tile, VCD, VCDALT, rr * TS + cc, v2, v1, green);
        }
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_cddiffsq(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!tile.live || rr < 4 || rr >= tile.rr1 - 4 || cc < 4 || cc >= tile.cc1 - 4)
        return;
    if (((fc(rr, 4) & 1) ^ ((cc - 4) & 1)) != 0)
        return;
    const int i = rr * TS + cc;
    precise float d = ld(tile, VCD, i) - ld(tile, HCD, i);
    st(tile, CDDIFFSQ, i, sqrf(d));
}

float average4(Tile tile, int p, int i, int o)
{
    precise float a = ld(tile, p, i) + ld(tile, p, i + o) + ld(tile, p, i + 2 * o) + ld(tile, p, i + 3 * o);
    return a;
}

float spread4(Tile tile, int p, int i, int o, float ave)
{
    precise float s = sqrf(ld(tile, p, i) - ave) + sqrf(ld(tile, p, i + o) - ave) + sqrf(ld(tile, p, i + 2 * o) - ave)
                      + sqrf(ld(tile, p, i + 3 * o) - ave);
    return s;
}

float sum3(Tile tile, int p, int i, int o)
{
    precise float s = ld(tile, p, i) + ld(tile, p, i + o) + ld(tile, p, i + 2 * o);
    return s;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_hvwt(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!tile.live || !rbSite(tile, rr, cc, 6))
        return;
    const int i = rr * TS + cc;
    const float uave = average4(tile, VCD, i, -v1);
    const float dave = average4(tile, VCD, i, v1);
    const float lave = average4(tile, HCD, i, -1);
    const float rave = average4(tile, HCD, i, 1);

    const float varu = spread4(tile, VCD, i, -v1, uave);
    const float vard = spread4(tile, VCD, i, v1, dave);
    const float varl = spread4(tile, HCD, i, -1, lave);
    const float varr = spread4(tile, HCD, i, 1, rave);

    const float wl = ld(tile, DIRWTS1, i - 1);
    const float wu = ld(tile, DIRWTS0, i - v1);
    precise float hwt = wl / (wl + ld(tile, DIRWTS1, i + 1));
    precise float vwt = wu / (ld(tile, DIRWTS0, i + v1) + wu);

    precise float vcdvar = kEpssq + vwt * vard + (1.0 - vwt) * varu;
    precise float hcdvar = kEpssq + hwt * varr + (1.0 - hwt) * varl;

    const float flu = sum3(tile, DGINTV, i, -v1);
    const float fld = sum3(tile, DGINTV, i, v1);
    const float fll = sum3(tile, DGINTH, i, -1);
    const float flr = sum3(tile, DGINTH, i, 1);

    precise float vcdvar1 = kEpssq + vwt * fld + (1.0 - vwt) * flu;
    precise float hcdvar1 = kEpssq + hwt * flr + (1.0 - hwt) * fll;

    precise float varwt = hcdvar / (vcdvar + hcdvar);
    precise float diffwt = hcdvar1 / (vcdvar1 + hcdvar1);

    // The CPU multiplies the two distances from 0.5 in double, where the product of floats is
    // exact: the test is that their signs agree, which a float product can lose to underflow.
    precise float dv = 0.5 - varwt;
    precise float dd = 0.5 - diffwt;
    const bool agree = (dv > 0.0 && dd > 0.0) || (dv < 0.0 && dd < 0.0);
    st(tile, HVWT, i >> 1, (agree && abs(dd) < abs(dv)) ? varwt : diffwt);
}

float cdsq(Tile tile, int i, int o)
{
    return ld(tile, CDDIFFSQ, i + o);
}

float dhv(Tile tile, int i, int o)
{
    return ld(tile, DELHVSQSUM, i + o);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_nyqutest(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!tile.live || !rbSite(tile, rr, cc, 6))
        return;
    const int i = rr * TS + cc;
    precise float test
        = (kGaussOdd0 * cdsq(tile, i, 0)
           + kGaussOdd1 * (cdsq(tile, i, -m1) + cdsq(tile, i, p1) + cdsq(tile, i, -p1) + cdsq(tile, i, m1))
           + kGaussOdd2 * (cdsq(tile, i, -v2) + cdsq(tile, i, -2) + cdsq(tile, i, 2) + cdsq(tile, i, v2))
           + kGaussOdd3 * (cdsq(tile, i, -m2) + cdsq(tile, i, p2) + cdsq(tile, i, -p2) + cdsq(tile, i, m2)))
          - (kGaussGrad0 * dhv(tile, i, 0)
             + kGaussGrad1 * (dhv(tile, i, -v1) + dhv(tile, i, 1) + dhv(tile, i, -1) + dhv(tile, i, v1))
             + kGaussGrad2 * (dhv(tile, i, -m1) + dhv(tile, i, p1) + dhv(tile, i, -p1) + dhv(tile, i, m1))
             + kGaussGrad3 * (dhv(tile, i, -v2) + dhv(tile, i, -2) + dhv(tile, i, 2) + dhv(tile, i, v2))
             + kGaussGrad4 * (dhv(tile, i, -v2 - 1) + dhv(tile, i, -v2 + 1) + dhv(tile, i, -TS - 2) + dhv(tile, i, -TS + 2) + dhv(tile, i, TS - 2)
                              + dhv(tile, i, TS + 2) + dhv(tile, i, v2 - 1) + dhv(tile, i, v2 + 1))
             + kGaussGrad5 * (dhv(tile, i, -m2) + dhv(tile, i, p2) + dhv(tile, i, -p2) + dhv(tile, i, m2)));
    st(tile, HVWT, HALF + (i >> 1), test);
    if (test > 0.0)
        st(tile, NYQUIST, i >> 1, 1.0);
}

groupshared int nyBounds[4];

// A workgroup per tile, a thread per row: the rows and columns the Nyquist pass covers.
[shader("compute")]
[numthreads(160, 1, 1)]
void amaze_nybounds(uint3 tid : SV_DispatchThreadID, uint3 lid : SV_GroupThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live)
        return;
    if (lid.x == 0u)
    {
        nyBounds[0] = 0x7FFFFFFF;
        nyBounds[1] = 0;
        nyBounds[2] = TS + 1;
        nyBounds[3] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const int rr = 6 + int(lid.x);
    if (rr < tile.rr1 - 6)
    {
        int first = TS + 1;
        int last = 0;
        for (int cc = 6 + (fc(rr, 2) & 1); cc < tile.cc1 - 6; cc += 2)
        {
            if (ld(tile, HVWT, HALF + ((rr * TS + cc) >> 1)) > 0.0)
            {
                first = min(first, cc);
                last = max(last, cc);
            }
        }
        if (last > 0)
        {
            InterlockedMin(nyBounds[0], rr);
            InterlockedMax(nyBounds[1], rr);
            InterlockedMin(nyBounds[2], first);
            InterlockedMax(nyBounds[3], last);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lid.x != 0u)
        return;
    int startRow = nyBounds[0] == 0x7FFFFFFF ? 0 : nyBounds[0];
    int endRow = nyBounds[1];
    int startCol = nyBounds[2];
    int endCol = nyBounds[3];
    const bool on = startRow != endRow && startCol != endCol;
    if (on)
    {
        endRow++;
        endCol++;
        startCol -= (startCol & 1);
        startRow = max(8, startRow);
        endRow = min(tile.rr1 - 8, endRow);
        startCol = max(8, startCol);
        endCol = min(tile.cc1 - 8, endCol);
    }
    scratch[headerAt(tile, 0)] = asfloat(startRow);
    scratch[headerAt(tile, 1)] = asfloat(endRow);
    scratch[headerAt(tile, 2)] = asfloat(startCol);
    scratch[headerAt(tile, 3)] = asfloat(endCol);
    scratch[headerAt(tile, 4)] = asfloat(on ? 1 : 0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_nyvote(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live)
        return;
    const Nyquist ny = nyquistOf(tile);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!nyquistSite(ny, rr, cc))
        return;
    const int i = rr * TS + cc;
    const uint votes = uint(nyquist(tile, (i - v2) >> 1)) + uint(nyquist(tile, (i - m1) >> 1)) + uint(nyquist(tile, (i + p1) >> 1))
                       + uint(nyquist(tile, (i - 2) >> 1)) + uint(nyquist(tile, (i + 2) >> 1)) + uint(nyquist(tile, (i - p1) >> 1))
                       + uint(nyquist(tile, (i + m1) >> 1)) + uint(nyquist(tile, (i + v2) >> 1));
    const float self = nyquist(tile, i >> 1);
    st(tile, NYQUIST, HALF + (i >> 1), votes > 4u ? 1.0 : (votes < 4u ? 0.0 : self));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_nyarea(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live)
        return;
    const Nyquist ny = nyquistOf(tile);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!nyquistSite(ny, rr, cc))
        return;
    const int i = rr * TS + cc;
    if (!nyquist2(tile, ny, i >> 1))
        return;
    float sumcfa = 0.0;
    float sumh = 0.0;
    float sumv = 0.0;
    float sumsqh = 0.0;
    float sumsqv = 0.0;
    float areawt = 0.0;
    for (int y = -6; y < 7; y += 2)
    {
        for (int x = -6; x < 7; x += 2)
        {
            const int j = i + y * TS + x;
            if (nyquist2(tile, ny, j >> 1))
            {
                const float c = cfa(tile, j);
                const float l = cfa(tile, j - 1);
                const float r = cfa(tile, j + 1);
                const float u = cfa(tile, j - v1);
                const float d = cfa(tile, j + v1);
                precise float a = sumcfa + c;
                precise float h = sumh + (l + r);
                precise float v = sumv + (u + d);
                precise float sh = sumsqh + (sqrf(c - l) + sqrf(c - r));
                precise float sv = sumsqv + (sqrf(c - u) + sqrf(c - d));
                precise float w = areawt + 1.0;
                sumcfa = a;
                sumh = h;
                sumv = v;
                sumsqh = sh;
                sumsqv = sv;
                areawt = w;
            }
        }
    }
    precise float dh = sumcfa - xdiv2f(sumh);
    precise float dv = sumcfa - xdiv2f(sumv);
    const float half = xdiv2f(areawt);
    precise float hcdvar = kEpssq + abs(half * sumsqh - dh * dh);
    precise float vcdvar = kEpssq + abs(half * sumsqv - dv * dv);
    precise float wt = hcdvar / (vcdvar + hcdvar);
    st(tile, HVWT, i >> 1, wt);
}

// A workgroup per tile. Each row's hvwt reads the row above as this pass left it and the row below
// as it found it, so the rows run in order; a row's photosites read no other of the row's.
[shader("compute")]
[numthreads(64, 1, 1)]
void amaze_populate(uint3 tid : SV_DispatchThreadID, uint3 lid : SV_GroupThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live)
        return;
    const Nyquist ny = nyquistOf(tile);
    for (int rr = 8; rr < tile.rr1 - 8; ++rr)
    {
        for (int n = int(lid.x); n < TSH; n += 64)
        {
            const int cc = 8 + (fc(rr, 2) & 1) + 2 * n;
            if (cc >= tile.cc1 - 8)
                break;
            const int i = rr * TS + cc;
            const int k = i >> 1;
            precise float sum = hvwt(tile, (i - m1) >> 1) + hvwt(tile, (i + p1) >> 1) + hvwt(tile, (i - p1) >> 1) + hvwt(tile, (i + m1) >> 1);
            const float alt = xdiv4f(sum);
            float wt = hvwt(tile, k);
            precise float dSelf = abs(0.5 - wt);
            precise float dAlt = abs(0.5 - alt);
            if (dSelf < dAlt)
                wt = alt;
            st(tile, HVWT, k, wt);
            const float hcd = ld(tile, HCD, i);
            precise float d = wt * (ld(tile, VCD, i) - hcd) + hcd;
            precise float g = cfa(tile, i) + d;
            st(tile, VCDALT, k, d);
            st(tile, RGBGREEN, i, g);
            float curvh = 0.0;
            float curvv = 0.0;
            if (nyquist2(tile, ny, k))
            {
                precise float hsum = ld(tile, RGBGREEN, i - 1) + ld(tile, RGBGREEN, i + 1);
                precise float vsum = ld(tile, RGBGREEN, i - v1) + ld(tile, RGBGREEN, i + v1);
                precise float h = g - xdiv2f(hsum);
                precise float v = g - xdiv2f(vsum);
                curvh = sqrf(h);
                curvv = sqrf(v);
            }
            st(tile, DGINTV, 2 * k, curvh);
            st(tile, DGINTV, 2 * k + 1, curvv);
        }
        DeviceMemoryBarrierWithGroupSync();
    }
}

float curvature(Tile tile, int i, int o, int hv)
{
    return ld(tile, DGINTV, 2 * ((i + o) >> 1) + hv);
}

float gquincAverage(Tile tile, int i, int hv)
{
    precise float g = kEpssq
                      + (kGquinc0 * curvature(tile, i, 0, hv)
                         + kGquinc1 * (curvature(tile, i, -m1, hv) + curvature(tile, i, p1, hv) + curvature(tile, i, -p1, hv) + curvature(tile, i, m1, hv))
                         + kGquinc2 * (curvature(tile, i, -v2, hv) + curvature(tile, i, -2, hv) + curvature(tile, i, 2, hv) + curvature(tile, i, v2, hv))
                         + kGquinc3 * (curvature(tile, i, -m2, hv) + curvature(tile, i, p2, hv) + curvature(tile, i, -p2, hv) + curvature(tile, i, m2, hv)));
    return g;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_nyrefine(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live)
        return;
    const Nyquist ny = nyquistOf(tile);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!nyquistSite(ny, rr, cc))
        return;
    const int i = rr * TS + cc;
    if (!nyquist2(tile, ny, i >> 1))
        return;
    const float gvarh = gquincAverage(tile, i, 0);
    const float gvarv = gquincAverage(tile, i, 1);
    precise float d = (ld(tile, HCD, i) * gvarv + ld(tile, VCD, i) * gvarh) / (gvarv + gvarh);
    precise float g = cfa(tile, i) + d;
    st(tile, VCDALT, i >> 1, d);
    st(tile, RGBGREEN, i, g);
}

float diagonalSquares(Tile tile, int at, int o)
{
    precise float s = sqrf(cfa(tile, at) - cfa(tile, at - o)) + sqrf(cfa(tile, at) - cfa(tile, at + o));
    return s;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_diagprep(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!tile.live || rr < 6 || rr >= tile.rr1 - 6 || cc < 6 || cc >= tile.cc1 - 6 || (cc & 1) != 0)
        return;
    const int i = rr * TS + cc;
    const int k = i >> 1;
    const int rb = greenRow(rr) ? i : i + 1;
    const int g = greenRow(rr) ? i + 1 : i;
    precise float delp = abs(cfa(tile, g + p1) - cfa(tile, g - p1));
    precise float delm = abs(cfa(tile, g + m1) - cfa(tile, g - m1));
    st(tile, CDDIFFSQ, k, delp);
    st(tile, CDDIFFSQ, HALF + k, delm);
    st(tile, DGRBSQ1, HALF + k, diagonalSquares(tile, rb, p1));
    st(tile, DGRBSQ1, k, diagonalSquares(tile, rb, m1));
}

float diagonalRatio(Tile tile, int i, int o)
{
    precise float r = xmul2f(cfa(tile, i + o)) / (kEps + cfa(tile, i) + cfa(tile, i + 2 * o));
    return r;
}

float diagonalColour(Tile tile, int i, int o, float cr)
{
    precise float distance = abs(1.0 - cr);
    if (distance < kArthresh)
    {
        precise float v = cfa(tile, i) * cr;
        return v;
    }
    precise float v = cfa(tile, i + o) + xdiv2f(cfa(tile, i) - cfa(tile, i + 2 * o));
    return v;
}

float diagonalWeight(Tile tile, int del, int i, int o)
{
    precise float w = kEps + ld(tile, CDDIFFSQ, del + (i >> 1)) + ld(tile, CDDIFFSQ, del + ((i + o) >> 1)) + ld(tile, CDDIFFSQ, del + ((i + 2 * o) >> 1));
    return w;
}

float rbVariance(Tile tile, int sq, int i)
{
    const int b = sq;
    precise float v = kEpssq
                      + (kGaussEven0 * (ld(tile, DGRBSQ1, b + ((i - v1) >> 1)) + ld(tile, DGRBSQ1, b + ((i - 1) >> 1)) + ld(tile, DGRBSQ1, b + ((i + 1) >> 1)) + ld(tile, DGRBSQ1, b + ((i + v1) >> 1)))
                         + kGaussEven1 * (ld(tile, DGRBSQ1, b + ((i - v2 - 1) >> 1)) + ld(tile, DGRBSQ1, b + ((i - v2 + 1) >> 1)) + ld(tile, DGRBSQ1, b + ((i - 2 - v1) >> 1))
                                          + ld(tile, DGRBSQ1, b + ((i + 2 - v1) >> 1)) + ld(tile, DGRBSQ1, b + ((i - 2 + v1) >> 1)) + ld(tile, DGRBSQ1, b + ((i + 2 + v1) >> 1))
                                          + ld(tile, DGRBSQ1, b + ((i + v2 - 1) >> 1)) + ld(tile, DGRBSQ1, b + ((i + v2 + 1) >> 1))));
    return v;
}

// The CPU's bound of a diagonal R+B interpolation rb towards offset o in regions of high
// saturation.
float boundDiagonal(Tile tile, int i, int o, float rb)
{
    const float c0 = cfa(tile, i);
    const float lo = cfa(tile, i - o);
    const float hi = cfa(tile, i + o);
    float v = rb;
    if (v < c0)
    {
        if (xmul2f(v) < c0)
            v = ulim(v, lo, hi);
        else
        {
            precise float diff = c0 - v;
            precise float wt = xmul2f(diff) / (kEps + v + c0);
            precise float blend = wt * v + (1.0 - wt) * ulim(v, lo, hi);
            v = blend;
        }
    }
    if (v > pc.clipPt)
        v = ulim(v, lo, hi);
    return v;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_diag(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!tile.live || !rbSite(tile, rr, cc, 8))
        return;
    const int i = rr * TS + cc;
    const int k = i >> 1;
    const float crse = diagonalRatio(tile, i, m1);
    const float crnw = diagonalRatio(tile, i, -m1);
    const float crne = diagonalRatio(tile, i, p1);
    const float crsw = diagonalRatio(tile, i, -p1);
    const float rbse = diagonalColour(tile, i, m1, crse);
    const float rbnw = diagonalColour(tile, i, -m1, crnw);
    const float rbne = diagonalColour(tile, i, p1, crne);
    const float rbsw = diagonalColour(tile, i, -p1, crsw);

    const float wtse = diagonalWeight(tile, HALF, i, m1);
    const float wtnw = diagonalWeight(tile, HALF, i, -m1);
    const float wtne = diagonalWeight(tile, 0, i, p1);
    const float wtsw = diagonalWeight(tile, 0, i, -p1);

    precise float rbm = (wtse * rbnw + wtnw * rbse) / (wtse + wtnw);
    precise float rbp = (wtne * rbsw + wtsw * rbne) / (wtne + wtsw);

    const float rbvarm = rbVariance(tile, 0, i);
    const float rbvarp = rbVariance(tile, HALF, i);
    precise float pmwt = rbvarm / (rbvarp + rbvarm);
    st(tile, DELHVSQSUM, k, pmwt);

    st(tile, VCD, HALF + k, boundDiagonal(tile, i, p1, rbp));
    st(tile, VCD, k, boundDiagonal(tile, i, m1, rbm));
}

float pmwt(Tile tile, int k)
{
    return ld(tile, DELHVSQSUM, k);
}

// A workgroup per tile, the rows in order as amaze_populate's.
[shader("compute")]
[numthreads(64, 1, 1)]
void amaze_pmwt(uint3 tid : SV_DispatchThreadID, uint3 lid : SV_GroupThreadID)
{
    const Tile tile = tileAt(tid.z);
    if (!tile.live)
        return;
    for (int rr = 10; rr < tile.rr1 - 10; ++rr)
    {
        for (int n = int(lid.x); n < TSH; n += 64)
        {
            const int cc = 10 + (fc(rr, 2) & 1) + 2 * n;
            if (cc >= tile.cc1 - 10)
                break;
            const int i = rr * TS + cc;
            const int k = i >> 1;
            precise float sum = pmwt(tile, (i - m1) >> 1) + pmwt(tile, (i + p1) >> 1) + pmwt(tile, (i - p1) >> 1) + pmwt(tile, (i + m1) >> 1);
            const float alt = xdiv4f(sum);
            float wt = pmwt(tile, k);
            precise float dSelf = abs(0.5 - wt);
            precise float dAlt = abs(0.5 - alt);
            if (dSelf < dAlt)
            {
                wt = alt;
                st(tile, DELHVSQSUM, k, wt);
            }
            precise float rb = cfa(tile, i) + ld(tile, VCD, k) * (1.0 - wt) + ld(tile, VCD, HALF + k) * wt;
            st(tile, CDDIFFSQ, HALF + k, xdiv2f(rb));
        }
        DeviceMemoryBarrierWithGroupSync();
    }
}

float rbint(Tile tile, int k)
{
    return ld(tile, CDDIFFSQ, HALF + k);
}

// G towards offset o from R+B at half-plane offset ok; the CPU divides 2 * cfa in double, which
// rounds the float quotient correctly.
float greenTowards(Tile tile, int i, int o, int k, int ok)
{
    const float rb = rbint(tile, k);
    const float rbFar = rbint(tile, k + ok);
    precise float cr = cfa(tile, i + o) * 2.0 / (kEps + rb + rbFar);
    precise float distance = abs(1.0 - cr);
    if (distance < kArthresh)
    {
        precise float g = rb * cr;
        return g;
    }
    precise float g = cfa(tile, i + o) + xdiv2f(rb - rbFar);
    return g;
}

float boundGreen(Tile tile, int i, int o, float g, float rb)
{
    const float lo = cfa(tile, i - o);
    const float hi = cfa(tile, i + o);
    float v = g;
    if (v < rb)
    {
        precise float twice = 2.0 * v;
        if (twice < rb)
            v = ulim(v, lo, hi);
        else
        {
            precise float wt = 2.0 * (rb - v) / (kEps + v + rb);
            precise float blend = wt * v + (1.0 - wt) * ulim(v, lo, hi);
            v = blend;
        }
    }
    return v;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_grefine(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!tile.live || !rbSite(tile, rr, cc, 12))
        return;
    const int i = rr * TS + cc;
    const int k = i >> 1;
    const float wt = hvwt(tile, k);
    precise float dp = abs(0.5 - pmwt(tile, k));
    precise float dh = abs(0.5 - wt);
    if (dp < dh)
        return;
    const float gu = greenTowards(tile, i, -v1, k, -v1);
    const float gd = greenTowards(tile, i, v1, k, v1);
    const float gl = greenTowards(tile, i, -1, k, -1);
    const float gr = greenTowards(tile, i, 1, k, 1);

    const float wu = ld(tile, DIRWTS0, i - v1);
    const float wd = ld(tile, DIRWTS0, i + v1);
    const float wl = ld(tile, DIRWTS1, i - 1);
    const float wr = ld(tile, DIRWTS1, i + 1);
    precise float gintv0 = (wu * gd + wd * gu) / (wd + wu);
    precise float ginth0 = (wl * gr + wr * gl) / (wl + wr);

    const float rb = rbint(tile, k);
    float gintv = boundGreen(tile, i, v1, gintv0, rb);
    float ginth = boundGreen(tile, i, 1, ginth0, rb);
    if (ginth > pc.clipPt)
        ginth = ulim(ginth, cfa(tile, i - 1), cfa(tile, i + 1));
    if (gintv > pc.clipPt)
        gintv = ulim(gintv, cfa(tile, i - v1), cfa(tile, i + v1));

    precise float g = ginth * (1.0 - wt) + gintv * wt;
    precise float d = g - cfa(tile, i);
    st(tile, RGBGREEN, i, g);
    st(tile, VCDALT, k, d);
}

// A thread per half-plane slot, (slot column, row), splitting G-B out of Dgrb[0] on blue rows.
[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_split(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    if (!tile.live || tid.x >= uint(TSH) || rr >= TS)
        return;
    int ex;
    int ey;
    if (fc(0, 0) == 1)
    {
        ey = fc(0, 1) == 0 ? 0 : 1;
        ex = 1 - ey;
    }
    else
    {
        ey = fc(0, 0) == 0 ? 0 : 1;
        ex = ey;
    }
    if (rr < 13 - ey || rr >= tile.rr1 - 12 || ((rr - (13 - ey)) & 1) != 0)
        return;
    const int k = rr * TSH + int(tid.x);
    if (k < ((rr * TS + 13 - ex) >> 1) || k >= ((rr * TS + tile.cc1 - 12) >> 1))
        return;
    st(tile, VCDALT, HALF + k, dgrb(tile, 0, k));
    st(tile, VCDALT, k, 0.0);
}

// 1 / (eps + |D(a) - D(b)| + |D(a) - D(c)| + |D(b) - D(d)|) of colour difference c's plane.
float chromaWeight(Tile tile, int c, int i, int a, int b, int far1, int far2)
{
    const float da = dgrb(tile, c, (i + a) >> 1);
    const float db = dgrb(tile, c, (i + b) >> 1);
    precise float w = 1.0 / (kEps + abs(da - db) + abs(da - dgrb(tile, c, (i + far1) >> 1)) + abs(db - dgrb(tile, c, (i + far2) >> 1)));
    return w;
}

float chromaArm(Tile tile, int c, int i, int o, int far, int side, int up)
{
    precise float v = k1325 * dgrb(tile, c, (i + o) >> 1) - k0175 * dgrb(tile, c, (i + far) >> 1) - k0075 * dgrb(tile, c, (i + o + side) >> 1)
                      - k0075 * dgrb(tile, c, (i + o + up) >> 1);
    return v;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_chroma(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!tile.live || !rbSite(tile, rr, cc, 14))
        return;
    const int i = rr * TS + cc;
    const int c = 1 - fc(rr, cc) / 2;
    const float wtnw = chromaWeight(tile, c, i, -m1, m1, -m3, -m3);
    const float wtne = chromaWeight(tile, c, i, p1, -p1, p3, p3);
    const float wtsw = chromaWeight(tile, c, i, -p1, p1, m3, -p3);
    const float wtse = chromaWeight(tile, c, i, m1, -m1, -p3, m3);
    const float nw = chromaArm(tile, c, i, -m1, -m3, -2, -v2);
    const float ne = chromaArm(tile, c, i, p1, p3, 2, v2);
    const float sw = chromaArm(tile, c, i, -p1, -p3, -2, -v2);
    const float se = chromaArm(tile, c, i, m1, m3, 2, v2);
    precise float v = (wtnw * nw + wtne * ne + wtsw * sw + wtse * se) / (wtnw + wtne + wtsw + wtse);
    st(tile, VCDALT, c * HALF + (i >> 1), v);
}

// The colour difference c at green photosite i, weighted over its four neighbours.
float weightedDifference(Tile tile, int c, int i)
{
    const float hu = hvwt(tile, (i - v1) >> 1);
    const float hr = hvwt(tile, (i + 1) >> 1);
    const float hl = hvwt(tile, (i - 1) >> 1);
    const float hd = hvwt(tile, (i + v1) >> 1);
    precise float temp = 1.0 / (hu + 2.0 - hr - hl + hd);
    precise float v = ld(tile, RGBGREEN, i)
                      - (hu * dgrb(tile, c, (i - v1) >> 1) + (1.0 - hr) * dgrb(tile, c, (i + 1) >> 1) + (1.0 - hl) * dgrb(tile, c, (i - 1) >> 1)
                         + hd * dgrb(tile, c, (i + v1) >> 1))
                            * temp;
    return v;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_output(uint3 tid : SV_DispatchThreadID)
{
    const Tile tile = tileAt(tid.z);
    const int rr = int(tid.y);
    const int cc = int(tid.x);
    if (!tile.live || rr < 16 || rr >= tile.rr1 - 16 || cc < 16 || cc >= tile.cc1 - 16)
        return;
    const int row = rr + tile.top;
    const int col = cc + tile.left;
    if (col >= int(pc.width) || row >= int(pc.height))
        return;
    const int i = rr * TS + cc;
    const float g = ld(tile, RGBGREEN, i);
    float red;
    float blue;
    if ((((cc - 16) & 1) == 0) == greenRow(rr))
    {
        red = weightedDifference(tile, 0, i);
        blue = weightedDifference(tile, 1, i);
    }
    else
    {
        precise float r = g - dgrb(tile, 0, i >> 1);
        precise float b = g - dgrb(tile, 1, i >> 1);
        red = r;
        blue = b;
    }
    const uint base = 4u * (uint(row) * pc.width + uint(col));
    plane[base] = clampnan(red);
    plane[base + 1] = clampnan(g);
    plane[base + 2] = clampnan(blue);
}

// The output window, RGBA with an alpha of 1, black outside the buffer.
[shader("compute")]
[numthreads(8, 8, 1)]
void amaze_write(uint3 tid : SV_DispatchThreadID)
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
