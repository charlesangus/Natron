import pixelwrap;

// VectorBlur::renderOp per pixel (Gather::pixel() in VectorBlur.cpp, step for step). The wrap's
// push block carries the Source, which is both the image sampled and the image blended under the
// mask and mix, the destination and the mask; the parameter block in wrapAux carries the settings
// and the views of the UV and depth layers, whose pixels are in uvBuf and depthBuf.
// VectorBlurGpuParams in VectorBlur.cpp mirrors it.

[[vk::binding(4, 0)]] StructuredBuffer<float> uvBuf;
[[vk::binding(5, 0)]] StructuredBuffer<float> depthBuf;

static const uint kVectorBlurUV = 1;
static const uint kVectorBlurOcclusion = 2;
static const uint kVectorBlurInverseDepth = 4;
static const uint kVectorBlurTriangle = 8;

static const float kDepthTolerance = 0.01;

struct VectorBlur
{
    uint flags;
    int minSamples;
    int maxSamples;
    float offset;
    float fx;
    float fy;
    float maxLength;
    uint uvOffset;
    uint uvStride;
    int uvX1;
    int uvY1;
    int uvX2;
    int uvY2;
    uint uvComps;
    int uComp;
    int vComp;
    uint depthOffset;
    uint depthStride;
    int depthX1;
    int depthY1;
    int depthX2;
    int depthY2;
    uint depthComps;
    int depthComp;
}

VectorBlur vectorBlurLoad(StructuredBuffer<uint> params)
{
    VectorBlur b;
    b.flags = params[0];
    b.minSamples = int(params[1]);
    b.maxSamples = int(params[2]);
    b.offset = asfloat(params[3]);
    b.fx = asfloat(params[4]);
    b.fy = asfloat(params[5]);
    b.maxLength = asfloat(params[6]);
    b.uvOffset = params[7];
    b.uvStride = params[8];
    b.uvX1 = int(params[9]);
    b.uvY1 = int(params[10]);
    b.uvX2 = int(params[11]);
    b.uvY2 = int(params[12]);
    b.uvComps = params[13];
    b.uComp = int(params[14]);
    b.vComp = int(params[15]);
    b.depthOffset = params[16];
    b.depthStride = params[17];
    b.depthX1 = int(params[18]);
    b.depthY1 = int(params[19]);
    b.depthX2 = int(params[20]);
    b.depthY2 = int(params[21]);
    b.depthComps = params[22];
    b.depthComp = int(params[23]);
    return b;
}

float vectorBlurUV(VectorBlur b, int2 p, int c)
{
    if ((b.flags & kVectorBlurUV) == 0 || c < 0 || uint(c) >= b.uvComps || !wrapInside(p, b.uvX1, b.uvY1, b.uvX2, b.uvY2))
        return 0.0;
    return uvBuf[b.uvOffset + uint(p.y - b.uvY1) * b.uvStride + uint(p.x - b.uvX1) * b.uvComps + uint(c)];
}

// Gather::vector(): the vector at p in pixels, shortened to maxLength, in xy, its length in z;
// false for none.
bool vectorBlurVector(VectorBlur b, int2 p, out float3 d)
{
    d = float3(0.0, 0.0, 0.0);
    const float u = vectorBlurUV(b, p, b.uComp);
    const float v = vectorBlurUV(b, p, b.vComp);
    if (!isfinite(u) || !isfinite(v))
        return false;
    float ex = u * b.fx;
    float ey = v * b.fy;
    float len = sqrt(ex * ex + ey * ey);
    if (!(len > 0.0) || !isfinite(len))
        return false;
    if (len > b.maxLength)
    {
        const float k = b.maxLength / len;
        ex *= k;
        ey *= k;
        len = b.maxLength;
        if (!(len > 0.0))
            return false;
    }
    d = float3(ex, ey, len);
    return true;
}

