/*
 * Ported from darktable's OpenCL demosaic kernels, data/kernels/demosaic_ppg.cl and
 * data/kernels/demosaic_rcd.cl at DARKTABLE_REF release-5.6.2, Copyright (C) 2010-2026 darktable
 * developers, distributed under the GNU GPL version 3 or later, in the pass structure of those
 * kernels and with the arithmetic of the CPU port in Engine/Nodes/Raw/RawDemosaicBasics.cpp.
 *
 * RATIO CORRECTED DEMOSAICING
 * Luis Sanz Rodriguez (luis.sanz.rodriguez(at)gmail(dot)com)
 * Release 2.3 @ 171125
 * Original code from https://github.com/LuisSR/RCD-Demosaicing
 * Licensed under the GNU GPL version 3
 */

import rawlayout;

// The CPU port runs RCD over 112-photosite tiles whose results it keeps 10 photosites in from
// every tile edge but the buffer's. These kernels run each step over the whole buffer instead,
// with the same row and column ranges counted from the buffer's edges and the same half-width
// indexing of the planes the CPU stores at every other photosite, which gives the CPU's values
// wherever it keeps them. The planes are `stride` floats a row, an even number, as the CPU's tile
// width is.

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> work;
RWStructuredBuffer<float> dstBuf;

// RawDemosaicGpuPush in Engine/Nodes/Raw/RawGpuParams.h mirrors it.
struct DemosaicParams
{
    int bufX1;
    int bufY2;
    uint width;
    uint height;
    uint stride;
    uint filters;
    float threshold;
    float revScaler;
    float scaler;
    uint margin;
    uint mode;
    uint srcOffset;
    uint srcStride;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcComps;
    uint dstOffset;
    uint dstStride;
    int outX;
    int outY;
    uint outWidth;
    uint outHeight;
};
[[vk::push_constant]] ConstantBuffer<DemosaicParams> pc;

static const uint kModePpg = 0;
static const uint kModeRcd = 1;
static const uint kModePassThrough = 2;

static const float kRcdEps = asfloat(0x3727c5acu);
static const float kRcdEpsSq = asfloat(0x2edbe6ffu);
static const uint kRcdBorder = 10;
static const uint kRcdMargin = 9;

// The work buffer, in floats: the median-filtered mosaic (width x height), the PPG result before
// and after its red and blue pass (4 floats a photosite each), then the RCD planes of stride x
// height: cfa, the vertical-horizontal direction, the low-pass filter shared with the P-Q
// direction (half), the P and Q high-pass filters (half each), and red, green and blue.
uint planeArea()
{
    return pc.width * pc.height;
}

uint rcdArea()
{
    return pc.stride * pc.height;
}

uint medOffset()
{
    return 0;
}

uint ppgOffset()
{
    return planeArea();
}

uint ppgRbOffset()
{
    return 5 * planeArea();
}

uint cfaOffset()
{
    return 9 * planeArea();
}

uint vhOffset()
{
    return cfaOffset() + rcdArea();
}

uint lpqOffset()
{
    return vhOffset() + rcdArea();
}

uint pOffset()
{
    return lpqOffset() + rcdArea() / 2;
}

uint qOffset()
{
    return pOffset() + rcdArea() / 2;
}

uint rgbOffset(uint c)
{
    return qOffset() + rcdArea() / 2 + c * rcdArea();
}

// RawKernels::fcBayer().
uint fc(uint row, uint col)
{
    return (pc.filters >> ((((row << 1) & 14u) + (col & 1u)) << 1)) & 3u;
}

// Photosite (row, col) of the buffer, zero outside the source image.
float mosaic(int row, int col)
{
    const int x = pc.bufX1 + col;
    const int y = pc.bufY2 - 1 - row;
    if (x < pc.srcX1 || x >= pc.srcX2 || y < pc.srcY1 || y >= pc.srcY2)
        return 0.0;
    return srcBuf[pc.srcOffset + uint(y - pc.srcY1) * pc.srcStride + uint(x - pc.srcX1) * pc.srcComps];
}

float sqrf(float a)
{
    precise float r = a * a;
    return r;
}

// a * b + (1 - a) * c, in darktable's evaluation order.
float interpolatef(float a, float b, float c)
{
    precise float r = a * (b - c) + c;
    return r;
}

float clip01(float x)
{
    return x >= 0.0 ? (x <= 1.0 ? x : 1.0) : 0.0;
}

