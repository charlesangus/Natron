import edgesample;

// ChannelRouteGpuParams in Engine/Nodes/Channel/ChannelCopy.cpp mirrors ChannelRouteParams, and
// its kRouteSkip and kRouteConstant mirror kSkip and kConstant. Pixels travel as their bits, so
// that a copy is exact whatever it holds.
struct ChannelRouteParams
{
    uint width;
    uint height;
    uint dstOffset;
    uint dstStride;
    uint dstComps;
    uint srcOffset;
    uint srcStride;
    uint srcComps;
    // The source's bounds from the dispatch's origin; srcOffset is its first pixel.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint select0;
    uint select1;
    uint select2;
    uint select3;
    uint constant0;
    uint constant1;
    uint constant2;
    uint constant3;
    // How the source reads past its bounds, edgesample's kEdgeStretch or kEdgeBlack.
    uint edgeMode;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> routeSrc;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> routeDst;
[[vk::push_constant]] ConstantBuffer<ChannelRouteParams> routeParams;

static const uint kSkip = 0xFFFFFFFFu;
static const uint kConstant = 0xFFFFFFFEu;

uint routeSelect(uint c)
{
    if (c == 0)
        return routeParams.select0;
    if (c == 1)
        return routeParams.select1;
    if (c == 2)
        return routeParams.select2;
    return routeParams.select3;
}

uint routeConstant(uint c)
{
    if (c == 0)
        return routeParams.constant0;
    if (c == 1)
        return routeParams.constant1;
    if (c == 2)
        return routeParams.constant2;
    return routeParams.constant3;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void channelroute(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= routeParams.width || tid.y >= routeParams.height)
        return;
    const uint dst = routeParams.dstOffset + tid.y * routeParams.dstStride + tid.x * routeParams.dstComps;
    int2 at;
    const bool inside = edgePixel(int2(tid.xy), int4(routeParams.srcX1, routeParams.srcY1, routeParams.srcX2, routeParams.srcY2), routeParams.edgeMode, at);
    uint src = 0;
    if (inside)
        src = routeParams.srcOffset + uint(at.y - routeParams.srcY1) * routeParams.srcStride
              + uint(at.x - routeParams.srcX1) * routeParams.srcComps;
    for (uint c = 0; c < routeParams.dstComps && c < 4; ++c)
    {
        const uint s = routeSelect(c);
        if (s == kSkip)
            continue;
        if (s == kConstant)
            routeDst[dst + c] = routeConstant(c);
        else
            routeDst[dst + c] = inside ? routeSrc[src + s] : 0u;
    }
}
