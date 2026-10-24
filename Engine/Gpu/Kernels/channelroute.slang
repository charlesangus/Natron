// One pass of a ChannelRoute: the destination components a source feeds, copied from their source
// channels, or the route's constants. Pixels travel as their bits, so a copy is exact whatever it
// holds. ChannelRouteGpuParams in Engine/Nodes/Channel/ChannelCopy.cpp mirrors ChannelRouteParams.

// The destination's offset addresses the output origin. Threads inside [validX1, validX2) x
// [validY1, validY2) read the source, whose offset addresses the pixel thread (validX1, validY1)
// reads; the others read zero. selectC is, for destination component C, the source channel it
// copies, kSkip to leave the component to another pass, or kConstant to write constantC.
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
    uint validX1;
    uint validY1;
    uint validX2;
    uint validY2;
    uint select0;
    uint select1;
    uint select2;
    uint select3;
    uint constant0;
    uint constant1;
    uint constant2;
    uint constant3;
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
    const bool inside = tid.x >= routeParams.validX1 && tid.x < routeParams.validX2
                        && tid.y >= routeParams.validY1 && tid.y < routeParams.validY2;
    uint src = 0;
    if (inside)
        src = routeParams.srcOffset + (tid.y - routeParams.validY1) * routeParams.srcStride
              + (tid.x - routeParams.validX1) * routeParams.srcComps;
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
