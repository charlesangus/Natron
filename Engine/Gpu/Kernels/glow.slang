// Glow's own passes, after the workers of Engine/Nodes/Filter/Glow.cpp: the saturated glow map of the
// source, and the combination of the source, the map and its bloom (bloom.slang's) into the output.
// The source and the map are tightly packed over the CPU's buffer, domWidth x domHeight pixels from
// (domX1, domY1), `channels` floats per pixel; the bloom is tightly packed over the output rect.
// GlowGpuParams in Engine/Nodes/Filter/Glow.cpp mirrors GlowParams.

StructuredBuffer<uint> srcBuf;
RWStructuredBuffer<uint> mapBuf;
StructuredBuffer<uint> accBuf;
RWStructuredBuffer<uint> outBuf;

struct GlowParams
{
    int domX1;
    int domY1;
    uint domWidth;
    uint domHeight;
    // combine: the rect written, whose first pixel is at outOffset of outBuf.
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint outOffset;
    uint outStride;
    uint accStride;
    uint channels;
    // kGlowGpu* in Glow.cpp.
    uint flags;
    float threshold;
    float knee;
    // The kind of channel c (0 unprocessed, 1 colour, 2 alpha) in bits 2c and 2c + 1.
    uint kinds;
    // Per channel: the luma weight, the gain (tint times brightness), the saturation and 1 / gamma.
    // Channel 3 is only ever alpha, which has no saturation, so its slot carries the mix.
    float weight0;
    float weight1;
    float weight2;
    float weight3;
    float gain0;
    float gain1;
    float gain2;
    float gain3;
    float saturation0;
    float saturation1;
    float saturation2;
    // The output's blend with the source: source + mix * (output - source).
    float mix;
    float invGamma0;
    float invGamma1;
    float invGamma2;
    float invGamma3;
};

[[vk::push_constant]] ConstantBuffer<GlowParams> params;

static const uint kPreview = 1;
static const uint kGlowOnly = 2;
static const uint kScreen = 4;
static const uint kAddInput = 8;
// accBuf holds one weight per pixel of the domain, which the map is multiplied by, in glow_map and
// in the preview.
static const uint kWeighted = 32;

static const uint kKindColour = 1;
static const uint kKindAlpha = 2;

float weightOf(uint c)
{
    return c == 0 ? params.weight0 : (c == 1 ? params.weight1 : (c == 2 ? params.weight2 : params.weight3));
}

float gainOf(uint c)
{
    return c == 0 ? params.gain0 : (c == 1 ? params.gain1 : (c == 2 ? params.gain2 : params.gain3));
}

float saturationOf(uint c)
{
    return c == 0 ? params.saturation0 : (c == 1 ? params.saturation1 : (c == 2 ? params.saturation2 : 1.0));
}

float invGammaOf(uint c)
{
    return c == 0 ? params.invGamma0 : (c == 1 ? params.invGamma1 : (c == 2 ? params.invGamma2 : params.invGamma3));
}

uint kindOf(uint c)
{
    return (params.kinds >> (2 * c)) & 3u;
}

// softKnee() in Glow.cpp.
float softKnee(float x, float k)
{
    if (x <= 0.0)
        return 0.0;
    if (k > 0.0 && x < k)
        return x * x / (2.0 * k);
    return x - 0.5 * k;
}

// The scale of the map, c(L) / L, and c, from the pixel at index i of srcBuf; mapScale() in Glow.cpp.
void mapScale(uint i, uint n, out float s, out float c)
{
    float l = 0.0;
    for (uint ch = 0; ch < n; ++ch)
        l += weightOf(ch) * asfloat(srcBuf[i + ch]);
    c = softKnee(l - params.threshold, params.knee);
    s = (l > 0.0 && c > 0.0) ? c / l : 0.0;
}

// The map over the buffer: each colour channel saturated about its luminance c, alpha as it is,
// zero on the unprocessed channels.
[shader("compute")]
[numthreads(8, 8, 1)]
void glow_map(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.domWidth || tid.y >= params.domHeight)
        return;
    const uint n = min(params.channels, 4u);
    const uint i = (tid.y * params.domWidth + tid.x) * params.channels;
    float s;
    float c;
    mapScale(i, n, s, c);
    const float w = (params.flags & kWeighted) != 0 ? asfloat(accBuf[tid.y * params.domWidth + tid.x]) : 1.0;
    for (uint ch = 0; ch < n; ++ch)
    {
        const uint kind = kindOf(ch);
        const float m = asfloat(srcBuf[i + ch]) * s;
        float g = 0.0;
        if (kind == kKindColour)
            g = c + saturationOf(ch) * (m - c);
        else if (kind == kKindAlpha)
            g = m;
        mapBuf[i + ch] = asuint(g * w);
    }
}

// The output: the ungraded map (preview), the glow alone, or the source plus or screened with the
// glow. The glow is the bloom, plus the map with addInput, gamma-corrected and multiplied by the
// gain, per channel. A processed alpha is clamped to [0, 1] outside the preview. Each processed
// channel is then blended with the source by the mix. Outside the buffer everything reads zero.
[shader("compute")]
[numthreads(8, 8, 1)]
void glow_combine(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = min(params.channels, 4u);
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    const uint dst = params.outOffset + tid.y * params.outStride + tid.x * params.channels;
    const uint a = tid.y * params.accStride + tid.x * params.channels;
    const bool inside = x >= params.domX1 && y >= params.domY1 && x < params.domX1 + int(params.domWidth) && y < params.domY1 + int(params.domHeight);
    const uint i = inside ? (uint(y - params.domY1) * params.domWidth + uint(x - params.domX1)) * params.channels : 0u;
    float s = 0.0;
    float c = 0.0;
    float w = 1.0;
    if (inside && (params.flags & kPreview) != 0)
    {
        mapScale(i, n, s, c);
        if ((params.flags & kWeighted) != 0)
            w = asfloat(accBuf[uint(y - params.domY1) * params.domWidth + uint(x - params.domX1)]);
    }
    for (uint ch = 0; ch < n; ++ch)
    {
        const uint kind = kindOf(ch);
        const float src = inside ? asfloat(srcBuf[i + ch]) : 0.0;
        if (kind == 0)
        {
            outBuf[dst + ch] = asuint(src);
            continue;
        }
        float v;
        if ((params.flags & kPreview) != 0)
        {
            v = (src * s) * w;
        }
        else
        {
            float glow = asfloat(accBuf[a + ch]);
            if ((params.flags & kAddInput) != 0 && inside)
                glow += asfloat(mapBuf[i + ch]);
            const float invGamma = invGammaOf(ch);
            if (invGamma != 1.0 && glow > 0.0)
                glow = pow(glow, invGamma);
            glow *= gainOf(ch);
            if ((params.flags & kGlowOnly) != 0)
                v = glow;
            else if ((params.flags & kScreen) != 0)
                v = (src <= 1.0 || glow <= 1.0) ? src + glow - src * glow : max(src, glow);
            else
                v = src + glow;
            if (kind == kKindAlpha)
                v = clamp(v, 0.0, 1.0);
        }
        // A mix of 1 keeps v bit-exact rather than as src + (v - src).
        if (params.mix != 1.0)
            v = src + params.mix * (v - src);
        outBuf[dst + ch] = asuint(v);
    }
}
