/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/demosaicing/vng.c: vng_interpolate() and _vng_lininterpolate(), which darktable adapted
 * from dcraw 9.20 by Dave Coffin, Copyright (C) 2010-2026 darktable developers, distributed under
 * the GNU GPL version 3 or later, with the arithmetic of the CPU port in
 * Engine/Nodes/Raw/RawDemosaicVng.cpp.
 */

import rawlayout;

StructuredBuffer<float> srcBuf;
// The linear interpolation then the gradient pass's result, 4 floats a photosite each.
RWStructuredBuffer<float> work;
RWStructuredBuffer<float> dstBuf;

// RawVngGpuPush in Engine/Nodes/Raw/RawGpuParams.h mirrors it. The layout is RawLayoutGpu, held as
// vectors so that the push block's array rules do not matter, and describes the pattern as the
// buffer sees it: origin (0, 0) at its top-left photosite, rows counted downwards, VNG4's second
// green as colour 3. The source and output rects are in buffer rows and columns.
struct VngParams
{
    int4 head;
    uint4 cells0;
    uint4 cells1;
    uint4 cells2;
    uint width;
    uint height;
    uint srcOffset;
    uint srcStride;
    uint srcComps;
    int srcCol1;
    int srcRow1;
    int srcCol2;
    int srcRow2;
    uint dstOffset;
    uint dstStride;
    int outCol;
    int outRow;
    uint outWidth;
    uint outHeight;
    uint padding;
};
[[vk::push_constant]] ConstantBuffer<VngParams> pc;

// dcraw's VNG terms: two photosites (y1, x1) and (y2, x2) whose difference, times the weight,
// adds to each of the 8 gradients set in the mask.
static const int kTerms[384] = {
    -2, -2, +0, -1, 1, 0x01, -2, -2, +0, +0, 2, 0x01, -2, -1, -1, +0, 1, 0x01, -2, -1, +0, -1, 1, 0x02,
    -2, -1, +0, +0, 1, 0x03, -2, -1, +0, +1, 2, 0x01, -2, +0, +0, -1, 1, 0x06, -2, +0, +0, +0, 2, 0x02,
    -2, +0, +0, +1, 1, 0x03, -2, +1, -1, +0, 1, 0x04, -2, +1, +0, -1, 2, 0x04, -2, +1, +0, +0, 1, 0x06,
    -2, +1, +0, +1, 1, 0x02, -2, +2, +0, +0, 2, 0x04, -2, +2, +0, +1, 1, 0x04, -1, -2, -1, +0, 1, 0x80,
    -1, -2, +0, -1, 1, 0x01, -1, -2, +1, -1, 1, 0x01, -1, -2, +1, +0, 2, 0x01, -1, -1, -1, +1, 1, 0x88,
    -1, -1, +1, -2, 1, 0x40, -1, -1, +1, -1, 1, 0x22, -1, -1, +1, +0, 1, 0x33, -1, -1, +1, +1, 2, 0x11,
    -1, +0, -1, +2, 1, 0x08, -1, +0, +0, -1, 1, 0x44, -1, +0, +0, +1, 1, 0x11, -1, +0, +1, -2, 2, 0x40,
    -1, +0, +1, -1, 1, 0x66, -1, +0, +1, +0, 2, 0x22, -1, +0, +1, +1, 1, 0x33, -1, +0, +1, +2, 2, 0x10,
    -1, +1, +1, -1, 2, 0x44, -1, +1, +1, +0, 1, 0x66, -1, +1, +1, +1, 1, 0x22, -1, +1, +1, +2, 1, 0x10,
    -1, +2, +0, +1, 1, 0x04, -1, +2, +1, +0, 2, 0x04, -1, +2, +1, +1, 1, 0x04, +0, -2, +0, +0, 2, 0x80,
    +0, -1, +0, +1, 2, 0x88, +0, -1, +1, -2, 1, 0x40, +0, -1, +1, +0, 1, 0x11, +0, -1, +2, -2, 1, 0x40,
    +0, -1, +2, -1, 1, 0x20, +0, -1, +2, +0, 1, 0x30, +0, -1, +2, +1, 2, 0x10, +0, +0, +0, +2, 2, 0x08,
    +0, +0, +2, -2, 2, 0x40, +0, +0, +2, -1, 1, 0x60, +0, +0, +2, +0, 2, 0x20, +0, +0, +2, +1, 1, 0x30,
    +0, +0, +2, +2, 2, 0x10, +0, +1, +1, +0, 1, 0x44, +0, +1, +1, +2, 1, 0x10, +0, +1, +2, -1, 2, 0x40,
    +0, +1, +2, +0, 1, 0x60, +0, +1, +2, +1, 1, 0x20, +0, +1, +2, +2, 1, 0x10, +1, -2, +1, +0, 1, 0x80,
    +1, -1, +1, +1, 1, 0x88, +1, +0, +1, +2, 1, 0x08, +1, +0, +2, -1, 1, 0x40, +1, +0, +2, +1, 1, 0x10,
};

