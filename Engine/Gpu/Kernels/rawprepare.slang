/*
 * The maths are a port of darktable's rawprepare and white balance OpenCL kernels,
 * data/kernels/basic.cl (rawprepare_1f, rawprepare_4f, whitebalance_1f, whitebalance_1f_xtrans,
 * whitebalance_4f) at DARKTABLE_REF release-5.6.2, Copyright (C) darktable developers, distributed
 * under the GNU GPL version 3 or later, kept in step with RawPrepare.cpp and RawTemperature.cpp.
 */

import pixelwrap;
import rawlayout;

// RawPrepare: each photosite less the black level of its cell of the 2x2 block at the CFA origin
// (of its channel without a CFA), over the white level less that black level. The source view is
// moved so that the kept area starts at the output's origin, and the layout is the cropped one, so
// the cell of output pixel (x, y) is that of the source photosite it shows.
struct PrepareOp : IPixelOp
{
    RawLayout layout;
    float4 sub;
    float4 div;
    uint hasCfa;

    // Parameter block, in words: the layout (16), sub (4), div (4), hasCfa.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        layout.load(params, 0);
        sub = wrapParamFloat4(params, 16);
        div = wrapParamFloat4(params, 20);
        hasCfa = params[24];
    }

    float4 apply(float4 v, int2 xy)
    {
        const uint n = wrap.nComps;
        const uint cell = layout.blackCell(xy.x, xy.y);
        for (uint c = 0; c < n && c < 4; ++c)
        {
            const uint bit = wrapChannelBit(n, c);
            const uint id = hasCfa != 0 ? cell : min(c, 3u);
            precise float scaled = (v[bit] - sub[id]) / div[id];
            v[bit] = scaled;
        }
        return v;
    }
}

// RawTemperature: on a mosaic every channel times the multiplier of the photosite's colour;
// without a CFA, red, green and blue times theirs when the pixel has them.
struct TemperatureOp : IPixelOp
{
    RawLayout layout;
    float3 coeffs;

    // Parameter block, in words: the layout (16), coeffs (3).
    [mutating] void load(StructuredBuffer<uint> params)
    {
        layout.load(params, 0);
        coeffs = float3(asfloat(params[16]), asfloat(params[17]), asfloat(params[18]));
    }

    float4 apply(float4 v, int2 xy)
    {
        const uint n = wrap.nComps;
        if (layout.hasCfa())
        {
            const float coeff = coeffs[layout.colourAt(xy.x, xy.y)];
            for (uint c = 0; c < n && c < 4; ++c)
            {
                const uint bit = wrapChannelBit(n, c);
                precise float scaled = v[bit] * coeff;
                v[bit] = scaled;
            }
            return v;
        }
        if (n >= 3)
        {
            [ForceUnroll]
            for (uint c = 0; c < 3; ++c)
            {
                precise float scaled = v[c] * coeffs[c];
                v[c] = scaled;
            }
        }
        return v;
    }
}

// A test op over the layout at word 0: with word 16 at 0 it returns the pixel as it is, otherwise
// the colour of its photosite in every channel.
struct RawLayoutProbe : IPixelOp
{
    RawLayout layout;
    uint mode;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        layout.load(params, 0);
        mode = params[16];
    }

    float4 apply(float4 v, int2 xy)
    {
        if (mode == 0)
            return v;
        return float4(float(layout.colourAt(xy.x, xy.y)));
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rawlayout_probe(uint3 tid : SV_DispatchThreadID)
{
    RawLayoutProbe op;
    runWrapped<RawLayoutProbe>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rawprepare_wrapped(uint3 tid : SV_DispatchThreadID)
{
    PrepareOp op;
    runWrapped<PrepareOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rawtemperature_wrapped(uint3 tid : SV_DispatchThreadID)
{
    TemperatureOp op;
    runWrapped<TemperatureOp>(op, tid);
}
