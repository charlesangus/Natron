/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/rawdenoise.c and the C path of dwt_denoise() in src/common/dwt.c, Copyright (C)
 * 2011-2026 darktable developers, distributed under the GNU GPL version 3 or later, with the
 * arithmetic of the CPU port in Engine/Nodes/Raw/RawDenoiseKernel.cpp.
 */

StructuredBuffer<float> srcBuf;
// One colour's plane, then the vertical pass's result, then the accumulated thresholded details,
// planeFloats floats each.
RWStructuredBuffer<float> work;
RWStructuredBuffer<float> dstBuf;

// RawDenoiseGpuPush in Engine/Nodes/Raw/RawGpuParams.h mirrors it. Rows and columns are the
// buffer's, counted downwards from the top-left photosite of the region the CPU reads; the source
// and output rects are in them too. pattern packs the X-Trans colours of that buffer, two bits a
// cell, row by row.
struct DenoiseParams
{
    uint width;
    uint height;
    uint planeWidth;
    uint planeHeight;
    uint planeFloats;
    uint channel;
    uint rowParity;
    uint colParity;
    uint pattern0;
    uint pattern1;
    uint pattern2;
    uint level;
    float thold;
    uint last;
    uint mode;
    uint srcOffset;
    uint srcStride;
    uint srcComps;
    int srcCol1;
    int srcRow1;
    int srcCol2;
    int srcRow2;
    uint dstOffset;
    uint dstStride;
    uint dstComps;
    int outCol;
    int outRow;
    uint outWidth;
    uint outHeight;
    uint padding0;
    uint padding1;
    uint padding2;
};
[[vk::push_constant]] ConstantBuffer<DenoiseParams> pc;

static const uint kModeBayer = 0;
static const uint kModeXTrans = 1;
static const uint kModeMosaic = 2;
static const uint kModeIdentity = 3;

// Component c of photosite (row, col), zero outside the source image.
float sourceAt(int row, int col, uint c)
{
    if (col < pc.srcCol1 || col >= pc.srcCol2 || row < pc.srcRow1 || row >= pc.srcRow2 || c >= pc.srcComps)
        return 0.0;
    return srcBuf[pc.srcOffset + uint(pc.srcRow2 - 1 - row) * pc.srcStride + uint(col - pc.srcCol1) * pc.srcComps + c];
}

float mosaic(int row, int col)
{
    return sourceAt(row, col, 0u);
}

// std::sqrt(std::max(0.0f, v)): a NaN becomes 0.
float vstransform(float v)
{
    return sqrt(v > 0.0 ? v : 0.0);
}

uint fc(int row, int col)
{
    const uint i = uint(row % 6) * 6u + uint(col % 6);
    const uint word = (i < 16u) ? pc.pattern0 : ((i < 32u) ? pc.pattern1 : pc.pattern2);
    return (word >> ((i % 16u) * 2u)) & 3u;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rawdenoise_bayer_extract(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.planeWidth || tid.y >= pc.planeHeight)
        return;
    const int row = int(pc.rowParity + 2u * tid.y);
    const int col = int(pc.colParity + 2u * tid.x);
    work[tid.y * pc.planeWidth + tid.x] = vstransform(mosaic(row, col));
}

// rawDenoiseXTrans()'s extraction runs down the rows writing each photosite of the colour into
// its neighbours, the rows above and below included, a later write replacing an earlier one. Each
// thread finds the last write of that sequence to its own photosite, from the row below back to
// the row above; without one it keeps what the previous colour's denoising left in the plane, as
// the CPU's shared buffer does.
[shader("compute")]
[numthreads(8, 8, 1)]
void rawdenoise_xtrans_extract(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.width || tid.y >= pc.height)
        return;
    const int C = int(tid.x);
    const int R = int(tid.y);
    const int W = int(pc.width);
    const int H = int(pc.height);
    const uint c = pc.channel;
    const uint index = tid.y * pc.width + tid.x;

    for (int r = min(R + 1, H - 1); r >= max(R - 1, 0); --r)
    {
        const int dr = R - r;

        const uint lastColour = fc(r, W - 1);
        if (c != 1u && lastColour == c)
        {
            if ((dr == 0 && (C == W - 2 || C == W - 1)) || (dr == -1 && C == W - 1))
            {
                work[index] = vstransform(mosaic(r, W - 1));
                return;
            }
        }
        else if (lastColour != c && dr == 0 && C == W - 1)
        {
            int srcRow = r;
            int srcCol = W - 1;
            if (fc(r, W - 2) == c)
            {
                srcCol = W - 2;
            }
            else if (r > 1 && fc(r - 1, W - 1) == c)
            {
                srcRow = r - 1;
            }
            else if (r > 1 && fc(r - 1, W - 2) == c)
            {
                srcRow = r - 1;
                srcCol = W - 2;
            }
            work[index] = vstransform(mosaic(srcRow, srcCol));
            return;
        }

        const uint firstColour = fc(r, 0);
        if (firstColour != c && dr == 0 && C == 0)
        {
            int srcRow = r;
            int srcCol = 0;
            if (r > 1 && fc(r - 1, 0) == c)
            {
                srcRow = r - 1;
            }
            else if (fc(r, 1) == c)
            {
                srcCol = 1;
            }
            else if (r > 1 && fc(r - 1, 1) == c)
            {
                srcRow = r - 1;
                srcCol = 1;
            }
            work[index] = vstransform(mosaic(srcRow, srcCol));
            return;
        }

        if (c == 1u)
        {
            if (dr == 0)
            {
                if (C <= W - 2 && fc(r, C) == 1u)
                {
                    work[index] = vstransform(mosaic(r, C));
                    return;
                }
                if (C >= 1 && fc(r, C - 1) == 1u)
                {
                    work[index] = vstransform(mosaic(r, C - 1));
                    return;
                }
            }
            else if (dr == 1 && C <= W - 2 && fc(r, C) == 1u)
            {
                work[index] = vstransform(mosaic(r, C));
                return;
            }
        }
        else
        {
            for (int k = C + 1; k >= C - 1; --k)
            {
                if (k >= 1 && k <= W - 2 && fc(r, k) == c)
                {
                    work[index] = vstransform(mosaic(r, k));
                    return;
                }
            }
            if (firstColour == c && ((dr == 0 && C == 0) || (dr == -1 && C <= 1)))
            {
                work[index] = vstransform(mosaic(r, 0));
                return;
            }
        }
    }

    if (R == 0 || R == H - 1)
        work[index] = 0.5;
    else if (c == 0u)
        work[index] = 0.0;
}

