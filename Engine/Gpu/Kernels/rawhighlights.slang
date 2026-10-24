/*
 * The maths are a port of darktable's highlights OpenCL kernels, data/kernels/basic.cl
 * (highlights_1f_clip, highlights_4f_clip, highlights_1f_lch_bayer, highlights_1f_lch_xtrans) at
 * DARKTABLE_REF release-5.6.2, Copyright (C) darktable developers, distributed under the GNU GPL
 * version 3 or later, kept in step with RawHighlights.cpp.
 */

import pixelwrap;
import rawlayout;

static const float kSqrt3 = asfloat(0x3fddb3d7u);
static const float kSqrt12 = asfloat(0x405db3d7u);
static const float kFltMax = asfloat(0x7f7fffffu);

// std::min(a, b) and std::max(a, b), which keep a unless b is strictly beyond it.
float stdMin(float a, float b)
{
    return b < a ? b : a;
}

float stdMax(float a, float b)
{
    return a < b ? b : a;
}

// Clip Highlights: the first channel of a mosaic, or red, green and blue without a CFA, at most
// the threshold.
struct HighlightsClipOp : IPixelOp
{
    RawLayout layout;
    float clip;

    // Parameter block, in words: the layout (16), clip.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        layout.load(params, 0);
        clip = asfloat(params[16]);
    }

    float4 apply(float4 v, int2 xy)
    {
        const uint n = wrap.nComps;
        if (layout.hasCfa())
        {
            const uint bit = wrapChannelBit(n, 0);
            v[bit] = rawFmin(v[bit], clip);
            return v;
        }
        for (uint c = 0; c < n && c < 3; ++c)
        {
            const uint bit = wrapChannelBit(n, c);
            v[bit] = rawFmin(clip, v[bit]);
        }
        return v;
    }
}

// Reconstruct in LCh on a Bayer or X-Trans mosaic. Photosites are counted as rows and columns of
// the source's region of definition from its top-left one, as the CPU node's buffer counts them,
// and read through the wrap's source view, which must hold every one the window reads.
struct HighlightsLChOp : IPixelOp
{
    RawLayout layout;
    float clip;
    int rodX1;
    int rodY1;
    int rodX2;
    int rodY2;

