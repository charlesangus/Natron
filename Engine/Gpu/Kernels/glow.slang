// Glow's own passes, after the workers of Engine/Nodes/Filter/Glow.cpp: the graded glow map of the
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
    float saturation;
    float brightness;
    float invGamma;
    uint pad0;
    uint pad1;
    // Per channel: the luma weight, the tint, and the kind (0 unprocessed, 1 colour, 2 alpha).
    float weight0;
    float weight1;
    float weight2;
    float weight3;
    float tint0;
    float tint1;
    float tint2;
    float tint3;
    uint kind0;
    uint kind1;
    uint kind2;
    uint kind3;
};

[[vk::push_constant]] ConstantBuffer<GlowParams> params;

static const uint kPreview = 1;
static const uint kGlowOnly = 2;
static const uint kScreen = 4;
static const uint kAddInput = 8;
static const uint kGamma = 16;

static const uint kKindColour = 1;
static const uint kKindAlpha = 2;

float weightOf(uint c)
{
    return c == 0 ? params.weight0 : (c == 1 ? params.weight1 : (c == 2 ? params.weight2 : params.weight3));
}

float tintOf(uint c)
{
    return c == 0 ? params.tint0 : (c == 1 ? params.tint1 : (c == 2 ? params.tint2 : params.tint3));
}

uint kindOf(uint c)
{
    return c == 0 ? params.kind0 : (c == 1 ? params.kind1 : (c == 2 ? params.kind2 : params.kind3));
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

// The graded map over the buffer: saturation about its luminance c, tint and brightness on colour,
// brightness alone on alpha, zero on the unprocessed channels.
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
    for (uint ch = 0; ch < n; ++ch)
    {
        const uint kind = kindOf(ch);
        const float m = asfloat(srcBuf[i + ch]) * s;
        float g = 0.0;
        if (kind == kKindColour)
            g = params.brightness * tintOf(ch) * (c + params.saturation * (m - c));
        else if (kind == kKindAlpha)
            g = params.brightness * m;
        mapBuf[i + ch] = asuint(g);
    }
}

// The output: the ungraded map (preview), the glow alone, or the source plus or screened with the
// glow. The glow is the bloom, plus the graded map with addInput, gamma-corrected. A processed
// alpha is clamped to [0, 1] outside the preview. Outside the buffer everything reads zero.
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
    if (inside && (params.flags & kPreview) != 0)
        mapScale(i, n, s, c);
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
            v = src * s;
        }
        else
        {
            float glow = asfloat(accBuf[a + ch]);
            if ((params.flags & kAddInput) != 0 && inside)
                glow += asfloat(mapBuf[i + ch]);
            if ((params.flags & kGamma) != 0 && glow > 0.0)
                glow = pow(glow, params.invGamma);
            if ((params.flags & kGlowOnly) != 0)
                v = glow;
            else if ((params.flags & kScreen) != 0)
                v = (src <= 1.0 || glow <= 1.0) ? src + glow - src * glow : max(src, glow);
            else
                v = src + glow;
            if (kind == kKindAlpha)
                v = clamp(v, 0.0, 1.0);
        }
        outBuf[dst + ch] = asuint(v);
    }
}