bool insideBuffer(int row, int col)
{
    return row >= 0 && col >= 0 && row < int(pc.height) && col < int(pc.width);
}

float ppgAt(uint row, uint col, uint c)
{
    return work[ppgOffset() + 4 * (row * pc.width + col) + c];
}

// RawKernels' preMedian(): the median of the 9 greens around each green photosite, those further
// than the threshold from it pushed past the others; every other photosite is copied.
[shader("compute")]
[numthreads(8, 8, 1)]
void demosaic_median(uint3 tid : SV_DispatchThreadID)
{
    const uint col = tid.x;
    const uint row = tid.y;
    if (col >= pc.width || row >= pc.height)
        return;
    const float centre = mosaic(int(row), int(col));
    float result = centre;
    const uint first = (fc(row, 3) != 1 && fc(row, 3) != 3) ? 4u : 3u;
    if (row >= 3 && row + 3 < pc.height && col >= first && col + 3 < pc.width && ((col - first) & 1u) == 0)
    {
        const int lim[5] = { 0, 1, 2, 1, 0 };
        float med[9];
        int cnt = 0;
        int k = 0;
        for (int i = 0; i < 5; ++i)
        {
            for (int j = -lim[i]; j <= lim[i]; j += 2)
            {
                const float p = mosaic(int(row) + i - 2, int(col) + j);
                if (abs(p - centre) < pc.threshold)
                {
                    med[k] = p;
                    ++cnt;
                }
                else
                {
                    precise float pushed = 64.0 + p;
                    med[k] = pushed;
                }
                ++k;
            }
        }
        for (int i = 0; i < 8; ++i)
        {
            for (int ii = i + 1; ii < 9; ++ii)
            {
                if (med[i] > med[ii])
                {
                    const float t = med[i];
                    med[i] = med[ii];
                    med[ii] = t;
                }
            }
        }
        precise float back = med[4] - 64.0;
        result = (cnt == 1) ? back : med[(cnt - 1) / 2];
    }
    work[medOffset() + row * pc.width + col] = result;
}

// The photosite PPG's green pass reads: the median-filtered one, except past the margin of a
// row the margin splits, where darktable reads the unfiltered input.
float greenInput(int row, int col, bool unfiltered)
{
    if (pc.threshold > 0.0 && !unfiltered)
        return work[medOffset() + uint(row) * pc.width + uint(col)];
    return mosaic(row, col);
}

