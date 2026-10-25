// EdgeBlur's mix, after mixWorker() in Engine/Nodes/Filter/EdgeBlur.cpp: the source mixed with its
// blur by the edge weight of the matte. The blur is blur.slang's or blur_lines.slang's and the
// edges are EdgeDetect's kernels, each written into a tightly packed buffer over the output.
// EdgeBlurGpuParams in Engine/Nodes/Filter/EdgeBlur.cpp mirrors EdgeBlurParams.

// The source image.
StructuredBuffer<uint> srcBuf;
// The blurred source, outWidth x outHeight pixels of `channels` floats.
StructuredBuffer<uint> blurBuf;
// The edges, outWidth x outHeight pixels of `edgeChannels` floats; bound but unread without them.
StructuredBuffer<uint> edgeBuf;
// The rect of outWidth x outHeight pixels at (outX1, outY1), whose first pixel is at dstOffset.
RWStructuredBuffer<uint> outBuf;

struct EdgeBlurParams
{
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint dstOffset;
    uint dstStride;
    uint channels;
    // A bit per component the node processes; the others are the source's.
    uint processed;
    // The source pixels that exist, the first at srcOffset; empty when there are none, which read
    // as zero.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    uint edgeChannels;
    // The component of the edges that holds the matte's.
    uint edgeComp;
    uint hasEdge;
    // 1 / Edge Mult.
    float exponent;
};
[[vk::push_constant]] ConstantBuffer<EdgeBlurParams> params;

// EdgeBlur::edgeWeight().
float edgeWeight(float e)
{
    if (!(e > 0.0))
        return 0.0;
    return min(1.0, pow(e, params.exponent));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void edgeblur_mix(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = min(params.channels, 4u);
    const uint dst = params.dstOffset + tid.y * params.dstStride + tid.x * params.channels;
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    const bool inSource = x >= params.srcX1 && x < params.srcX2 && y >= params.srcY1 && y < params.srcY2;
    const uint src = inSource ? params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.channels : 0u;
    const uint pixel = tid.y * params.outWidth + tid.x;
    float w = 0.0;
    if (params.hasEdge != 0u)
        w = edgeWeight(asfloat(edgeBuf[pixel * params.edgeChannels + params.edgeComp]));
    for (uint c = 0; c < n; ++c)
    {
        const float s = inSource ? asfloat(srcBuf[src + c]) : 0.0;
        float o = s;
        if ((params.processed & (1u << c)) != 0u && w > 0.0)
            o = s + (asfloat(blurBuf[pixel * params.channels + c]) - s) * w;
        outBuf[dst + c] = asuint(o);
    }
}
