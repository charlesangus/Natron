// Glow's weights and its rotated frame, after Engine/Nodes/Filter/Glow.cpp: glow_weight writes what
// the glow map is multiplied by (the mask times the hue key), glow_resample turns an image into or
// out of the frame the stretched bloom runs in. Every buffer is tightly packed row by row, `channels`
// floats per pixel, except the mask. GlowFrameGpuParams in Engine/Nodes/Filter/Glow.cpp mirrors
// FrameParams.

StructuredBuffer<uint> srcBuf;
StructuredBuffer<uint> maskBuf;
RWStructuredBuffer<uint> dstBuf;

struct FrameParams
{
    // The source, srcWidth x srcHeight pixels from (srcX1, srcY1), srcStride floats per row.
    int srcX1;
    int srcY1;
    uint srcWidth;
    uint srcHeight;
    uint srcStride;
    // The rect written.
    int dstX1;
    int dstY1;
    uint dstWidth;
    uint dstHeight;
    uint channels;
    uint flags;
    // glow_resample: the row-major matrix taking a destination pixel position to the source's.
    float m00;
    float m01;
    float m10;
    float m11;
    // glow_weight: the mask's view; its pixel (x, y) holds the mask at maskOffset +
    // (y - maskY1) * maskStride + (x - maskX1) * maskComps + maskComponent.
    int maskX1;
    int maskY1;
    uint maskWidth;
    uint maskHeight;
    uint maskOffset;
    uint maskStride;
    uint maskComps;
    uint maskComponent;
    // The picked hue in degrees, and per channel two bits: its colour (0 red, 1 green, 2 blue) or 3.
    float hue;
    uint colourBits;
};

[[vk::push_constant]] ConstantBuffer<FrameParams> params;

static const uint kHue = 1;
static const uint kMask = 2;
static const uint kMaskZero = 4;

// HSVTool's rgb_to_hsv hue in degrees: 0 for a grey.
float hueOf(float r, float g, float b)
{
    const float minv = min(min(r, g), b);
    const float maxv = max(max(r, g), b);
    const float delta = maxv - minv;
    if (maxv == 0.0 || delta == 0.0)
        return 0.0;
    float h;
    if (r == maxv)
        h = (g - b) / delta;
    else if (g == maxv)
        h = 2.0 + (b - r) / delta;
    else
        h = 4.0 + (r - g) / delta;
    h *= 1.0 / 6.0;
    if (h < 0.0)
        h += 1.0;
    return h * 360.0;
}

float hueKey(float h, float pick)
{
    float d = abs(h - pick);
    d = min(d, 360.0 - d);
    return max(0.0, 1.0 - d / 50.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void glow_weight(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.dstWidth || tid.y >= params.dstHeight)
        return;
    float w = 1.0;
    if ((params.flags & kMaskZero) != 0)
    {
        w = 0.0;
    }
    else if ((params.flags & kMask) != 0)
    {
        const int x = params.dstX1 + int(tid.x);
        const int y = params.dstY1 + int(tid.y);
        const bool inside = x >= params.maskX1 && y >= params.maskY1 && x < params.maskX1 + int(params.maskWidth) && y < params.maskY1 + int(params.maskHeight);
        w = inside ? asfloat(maskBuf[params.maskOffset + uint(y - params.maskY1) * params.maskStride + uint(x - params.maskX1) * params.maskComps + params.maskComponent]) : 0.0;
    }
    if ((params.flags & kHue) != 0)
    {
        float rgb[3] = { 0.0, 0.0, 0.0 };
        const uint i = tid.y * params.srcStride + tid.x * params.channels;
        for (uint ch = 0; ch < min(params.channels, 4u); ++ch)
        {
            const uint bit = (params.colourBits >> (2 * ch)) & 3u;
            if (bit < 3)
                rgb[bit] = asfloat(srcBuf[i + ch]);
        }
        w *= hueKey(hueOf(rgb[0], rgb[1], rgb[2]), params.hue);
    }
    dstBuf[tid.y * params.dstWidth + tid.x] = asuint(w);
}

float sourceAt(int x, int y, uint ch)
{
    if (x < 0 || y < 0 || x >= int(params.srcWidth) || y >= int(params.srcHeight))
        return 0.0;
    return asfloat(srcBuf[uint(y) * params.srcStride + uint(x) * params.channels + ch]);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void glow_resample(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.dstWidth || tid.y >= params.dstHeight)
        return;
    const float x = float(params.dstX1 + int(tid.x)) + 0.5;
    const float y = float(params.dstY1 + int(tid.y)) + 0.5;
    // The weights and the pixels read come from the absolute position alone, never from where
    // the source starts, so any window over the same pixels gives the same sample.
    const float u = (params.m00 * x + params.m01 * y) - 0.5;
    const float v = (params.m10 * x + params.m11 * y) - 0.5;
    const float fx = floor(u);
    const float fy = floor(v);
    const int x0 = int(fx) - params.srcX1;
    const int y0 = int(fy) - params.srcY1;
    const float wx = u - fx;
    const float wy = v - fy;
    const uint o = (tid.y * params.dstWidth + tid.x) * params.channels;
    for (uint ch = 0; ch < params.channels; ++ch)
    {
        const float s = (1.0 - wy) * ((1.0 - wx) * sourceAt(x0, y0, ch) + wx * sourceAt(x0 + 1, y0, ch))
                      + wy * ((1.0 - wx) * sourceAt(x0, y0 + 1, ch) + wx * sourceAt(x0 + 1, y0 + 1, ch));
        dstBuf[o + ch] = asuint(s);
    }
}