// The neighbour each gradient points to, as (y, x).
static const int kChood[16] = { -1, -1, -1, 0, -1, +1, 0, +1, +1, +1, +1, 0, +1, -1, 0, -1 };

RawLayout loadLayout()
{
    RawLayout l;
    l.period = pc.head.x;
    l.originX = pc.head.y;
    l.originY = pc.head.z;
    l.cells[0] = pc.cells0.x;
    l.cells[1] = pc.cells0.y;
    l.cells[2] = pc.cells0.z;
    l.cells[3] = pc.cells0.w;
    l.cells[4] = pc.cells1.x;
    l.cells[5] = pc.cells1.y;
    l.cells[6] = pc.cells1.z;
    l.cells[7] = pc.cells1.w;
    l.cells[8] = pc.cells2.x;
    return l;
}

// X-Trans interpolates three colours, VNG4 four, its two greens apart until the output.
uint colourCount()
{
    return pc.head.x == 6 ? 3u : 4u;
}

// Photosite (row, col) of the buffer, zero outside the source image.
float mosaic(int row, int col)
{
    if (col < pc.srcCol1 || col >= pc.srcCol2 || row < pc.srcRow1 || row >= pc.srcRow2)
        return 0.0;
    return srcBuf[pc.srcOffset + uint(pc.srcRow2 - 1 - row) * pc.srcStride + uint(col - pc.srcCol1) * pc.srcComps];
}

uint pixelBase(uint plane, int row, int col)
{
    return plane * 4u * pc.width * pc.height + 4u * (uint(row) * pc.width + uint(col));
}

float linearAt(int row, int col, uint c)
{
    return work[pixelBase(0, row, col) + c];
}

// _mm_max_ps(v, 0) as darktable's dt_vector_clipneg() uses it: a NaN becomes 0.
float clipNeg(float v)
{
    return v > 0.0 ? v : 0.0;
}

// RawKernels::vngLinearInterpolate() with a border larger than the buffer: on the outermost
// photosites each missing colour is the mean of its 3x3 neighbours of that colour, inside them
// the weighted mean of _vng_lininterpolate()'s lookup, summed in the same order.
[shader("compute")]
[numthreads(8, 8, 1)]
void vng_linear(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.width || tid.y >= pc.height)
        return;
    const int col = int(tid.x);
    const int row = int(tid.y);
    const int width = int(pc.width);
    const int height = int(pc.height);
    RawLayout layout = loadLayout();
    const uint colours = colourCount();
    const uint f = layout.colourAt(col, row);
    const float centre = mosaic(row, col);
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    const bool border = (row == 0) || (row == height - 1) || (col == 0) || (col == width - 1);
    if (border)
    {
        float sum[4] = { 0.0, 0.0, 0.0, 0.0 };
        uint count[4] = { 0, 0, 0, 0 };
        for (int y = row - 1; y != row + 2; ++y)
        {
            for (int x = col - 1; x != col + 2; ++x)
            {
                if (y >= 0 && x >= 0 && y < height && x < width)
                {
                    const uint c = layout.colourAt(x, y);
                    precise float s = sum[c] + rawFmax(0.0, mosaic(y, x));
                    sum[c] = s;
                    count[c] += 1u;
                }
            }
        }
        for (uint c = 0; c < colours; ++c)
        {
            if (c != f && count[c] != 0u)
            {
                precise float mean = sum[c] / float(count[c]);
                result[c] = mean;
            }
            else
                result[c] = rawFmax(0.0, centre);
        }
    }
    else
    {
        float sum[4] = { 0.0, 0.0, 0.0, 0.0 };
        uint weights[4] = { 0, 0, 0, 0 };
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                const uint c = layout.colourAt(col + x, row + y);
                if (c == f)
                    continue;
                const uint weight = 1u << ((y == 0 ? 1u : 0u) + (x == 0 ? 1u : 0u));
                precise float term = rawFmax(0.0, mosaic(row + y, col + x)) * float(weight);
                precise float s = sum[c] + term;
                sum[c] = s;
                weights[c] += weight;
            }
        }
        for (uint c = 0; c < colours; ++c)
        {
            if (c == f)
                continue;
            // The CPU divides 0 by 0 for a colour missing from the 3x3 block, which the clip turns
            // into 0; a device's division by 0 is undefined.
            if (weights[c] != 0u)
            {
                precise float mean = sum[c] / float(weights[c]);
                result[c] = mean;
            }
        }
        result[f] = centre;
        [ForceUnroll]
        for (uint c = 0; c < 4; ++c)
            result[c] = clipNeg(result[c]);
    }
    const uint base = pixelBase(0, row, col);
    [ForceUnroll]
    for (uint c = 0; c < 4; ++c)
        work[base + c] = result[c];
}