// dwtDenoiseVert(): the scale is capped at the plane's last row, and rows past either edge
// reflect back into it.
[shader("compute")]
[numthreads(8, 8, 1)]
void rawdenoise_vert(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.planeWidth || tid.y >= pc.planeHeight)
        return;
    const int row = int(tid.y);
    const int height = int(pc.planeHeight);
    const int vscale = min(1 << int(pc.level), height - 1);
    const int above = abs(row - vscale);
    const int below = (row + vscale < height) ? (row + vscale) : (2 * (height - 1) - (row + vscale));
    const uint w = pc.planeWidth;
    const float centre = work[tid.y * w + tid.x];
    const float up = work[uint(above) * w + tid.x];
    const float down = work[uint(below) * w + tid.x];
    precise float sum = 2.0 * centre + up + down;
    work[pc.planeFloats + tid.y * w + tid.x] = sum;
}

// dwtDenoiseHoriz(): its three column ranges are one reflection rule, the same on every column.
// The soft threshold uses std::max() and std::min()'s comparisons.
[shader("compute")]
[numthreads(8, 8, 1)]
void rawdenoise_horiz(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.planeWidth || tid.y >= pc.planeHeight)
        return;
    const int col = int(tid.x);
    const int width = int(pc.planeWidth);
    const int hscale = min(1 << int(pc.level), width - 1);
    const int left = (col >= hscale) ? (col - hscale) : (hscale - col);
    const int right = (col + hscale < width) ? (col + hscale) : (2 * (width - 1) - (col + hscale));
    const uint rowBase = tid.y * pc.planeWidth;
    const uint coarse = pc.planeFloats + rowBase;
    const uint detail = rowBase + tid.x;
    const uint accum = 2u * pc.planeFloats + rowBase + tid.x;

    precise float hat = (2.0 * work[coarse + tid.x] + work[coarse + uint(left)] + work[coarse + uint(right)]) * 0.0625;
    precise float diff = work[detail] - hat;
    precise float above = diff - pc.thold;
    precise float below = diff + pc.thold;
    precise float shrunk = (above < 0.0 ? 0.0 : above) + (0.0 < below ? 0.0 : below);
    precise float total = (pc.level == 0u ? 0.0 : work[accum]) + shrunk;
    work[accum] = total;
    if (pc.last != 0u)
    {
        precise float restored = hat + total;
        work[detail] = restored;
    }
    else
    {
        work[detail] = hat;
    }
}

// One colour's photosites of the output squared back from its plane; photosites outside the
// region the CPU reads are zero, as the CPU writes them. The mosaic mode copies the region as the
// CPU does below X-Trans's minimum size, and the identity mode copies the source whole.
[shader("compute")]
[numthreads(8, 8, 1)]
void rawdenoise_write(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.outWidth || tid.y >= pc.outHeight)
        return;
    const int col = pc.outCol + int(tid.x);
    const int row = pc.outRow - int(tid.y);
    const uint pixel = pc.dstOffset + tid.y * pc.dstStride + tid.x * pc.dstComps;
    const bool inside = row >= 0 && col >= 0 && row < int(pc.height) && col < int(pc.width);

    if (pc.mode == kModeIdentity)
    {
        for (uint c = 0u; c < pc.dstComps; ++c)
            dstBuf[pixel + c] = sourceAt(row, col, c);
        return;
    }
    float value = 0.0;
    if (inside)
    {
        if (pc.mode == kModeMosaic)
        {
            value = mosaic(row, col);
        }
        else if (pc.mode == kModeBayer)
        {
            if ((uint(row) & 1u) != pc.rowParity || (uint(col) & 1u) != pc.colParity)
                return;
            const float d = work[uint(row >> 1) * pc.planeWidth + uint(col >> 1)];
            precise float squared = d * d;
            value = squared;
        }
        else
        {
            if (fc(row, col) != pc.channel)
                return;
            const float d = work[uint(row) * pc.planeWidth + uint(col)];
            precise float squared = d * d;
            value = squared;
        }
    }
    dstBuf[pixel] = value;
    for (uint c = 1u; c < pc.dstComps; ++c)
        dstBuf[pixel + c] = 0.0;
}