// Gather::distance(): far for a non-finite or non-positive depth.
float vectorBlurDistance(VectorBlur b, int2 p, out bool far)
{
    float z = 0.0;
    if (b.depthComp >= 0 && uint(b.depthComp) < b.depthComps && wrapInside(p, b.depthX1, b.depthY1, b.depthX2, b.depthY2))
        z = depthBuf[b.depthOffset + uint(p.y - b.depthY1) * b.depthStride + uint(p.x - b.depthX1) * b.depthComps + uint(b.depthComp)];
    far = !isfinite(z) || !(z > 0.0);
    if (far)
        return 0.0;
    return ((b.flags & kVectorBlurInverseDepth) != 0) ? (1.0 / z) : z;
}

// Gather::reaches(): whether q's own streak passes within one pixel of c.
bool vectorBlurReaches(VectorBlur b, int2 q, float2 c)
{
    float3 e;
    if (!vectorBlurVector(b, q, e))
        return false;
    const float ax = (float(q.x) + 0.5) - b.offset * e.x;
    const float ay = (float(q.y) + 0.5) - b.offset * e.y;
    const float px = c.x - ax;
    const float py = c.y - ay;
    const float h = min(1.0, max(0.0, (px * e.x + py * e.y) / (e.x * e.x + e.y * e.y)));
    const float rx = px - h * e.x;
    const float ry = py - h * e.y;
    return (rx * rx + ry * ry) <= 1.0;
}

// Gather::tap(): bilinear, black outside the Source.
float4 vectorBlurTap(float sx, float sy)
{
    const float gx = sx - 0.5;
    const float gy = sy - 0.5;
    const float x0f = floor(gx);
    const float y0f = floor(gy);
    const float ax = gx - x0f;
    const float ay = gy - y0f;
    const int x0 = int(x0f);
    const int y0 = int(y0f);
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    v += ((1.0 - ax) * (1.0 - ay)) * wrapReadSource(int2(x0, y0));
    v += (ax * (1.0 - ay)) * wrapReadSource(int2(x0 + 1, y0));
    v += ((1.0 - ax) * ay) * wrapReadSource(int2(x0, y0 + 1));
    v += (ax * ay) * wrapReadSource(int2(x0 + 1, y0 + 1));
    return v;
}

struct VectorBlurOp : IPixelOp
{
    VectorBlur b;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        b = vectorBlurLoad(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        float3 d;
        if (!vectorBlurVector(b, xy, d))
            return v;
        const int n = min(b.maxSamples, max(b.minSamples, int(ceil(d.z))));
        const float2 c = float2(float(xy.x) + 0.5, float(xy.y) + 0.5);
        const bool occlusion = (b.flags & kVectorBlurOcclusion) != 0;
        bool centreFar = false;
        float centreDistance = 0.0;
        if (occlusion)
            centreDistance = vectorBlurDistance(b, xy, centreFar);
        const bool triangle = (b.flags & kVectorBlurTriangle) != 0;
        float4 acc = float4(0.0, 0.0, 0.0, 0.0);
        float weights = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float u = (float(i) + 0.5) / float(n);
            const float t = u - b.offset;
            const float sx = c.x + t * d.x;
            const float sy = c.y + t * d.y;
            if (occlusion)
            {
                const int2 q = int2(int(floor(sx)), int(floor(sy)));
                bool sampleFar = false;
                const float sampleDistance = vectorBlurDistance(b, q, sampleFar);
                const bool nearer = !sampleFar && (centreFar || (sampleDistance < centreDistance * (1.0 - kDepthTolerance)));
                if (nearer && !vectorBlurReaches(b, q, c))
                    continue;
            }
            const float w = triangle ? (1.0 - abs(2.0 * u - 1.0)) : 1.0;
            acc += w * vectorBlurTap(sx, sy);
            weights += w;
        }
        if (!(weights > 0.0))
            return v;
        return acc / weights;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vectorblur_main(uint3 tid : SV_DispatchThreadID)
{
    VectorBlurOp op;
    runWrapped(op, tid);
}