    // Parameter block, in words: the layout (16), clip, rodX1, rodY1, rodX2, rodY2.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        layout.load(params, 0);
        clip = asfloat(params[16]);
        rodX1 = int(params[17]);
        rodY1 = int(params[18]);
        rodX2 = int(params[19]);
        rodY2 = int(params[20]);
    }

    float at(int row, int col)
    {
        return wrapReadSource(int2(rodX1 + col, rodY2 - 1 - row))[wrapChannelBit(wrap.nComps, 0)];
    }

    uint cfaAt(int row, int col)
    {
        return layout.colourAt(rodX1 + col, rodY2 - 1 - row);
    }

    float bayer(int j, int i)
    {
        const float v = at(j, i);
        if (i == rodX2 - rodX1 - 1 || j == rodY2 - rodY1 - 1)
            return stdMin(clip, v);
        bool clipped = false;
        float R = 0.0;
        float Gmin = kFltMax;
        float Gmax = -kFltMax;
        float B = 0.0;
        for (int jj = 0; jj <= 1; ++jj)
        {
            for (int ii = 0; ii <= 1; ++ii)
            {
                const float val = at(j + jj, i + ii);
                clipped = clipped || (val > clip);
                const uint c = cfaAt(j + jj, i + ii);
                if (c == 0)
                    R = val;
                else if (c == 1)
                {
                    Gmin = stdMin(Gmin, val);
                    Gmax = stdMax(Gmax, val);
                }
                else if (c == 2)
                    B = val;
            }
        }
        if (!clipped)
            return v;
        return reconstruct(R, Gmax, B, stdMin(R, clip), stdMin(Gmin, clip), stdMin(B, clip), cfaAt(j, i));
    }

    float xtrans(int j, int i)
    {
        const float v = at(j, i);
        const int width = rodX2 - rodX1;
        const int height = rodY2 - rodY1;
        if (i < 2 || i > width - 3 || j < 2 || j > height - 3)
            return stdMin(clip, v);
        bool clipped = v > clip;
        if (!clipped)
        {
            bool ring = false;
            for (int ii = i - 2; ii <= i; ++ii)
                ring = ring || (at(j - 1, ii) > clip) || (at(j, ii) > clip) || (at(j + 1, ii) > clip);
            clipped = ring;
            if (clipped)
            {
                for (int offsetJ = -2; offsetJ <= 0; ++offsetJ)
                {
                    for (int offsetI = -2; offsetI <= 0; ++offsetI)
                    {
                        if (clipped)
                        {
                            clipped = false;
                            for (int jj = offsetJ; jj <= offsetJ + 2; ++jj)
                            {
                                for (int ii = offsetI; ii <= offsetI + 2; ++ii)
                                    clipped = clipped || (at(j + jj, i + ii) > clip);
                            }
                        }
                    }
                }
            }
        }
        if (!clipped)
            return v;
        float3 mean = float3(0.0, 0.0, 0.0);
        float3 rgbMax = float3(-kFltMax, -kFltMax, -kFltMax);
        int3 cnt = int3(0, 0, 0);
        for (int jj = -1; jj <= 1; ++jj)
        {
            for (int ii = -1; ii <= 1; ++ii)
            {
                const float val = at(j + jj, i + ii);
                const uint c = cfaAt(j + jj, i + ii);
                if (c > 2)
                    continue;
                precise float sum = mean[c] + val;
                mean[c] = sum;
                cnt[c] += 1;
                rgbMax[c] = stdMax(rgbMax[c], val);
            }
        }
        precise float ro = mean[0] / float(cnt[0]);
        precise float go = mean[1] / float(cnt[1]);
        precise float bo = mean[2] / float(cnt[2]);
        return reconstruct(rgbMax[0], rgbMax[1], rgbMax[2], stdMin(ro, clip), stdMin(go, clip), stdMin(bo, clip), cfaAt(j, i));
    }

    // Keeps the lightness of (R, G, B) and scales its chroma to that of the clipped (Ro, Go, Bo).
    float reconstruct(float R, float G, float B, float Ro, float Go, float Bo, uint colour)
    {
        precise float L = (R + G + B) / 3.0;
        precise float C = kSqrt3 * (R - G);
        precise float H = 2.0 * B - G - R;
        precise float Co = kSqrt3 * (Ro - Go);
        precise float Ho = 2.0 * Bo - Go - Ro;
        if (R != G && G != B)
        {
            precise float ratio = sqrt((Co * Co + Ho * Ho) / (C * C + H * H));
            C = C * ratio;
            H = H * ratio;
        }
        precise float r = L - H / 6.0 + C / kSqrt12;
        precise float g = L - H / 6.0 - C / kSqrt12;
        precise float b = L + H / 3.0;
        return colour == 0 ? r : (colour == 1 ? g : b);
    }

    float4 apply(float4 v, int2 xy)
    {
        if (xy.x < rodX1 || xy.x >= rodX2 || xy.y < rodY1 || xy.y >= rodY2)
            return float4(0.0, 0.0, 0.0, 0.0);
        const int j = rodY2 - 1 - xy.y;
        const int i = xy.x - rodX1;
        v[wrapChannelBit(wrap.nComps, 0)] = layout.period == 2 ? bayer(j, i) : xtrans(j, i);
        return v;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rawhighlights_clip(uint3 tid : SV_DispatchThreadID)
{
    HighlightsClipOp op;
    runWrapped<HighlightsClipOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rawhighlights_lch(uint3 tid : SV_DispatchThreadID)
{
    HighlightsLChOp op;
    runWrapped<HighlightsLChOp>(op, tid);
}
