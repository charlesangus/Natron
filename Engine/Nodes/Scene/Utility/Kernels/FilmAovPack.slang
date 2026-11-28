import FilmIdsTable;

// KernelFilmAovPackParams in UtilityRenderBuffer.cpp mirrors this. It writes a window of a frame's depth or prim
// ids, from its picks, as HydraRender::PlaneWindow lays out such a window: every component of a pixel holds its
// value, and a pixel that hit nothing, or past the frame or the picks' window, holds +infinity for depth and 0 for
// a prim id.
struct FilmAovPackParams
{
    // Columns 2 and 3 of the inverse projection, rows 0 to 3: what takes a window depth back to the camera.
    float4 zColumn;
    float4 wColumn;
    uint renderWidth;
    uint renderHeight;
    int picksX;
    int picksY;
    uint picksWidth;
    uint picksHeight;
    int x1;
    int y1;
    uint width;
    uint height;
    uint nComps;
    uint outOffset;
    uint outRowStride;
    // 0 writes the depth, 1 the prim ids.
    uint mode;
    uint padding0;
    uint padding1;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> picks;
// For the prim ids: the id table FilmIds.slang filled, and each entry's prim path hash.
[[vk::binding(1, 0)]] StructuredBuffer<uint> table;
[[vk::binding(2, 0)]] StructuredBuffer<uint> hashes;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> pixels;
[[vk::push_constant]] ConstantBuffer<FilmAovPackParams> params;

static const uint kInfinityBits = 0x7F800000u;

[shader("compute")]
[numthreads(8, 8, 1)]
void utilityFilmAovPack(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.width || id.y >= params.height)
        return;

    const int fx = params.x1 + int(id.x);
    const int fy = params.y1 + int(id.y);
    const int px = fx - params.picksX;
    const int py = fy - params.picksY;
    const bool inside = fx >= 0 && fy >= 0 && uint(fx) < params.renderWidth && uint(fy) < params.renderHeight && px >= 0 && py >= 0 && uint(px) < params.picksWidth && uint(py) < params.picksHeight;
    const uint n = params.picksWidth * params.picksHeight;
    const uint at = inside ? uint(py) * params.picksWidth + uint(px) : 0u;
    const bool hit = inside && (picks[kPickHitPlane * n + at] != 0u);
    uint word = (params.mode == 0u) ? kInfinityBits : 0u;

    if (hit && (params.mode == 0u)) {
        const float window = asfloat(picks[kPickDepthPlane * n + at]);

        if (window < 1.0) {
            const float ndcX = (float(fx) + 0.5) / float(params.renderWidth) * 2.0 - 1.0;
            const float ndcY = (float(fy) + 0.5) / float(params.renderHeight) * 2.0 - 1.0;
            const float4 ndc = float4(ndcX, ndcY, window * 2.0 - 1.0, 1.0);

            word = asuint(-dot(ndc, params.zColumn) / dot(ndc, params.wColumn));
        }
    } else if (hit) {
        const uint prim = picks[kPickPrimIdPlane * n + at];

        if (int(prim) >= 0) {
            const uint entry = findIdEntry(table, idTag(prim, picks[kPickInstanceIdPlane * n + at]));

            word = (entry != 0u) ? hashes[entry - 1u] : 0u;
        }
    }

    const uint base = params.outOffset + id.y * params.outRowStride + id.x * params.nComps;

    for (uint c = 0; c < params.nComps; ++c)
        pixels[base + c] = word;
}