// The gradient pass of RawKernels::vng(), on the photosites at least 2 in from every edge of a
// buffer more than 4 wide and high; every other photosite keeps the linear interpolation. Each
// reads its neighbours as the linear pass left them, as darktable's ring of rows ensures.
[shader("compute")]
[numthreads(8, 8, 1)]
void vng_gradient(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.width || tid.y >= pc.height)
        return;
    const int col = int(tid.x);
    const int row = int(tid.y);
    const int width = int(pc.width);
    const int height = int(pc.height);
    const uint base = pixelBase(0, row, col);
    float4 pix = float4(work[base], work[base + 1], work[base + 2], work[base + 3]);
    float4 result = pix;
    const bool interior = (width > 4) && (height > 4) && (row >= 2) && (row < height - 2) && (col >= 2) && (col < width - 2);
    if (interior)
    {
        RawLayout layout = loadLayout();
        const uint colours = colourCount();
        const uint colour = layout.colourAt(col, row);
        float gval[8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        for (int t = 0; t < 64; ++t)
        {
            const int y1 = kTerms[6 * t];
            const int x1 = kTerms[6 * t + 1];
            const int y2 = kTerms[6 * t + 2];
            const int x2 = kTerms[6 * t + 3];
            const int weight = kTerms[6 * t + 4];
            const uint grads = uint(kTerms[6 * t + 5]);
            const uint c = layout.colourAt(col + x1, row + y1);
            if (layout.colourAt(col + x2, row + y2) != c)
                continue;
            const int diag = ((layout.colourAt(col + 1, row) == c) && (layout.colourAt(col, row + 1) == c)) ? 2 : 1;
            if ((abs(y1 - y2) == diag) && (abs(x1 - x2) == diag))
                continue;
            precise float diff = abs(linearAt(row + y1, col + x1, c) - linearAt(row + y2, col + x2, c)) * float(weight);
            [ForceUnroll]
            for (uint g = 0; g < 8; ++g)
            {
                if ((grads & (1u << g)) != 0u)
                {
                    precise float s = gval[g] + diff;
                    gval[g] = s;
                }
            }
        }
        float gmin = gval[0];
        float gmax = gval[0];
        [ForceUnroll]
        for (uint g = 1; g < 8; ++g)
        {
            if (gmin > gval[g])
                gmin = gval[g];
            if (gmax < gval[g])
                gmax = gval[g];
        }
        if (gmax != 0.0)
        {
            precise float half = gmax * 0.5;
            precise float thold = gmin + half;
            float sum[4] = { 0.0, 0.0, 0.0, 0.0 };
            int num = 0;
            for (uint g = 0; g < 8; ++g)
            {
                if (gval[g] <= thold)
                {
                    const int y = kChood[2 * g];
                    const int x = kChood[2 * g + 1];
                    const bool twoAway = (layout.colourAt(col + x, row + y) != colour) && (layout.colourAt(col + 2 * x, row + 2 * y) == colour);
                    for (uint c = 0; c < colours; ++c)
                    {
                        if (c == colour && twoAway)
                        {
                            precise float mid = (pix[c] + linearAt(row + 2 * y, col + 2 * x, c)) * 0.5;
                            precise float s = sum[c] + mid;
                            sum[c] = s;
                        }
                        else
                        {
                            precise float s = sum[c] + linearAt(row + y, col + x, c);
                            sum[c] = s;
                        }
                    }
                    ++num;
                }
            }
            for (uint c = 0; c < colours; ++c)
            {
                if (c != colour)
                {
                    precise float tot = pix[colour] + (sum[c] - sum[colour]) / float(num);
                    result[c] = tot;
                }
                else
                    result[c] = pix[colour];
            }
        }
    }
    const uint out = pixelBase(1, row, col);
    [ForceUnroll]
    for (uint c = 0; c < 4; ++c)
        work[out + c] = result[c];
}

// The output window, RGBA with an alpha of 1: VNG4's two greens averaged, then negative values
// clipped. A pixel outside the buffer is black.
[shader("compute")]
[numthreads(8, 8, 1)]
void vng_write(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.outWidth || tid.y >= pc.outHeight)
        return;
    const int col = pc.outCol + int(tid.x);
    const int row = pc.outRow - int(tid.y);
    float3 rgb = float3(0.0, 0.0, 0.0);
    if (row >= 0 && col >= 0 && row < int(pc.height) && col < int(pc.width))
    {
        const uint base = pixelBase(1, row, col);
        float green = work[base + 1];
        if (colourCount() == 4u)
        {
            precise float mixed = 0.5 * (green + work[base + 3]);
            green = mixed;
        }
        rgb = float3(clipNeg(work[base]), clipNeg(green), clipNeg(work[base + 2]));
    }
    const uint pixel = pc.dstOffset + tid.y * pc.dstStride + tid.x * 4;
    dstBuf[pixel] = rgb.x;
    dstBuf[pixel + 1] = rgb.y;
    dstBuf[pixel + 2] = rgb.z;
    dstBuf[pixel + 3] = 1.0;
}