// The first two passes of RawKernels::ppg(): the average of each colour over the 3x3 block on the
// outer three photosites, and green on red and blue photosites inside them, which a margin below
// the buffer's size limits to a frame margin + 3 wide. Every other photosite reads zero.
[shader("compute")]
[numthreads(8, 8, 1)]
void demosaic_ppg_green(uint3 tid : SV_DispatchThreadID)
{
    const int i = int(tid.x);
    const int j = int(tid.y);
    const int width = int(pc.width);
    const int height = int(pc.height);
    if (i >= width || j >= height)
        return;
    float4 site = float4(0.0, 0.0, 0.0, 0.0);
    const bool interiorRow = (j >= 3) && (j < height - 3) && (width - 3 > 3);
    const bool border = !(interiorRow && i >= 3 && i < width - 3);
    const int frame = int(pc.margin) + 3;
    const bool rowSplits = (j >= frame) && (j < height - frame) && (width - frame > frame);
    const bool green = (j >= 3) && (j < height - 3) && (i >= 3) && (i < width - 3) && !(rowSplits && i >= frame && i < width - frame);
    if (border)
    {
        float sum[8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        for (int y = j - 1; y != j + 2; ++y)
        {
            for (int x = i - 1; x != i + 2; ++x)
            {
                if (insideBuffer(y, x))
                {
                    const uint f = fc(uint(y), uint(x));
                    precise float s = sum[f] + mosaic(y, x);
                    sum[f] = s;
                    precise float n = sum[f + 4] + 1.0;
                    sum[f + 4] = n;
                }
            }
        }
        const uint f = fc(uint(j), uint(i));
        const float centre = rawFmax(0.0, mosaic(j, i));
        for (uint c = 0; c < 3; ++c)
        {
            if (c != f && sum[c + 4] > 0.0)
            {
                precise float avg = sum[c] / sum[c + 4];
                site[c] = rawFmax(0.0, avg);
            }
            else
                site[c] = centre;
        }
    }
    else if (green)
    {
        const bool unfiltered = rowSplits && i >= width - frame;
        const uint c = fc(uint(j), uint(i));
        float4 color = float4(0.0, 0.0, 0.0, 0.0);
        const float pcv = greenInput(j, i, unfiltered);
        if (c == 0 || c == 2)
        {
            color[c] = pcv;
            const float pym = greenInput(j - 1, i, unfiltered);
            const float pym2 = greenInput(j - 2, i, unfiltered);
            const float pym3 = greenInput(j - 3, i, unfiltered);
            const float pyM = greenInput(j + 1, i, unfiltered);
            const float pyM2 = greenInput(j + 2, i, unfiltered);
            const float pyM3 = greenInput(j + 3, i, unfiltered);
            const float pxm = greenInput(j, i - 1, unfiltered);
            const float pxm2 = greenInput(j, i - 2, unfiltered);
            const float pxm3 = greenInput(j, i - 3, unfiltered);
            const float pxM = greenInput(j, i + 1, unfiltered);
            const float pxM2 = greenInput(j, i + 2, unfiltered);
            const float pxM3 = greenInput(j, i + 3, unfiltered);

            precise float guessx = (pxm + pcv + pxM) * 2.0 - pxM2 - pxm2;
            precise float diffx = (abs(pxm2 - pcv) + abs(pxM2 - pcv) + abs(pxm - pxM)) * 3.0 + (abs(pxM3 - pxM) + abs(pxm3 - pxm)) * 2.0;
            precise float guessy = (pym + pcv + pyM) * 2.0 - pyM2 - pym2;
            precise float diffy = (abs(pym2 - pcv) + abs(pyM2 - pcv) + abs(pym - pyM)) * 3.0 + (abs(pyM3 - pyM) + abs(pym3 - pym)) * 2.0;
            if (diffx > diffy)
            {
                precise float g = guessy * 0.25;
                color[1] = rawFmax(rawFmin(g, rawFmax(pym, pyM)), rawFmin(pym, pyM));
            }
            else
            {
                precise float g = guessx * 0.25;
                color[1] = rawFmax(rawFmin(g, rawFmax(pxm, pxM)), rawFmin(pxm, pxM));
            }
        }
        else
            color[1] = pcv;
        [ForceUnroll]
        for (uint k = 0; k < 4; ++k)
            site[k] = rawFmax(0.0, color[k]);
    }
    const uint base = ppgOffset() + 4 * (uint(j) * pc.width + uint(i));
    [ForceUnroll]
    for (uint k = 0; k < 4; ++k)
        work[base + k] = site[k];
}

// The last pass of RawKernels::ppg(): red and blue from the neighbours' colour differences with
// green, on a frame margin photosites wide, or everywhere for a margin over half the buffer.
[shader("compute")]
[numthreads(8, 8, 1)]
void demosaic_ppg_rb(uint3 tid : SV_DispatchThreadID)
{
    const int i = int(tid.x);
    const int j = int(tid.y);
    const int width = int(pc.width);
    const int height = int(pc.height);
    if (i >= width || j >= height)
        return;
    const uint ui = uint(i);
    const uint uj = uint(j);
    float4 color = float4(ppgAt(uj, ui, 0), ppgAt(uj, ui, 1), ppgAt(uj, ui, 2), ppgAt(uj, ui, 3));
    const int margin = int(pc.margin);
    const bool skipped = (j >= margin) && (j < height - margin) && (width - margin > margin) && (i >= margin) && (i < width - margin);
    if (!skipped)
    {
        if (j > 0 && i > 0 && i < width - 1 && j < height - 1)
        {
            const uint c = fc(uj, ui);
            if ((c & 1u) != 0)
            {
                if (fc(uj, ui + 1) == 0)
                {
                    precise float b = (ppgAt(uj - 1, ui, 2) + ppgAt(uj + 1, ui, 2) + 2.0 * color[1] - ppgAt(uj - 1, ui, 1) - ppgAt(uj + 1, ui, 1)) * 0.5;
                    precise float r = (ppgAt(uj, ui - 1, 0) + ppgAt(uj, ui + 1, 0) + 2.0 * color[1] - ppgAt(uj, ui - 1, 1) - ppgAt(uj, ui + 1, 1)) * 0.5;
                    color[2] = b;
                    color[0] = r;
                }
                else
                {
                    precise float r = (ppgAt(uj - 1, ui, 0) + ppgAt(uj + 1, ui, 0) + 2.0 * color[1] - ppgAt(uj - 1, ui, 1) - ppgAt(uj + 1, ui, 1)) * 0.5;
                    precise float b = (ppgAt(uj, ui - 1, 2) + ppgAt(uj, ui + 1, 2) + 2.0 * color[1] - ppgAt(uj, ui - 1, 1) - ppgAt(uj, ui + 1, 1)) * 0.5;
                    color[0] = r;
                    color[2] = b;
                }
            }
            else
            {
                const uint other = (c == 0) ? 2u : 0u;
                const float tl = ppgAt(uj - 1, ui - 1, other);
                const float br = ppgAt(uj + 1, ui + 1, other);
                const float tr = ppgAt(uj - 1, ui + 1, other);
                const float bl = ppgAt(uj + 1, ui - 1, other);
                const float tlG = ppgAt(uj - 1, ui - 1, 1);
                const float brG = ppgAt(uj + 1, ui + 1, 1);
                const float trG = ppgAt(uj - 1, ui + 1, 1);
                const float blG = ppgAt(uj + 1, ui - 1, 1);
                precise float diff1 = abs(tl - br) + abs(tlG - color[1]) + abs(brG - color[1]);
                precise float guess1 = tl + br + 2.0 * color[1] - tlG - brG;
                precise float diff2 = abs(tr - bl) + abs(trG - color[1]) + abs(blG - color[1]);
                precise float guess2 = tr + bl + 2.0 * color[1] - trG - blG;
                precise float value = 0.0;
                if (diff1 > diff2)
                    value = guess2 * 0.5;
                else if (diff1 < diff2)
                    value = guess1 * 0.5;
                else
                    value = (guess1 + guess2) * 0.25;
                color[other] = value;
            }
        }
        [ForceUnroll]
        for (uint k = 0; k < 4; ++k)
            color[k] = rawFmax(0.0, color[k]);
    }
    const uint base = ppgRbOffset() + 4 * (uj * pc.width + ui);
    [ForceUnroll]
    for (uint k = 0; k < 4; ++k)
        work[base + k] = color[k];
}

float cfaAt(uint index)
{
    return work[cfaOffset() + index];
}

float vhAt(uint index)
{
    return work[vhOffset() + index];
}

float lpqAt(uint index)
{
    return work[lpqOffset() + index];
}

float rgbAt(uint c, uint index)
{
    return work[rgbOffset(c) + index];
}

// The CPU's per-tile population: cfa, and on each row the two colours it holds set to cfa in
// every column, the third zero. Columns past the buffer read zero.
[shader("compute")]
[numthreads(8, 8, 1)]
void rcd_populate(uint3 tid : SV_DispatchThreadID)
{
    const uint col = tid.x;
    const uint row = tid.y;
    if (col >= pc.stride || row >= pc.height)
        return;
    float v = 0.0;
    if (col < pc.width)
    {
        precise float scaled = rawFmax(0.0, mosaic(int(row), int(col))) * pc.revScaler;
        v = scaled;
    }
    const uint index = row * pc.stride + col;
    const uint c0 = fc(row, 0);
    const uint c1 = fc(row, 1);
    work[cfaOffset() + index] = v;
    [ForceUnroll]
    for (uint c = 0; c < 3; ++c)
        work[rgbOffset(c) + index] = (c == c0 || c == c1) ? v : 0.0;
}

float rcdVerticalHpf(uint index)
{
    const uint w1 = pc.stride;
    precise float d = (cfaAt(index - 3 * w1) - cfaAt(index - w1) - cfaAt(index + w1) + cfaAt(index + 3 * w1)) - 3.0 * (cfaAt(index - 2 * w1) + cfaAt(index + 2 * w1)) + 6.0 * cfaAt(index);
    return sqrf(d);
}

float rcdHorizontalHpf(uint index)
{
    precise float d = (cfaAt(index - 3) - cfaAt(index - 1) - cfaAt(index + 1) + cfaAt(index + 3)) - 3.0 * (cfaAt(index - 2) + cfaAt(index + 2)) + 6.0 * cfaAt(index);
    return sqrf(d);
}

// Step 1: the vertical-horizontal discrimination, zero where the CPU leaves it unset.
[shader("compute")]
[numthreads(8, 8, 1)]
void rcd_vh(uint3 tid : SV_DispatchThreadID)
{
    const uint col = tid.x;
    const uint row = tid.y;
    if (col >= pc.stride || row >= pc.height)
        return;
    const uint index = row * pc.stride + col;
    float result = 0.0;
    if (row >= 4 && row + 4 < pc.height && col >= 4 && col + 4 < pc.width)
    {
        const uint w1 = pc.stride;
        precise float vSum = rcdVerticalHpf(index - w1) + rcdVerticalHpf(index) + rcdVerticalHpf(index + w1);
        precise float hSum = rcdHorizontalHpf(index - 1) + rcdHorizontalHpf(index) + rcdHorizontalHpf(index + 1);
        const float vStat = rawFmax(kRcdEpsSq, vSum);
        const float hStat = rawFmax(kRcdEpsSq, hSum);
        precise float dir = vStat / (vStat + hStat);
        result = dir;
    }
    work[vhOffset() + index] = result;
}

// Step 2: the low-pass filter at red and blue photosites, one slot of the half-width plane a
// thread; slot k of a row holds column 2k or 2k + 1, whichever is red or blue.
[shader("compute")]
[numthreads(8, 8, 1)]
void rcd_lpf(uint3 tid : SV_DispatchThreadID)
{
    const uint slot = tid.x;
    const uint row = tid.y;
    const uint halfStride = pc.stride / 2;
    if (slot >= halfStride || row >= pc.height)
        return;
    const uint col = 2 * slot + (fc(row, 0) & 1u);
    float result = 0.0;
    if (row >= 2 && row + 2 < pc.height && col >= 2 && col + 2 < pc.width)
    {
        const uint i = row * pc.stride + col;
        const uint w1 = pc.stride;
        precise float lpf = cfaAt(i) + 0.5 * (cfaAt(i - w1) + cfaAt(i + w1) + cfaAt(i - 1) + cfaAt(i + 1)) + 0.25 * (cfaAt(i - w1 - 1) + cfaAt(i - w1 + 1) + cfaAt(i + w1 - 1) + cfaAt(i + w1 + 1));
        result = lpf;
    }
    work[lpqOffset() + row * halfStride + slot] = result;
}

bool rcdRedOrBlue(uint row, uint col)
{
    return row >= 4 && row + 4 < pc.height && col >= 4 && col + 4 < pc.width && ((col - (fc(row, 0) & 1u)) & 1u) == 0;
}

// Step 3: green at red and blue photosites.
[shader("compute")]
[numthreads(8, 8, 1)]
void rcd_green(uint3 tid : SV_DispatchThreadID)
{
    const uint col = tid.x;
    const uint row = tid.y;
    if (col >= pc.width || row >= pc.height || !rcdRedOrBlue(row, col))
        return;
    const uint indx = row * pc.stride + col;
    const uint lpindx = indx / 2;
    const uint w1 = pc.stride;
    const uint w2 = 2 * w1;
    const uint w3 = 3 * w1;
    const uint w4 = 4 * w1;
    const float cfai = cfaAt(indx);

    precise float nGrad = kRcdEps + abs(cfaAt(indx - w1) - cfaAt(indx + w1)) + abs(cfai - cfaAt(indx - w2)) + abs(cfaAt(indx - w1) - cfaAt(indx - w3)) + abs(cfaAt(indx - w2) - cfaAt(indx - w4));
    precise float sGrad = kRcdEps + abs(cfaAt(indx + w1) - cfaAt(indx - w1)) + abs(cfai - cfaAt(indx + w2)) + abs(cfaAt(indx + w1) - cfaAt(indx + w3)) + abs(cfaAt(indx + w2) - cfaAt(indx + w4));
    precise float wGrad = kRcdEps + abs(cfaAt(indx - 1) - cfaAt(indx + 1)) + abs(cfai - cfaAt(indx - 2)) + abs(cfaAt(indx - 1) - cfaAt(indx - 3)) + abs(cfaAt(indx - 2) - cfaAt(indx - 4));
    precise float eGrad = kRcdEps + abs(cfaAt(indx + 1) - cfaAt(indx - 1)) + abs(cfai - cfaAt(indx + 2)) + abs(cfaAt(indx + 1) - cfaAt(indx + 3)) + abs(cfaAt(indx + 2) - cfaAt(indx + 4));

    const float lpfi = lpqAt(lpindx);
    precise float twice = lpfi + lpfi;
    precise float nEst = cfaAt(indx - w1) * twice / (kRcdEps + lpfi + lpqAt(lpindx - w1));
    precise float sEst = cfaAt(indx + w1) * twice / (kRcdEps + lpfi + lpqAt(lpindx + w1));
    precise float wEst = cfaAt(indx - 1) * twice / (kRcdEps + lpfi + lpqAt(lpindx - 1));
    precise float eEst = cfaAt(indx + 1) * twice / (kRcdEps + lpfi + lpqAt(lpindx + 1));

    precise float vEst = (sGrad * nEst + nGrad * sEst) / (nGrad + sGrad);
    precise float hEst = (wGrad * eEst + eGrad * wEst) / (eGrad + wGrad);

    const float central = vhAt(indx);
    precise float neighbourhood = 0.25 * (vhAt(indx - w1 - 1) + vhAt(indx - w1 + 1) + vhAt(indx + w1 - 1) + vhAt(indx + w1 + 1));
    precise float centralDist = abs(0.5 - central);
    precise float neighbourDist = abs(0.5 - neighbourhood);
    const float disc = (centralDist < neighbourDist) ? neighbourhood : central;

    work[rgbOffset(1) + indx] = interpolatef(clip01(disc), hEst, vEst);
}

// Step 4.0: the P and Q diagonal high-pass filters at odd columns; slot k of a row holds column
// 2k + 1.
[shader("compute")]
[numthreads(8, 8, 1)]
void rcd_pq(uint3 tid : SV_DispatchThreadID)
{
    const uint slot = tid.x;
    const uint row = tid.y;
    const uint halfStride = pc.stride / 2;
    if (slot >= halfStride || row >= pc.height)
        return;
    const uint col = 2 * slot + 1;
    float p = 0.0;
    float q = 0.0;
    if (row >= 3 && row + 3 < pc.height && col >= 3 && col + 3 < pc.width)
    {
        const uint indx = row * pc.stride + col;
        const uint w1 = pc.stride;
        const uint w2 = 2 * w1;
        const uint w3 = 3 * w1;
        precise float pd = (cfaAt(indx - w3 - 3) - cfaAt(indx - w1 - 1) - cfaAt(indx + w1 + 1) + cfaAt(indx + w3 + 3)) - 3.0 * (cfaAt(indx - w2 - 2) + cfaAt(indx + w2 + 2)) + 6.0 * cfaAt(indx);
        precise float qd = (cfaAt(indx - w3 + 3) - cfaAt(indx - w1 + 1) - cfaAt(indx + w1 - 1) + cfaAt(indx + w3 - 3)) - 3.0 * (cfaAt(indx - w2 + 2) + cfaAt(indx + w2 - 2)) + 6.0 * cfaAt(indx);
        p = sqrf(pd);
        q = sqrf(qd);
    }
    const uint slotIndex = row * halfStride + slot;
    work[pOffset() + slotIndex] = p;
    work[qOffset() + slotIndex] = q;
}

// Step 4.1: the P-Q discrimination at red and blue photosites, over the low-pass filter's slots,
// as the CPU shares their storage.
[shader("compute")]
[numthreads(8, 8, 1)]
void rcd_pqdir(uint3 tid : SV_DispatchThreadID)
{
    const uint slot = tid.x;
    const uint row = tid.y;
    const uint halfStride = pc.stride / 2;
    if (slot >= halfStride || row >= pc.height)
        return;
    const uint col = 2 * slot + (fc(row, 0) & 1u);
    if (!rcdRedOrBlue(row, col))
        return;
    const uint indx = row * pc.stride + col;
    const uint indx2 = indx / 2;
    const uint indx3 = (indx - pc.stride - 1) / 2;
    const uint indx4 = (indx + pc.stride - 1) / 2;
    precise float pSum = work[pOffset() + indx3] + work[pOffset() + indx2] + work[pOffset() + indx4 + 1];
    precise float qSum = work[qOffset() + indx3 + 1] + work[qOffset() + indx2] + work[qOffset() + indx4];
    const float pStat = rawFmax(kRcdEpsSq, pSum);
    const float qStat = rawFmax(kRcdEpsSq, qSum);
    precise float dir = pStat / (pStat + qStat);
    work[lpqOffset() + indx2] = dir;
}

// Step 4.2: blue at red photosites and red at blue ones.
[shader("compute")]
[numthreads(8, 8, 1)]
void rcd_rb(uint3 tid : SV_DispatchThreadID)
{
    const uint col = tid.x;
    const uint row = tid.y;
    if (col >= pc.width || row >= pc.height || !rcdRedOrBlue(row, col))
        return;
    const uint c = 2 - fc(row, col);
    const uint indx = row * pc.stride + col;
    const uint w1 = pc.stride;
    const uint w2 = 2 * w1;
    const uint w3 = 3 * w1;
    const uint pqindx = indx / 2;
    const uint pqindx2 = (indx - w1 - 1) / 2;
    const uint pqindx3 = (indx + w1 - 1) / 2;

    const float central = lpqAt(pqindx);
    precise float neighbourhood = 0.25 * (lpqAt(pqindx2) + lpqAt(pqindx2 + 1) + lpqAt(pqindx3) + lpqAt(pqindx3 + 1));
    precise float centralDist = abs(0.5 - central);
    precise float neighbourDist = abs(0.5 - neighbourhood);
    const float disc = (centralDist < neighbourDist) ? neighbourhood : central;

    const float g = rgbAt(1, indx);
    precise float nwGrad = kRcdEps + abs(rgbAt(c, indx - w1 - 1) - rgbAt(c, indx + w1 + 1)) + abs(rgbAt(c, indx - w1 - 1) - rgbAt(c, indx - w3 - 3)) + abs(g - rgbAt(1, indx - w2 - 2));
    precise float neGrad = kRcdEps + abs(rgbAt(c, indx - w1 + 1) - rgbAt(c, indx + w1 - 1)) + abs(rgbAt(c, indx - w1 + 1) - rgbAt(c, indx - w3 + 3)) + abs(g - rgbAt(1, indx - w2 + 2));
    precise float swGrad = kRcdEps + abs(rgbAt(c, indx - w1 + 1) - rgbAt(c, indx + w1 - 1)) + abs(rgbAt(c, indx + w1 - 1) - rgbAt(c, indx + w3 - 3)) + abs(g - rgbAt(1, indx + w2 - 2));
    precise float seGrad = kRcdEps + abs(rgbAt(c, indx - w1 - 1) - rgbAt(c, indx + w1 + 1)) + abs(rgbAt(c, indx + w1 + 1) - rgbAt(c, indx + w3 + 3)) + abs(g - rgbAt(1, indx + w2 + 2));

    precise float nwEst = rgbAt(c, indx - w1 - 1) - rgbAt(1, indx - w1 - 1);
    precise float neEst = rgbAt(c, indx - w1 + 1) - rgbAt(1, indx - w1 + 1);
    precise float swEst = rgbAt(c, indx + w1 - 1) - rgbAt(1, indx + w1 - 1);
    precise float seEst = rgbAt(c, indx + w1 + 1) - rgbAt(1, indx + w1 + 1);

    precise float pEst = (nwGrad * seEst + seGrad * nwEst) / (nwGrad + seGrad);
    precise float qEst = (neGrad * swEst + swGrad * neEst) / (neGrad + swGrad);

    precise float value = g + interpolatef(clip01(disc), qEst, pEst);
    work[rgbOffset(c) + indx] = value;
}

// Step 4.3: red and blue at green photosites.
[shader("compute")]
[numthreads(8, 8, 1)]
void rcd_rb_green(uint3 tid : SV_DispatchThreadID)
{
    const uint col = tid.x;
    const uint row = tid.y;
    if (col >= pc.width || row >= pc.height)
        return;
    if (!(row >= 4 && row + 4 < pc.height && col >= 4 && col + 4 < pc.width && ((col - (fc(row, 1) & 1u)) & 1u) == 0))
        return;
    const uint indx = row * pc.stride + col;
    const uint w1 = pc.stride;
    const uint w2 = 2 * w1;
    const uint w3 = 3 * w1;

    const float central = vhAt(indx);
    precise float neighbourhood = 0.25 * (vhAt(indx - w1 - 1) + vhAt(indx - w1 + 1) + vhAt(indx + w1 - 1) + vhAt(indx + w1 + 1));
    precise float centralDist = abs(0.5 - central);
    precise float neighbourDist = abs(0.5 - neighbourhood);
    const float disc = (centralDist < neighbourDist) ? neighbourhood : central;

    const float rgb1 = rgbAt(1, indx);
    precise float n1 = kRcdEps + abs(rgb1 - rgbAt(1, indx - w2));
    precise float s1 = kRcdEps + abs(rgb1 - rgbAt(1, indx + w2));
    precise float w1Grad = kRcdEps + abs(rgb1 - rgbAt(1, indx - 2));
    precise float e1 = kRcdEps + abs(rgb1 - rgbAt(1, indx + 2));

    const float rgb1mw1 = rgbAt(1, indx - w1);
    const float rgb1pw1 = rgbAt(1, indx + w1);
    const float rgb1m1 = rgbAt(1, indx - 1);
    const float rgb1p1 = rgbAt(1, indx + 1);

    for (uint c = 0; c <= 2; c += 2)
    {
        precise float snAbs = abs(rgbAt(c, indx - w1) - rgbAt(c, indx + w1));
        precise float ewAbs = abs(rgbAt(c, indx - 1) - rgbAt(c, indx + 1));

        precise float nGrad = n1 + snAbs + abs(rgbAt(c, indx - w1) - rgbAt(c, indx - w3));
        precise float sGrad = s1 + snAbs + abs(rgbAt(c, indx + w1) - rgbAt(c, indx + w3));
        precise float wGrad = w1Grad + ewAbs + abs(rgbAt(c, indx - 1) - rgbAt(c, indx - 3));
        precise float eGrad = e1 + ewAbs + abs(rgbAt(c, indx + 1) - rgbAt(c, indx + 3));

        precise float nEst = rgbAt(c, indx - w1) - rgb1mw1;
        precise float sEst = rgbAt(c, indx + w1) - rgb1pw1;
        precise float wEst = rgbAt(c, indx - 1) - rgb1m1;
        precise float eEst = rgbAt(c, indx + 1) - rgb1p1;

        precise float vEst = (nGrad * sEst + sGrad * nEst) / (nGrad + sGrad);
        precise float hEst = (eGrad * wEst + wGrad * eEst) / (eGrad + wGrad);

        precise float value = rgb1 + interpolatef(clip01(disc), hEst, vEst);
        work[rgbOffset(c) + indx] = value;
    }
}

// The output window, RGBA with an alpha of 1: PPG's result, RCD's inside the 9 photosites it
// leaves to PPG along the buffer's edges, or the source itself without a CFA, grey for one
// channel. A pixel outside the buffer is black.
[shader("compute")]
[numthreads(8, 8, 1)]
void demosaic_write(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.outWidth || tid.y >= pc.outHeight)
        return;
    const int x = pc.outX + int(tid.x);
    const int y = pc.outY + int(tid.y);
    float3 rgb = float3(0.0, 0.0, 0.0);
    if (pc.mode == kModePassThrough)
    {
        if (x >= pc.srcX1 && x < pc.srcX2 && y >= pc.srcY1 && y < pc.srcY2)
        {
            const uint base = pc.srcOffset + uint(y - pc.srcY1) * pc.srcStride + uint(x - pc.srcX1) * pc.srcComps;
            [ForceUnroll]
            for (uint c = 0; c < 3; ++c)
                rgb[c] = srcBuf[base + (pc.srcComps >= 3 ? c : 0u)];
        }
    }
    else
    {
        const int row = pc.bufY2 - 1 - y;
        const int col = x - pc.bufX1;
        if (insideBuffer(row, col))
        {
            const uint urow = uint(row);
            const uint ucol = uint(col);
            const bool rcd = (pc.mode == kModeRcd) && (pc.width >= 2 * kRcdBorder) && (pc.height >= 2 * kRcdBorder)
                && (urow >= kRcdMargin) && (urow + kRcdMargin < pc.height) && (ucol >= kRcdMargin) && (ucol + kRcdMargin < pc.width);
            if (rcd)
            {
                const uint index = urow * pc.stride + ucol;
                [ForceUnroll]
                for (uint c = 0; c < 3; ++c)
                {
                    precise float scaled = pc.scaler * rawFmax(0.0, rgbAt(c, index));
                    rgb[c] = scaled;
                }
            }
            else
            {
                const uint base = ppgRbOffset() + 4 * (urow * pc.width + ucol);
                rgb = float3(work[base], work[base + 1], work[base + 2]);
            }
        }
    }
    const uint pixel = pc.dstOffset + tid.y * pc.dstStride + tid.x * 4;
    dstBuf[pixel] = rgb.x;
    dstBuf[pixel + 1] = rgb.y;
    dstBuf[pixel + 2] = rgb.z;
    dstBuf[pixel + 3] = 1.0;
}
