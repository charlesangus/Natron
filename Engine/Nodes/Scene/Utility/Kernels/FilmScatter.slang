import UtilityTypes;

// KernelFilmScatterParams in UtilityRenderBuffer.cpp mirrors this. A batch is a run of whole rows of
// the film starting at (x, y), its pixels in the order the ray generation made its rays.
struct FilmScatterParams
{
    uint filmWidth;
    uint filmHeight;
    int x;
    int y;
    uint width;
    uint count;
    uint padding0;
    uint padding1;
};

// The geometry array comes first in the hit ids, so the first count values tell hit from miss.
[[vk::binding(0, 0)]] StructuredBuffer<uint> hitIds;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> color;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint4> film;
[[vk::push_constant]] ConstantBuffer<FilmScatterParams> params;

// A miss leaves the film's clear value, as the host film does.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityFilmScatter(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.count || hitIds[kHitGeometry * params.count + id.x] == kNoHit)
        return;

    const int fx = params.x + int(id.x % params.width);
    const int fy = params.y + int(id.x / params.width);

    if (fx < 0 || fy < 0 || uint(fx) >= params.filmWidth || uint(fy) >= params.filmHeight)
        return;

    film[uint(fy) * params.filmWidth + uint(fx)] = color[id.x];
}
