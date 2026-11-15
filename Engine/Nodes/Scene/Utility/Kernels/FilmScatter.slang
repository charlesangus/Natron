import UtilityTypes;

// KernelFilmScatterParams in UtilityRenderBuffer.cpp mirrors this. A batch is a run of whole rows of
// the film starting at (x, y), its pixels in the order the ray generation made its rays, one ray each.
struct FilmScatterParams
{
    uint4 clearValue;
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
[[vk::binding(1, 0)]] StructuredBuffer<float4> color;
[[vk::binding(2, 0)]] StructuredBuffer<float> weights;
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> film;
[[vk::binding(4, 0)]] RWStructuredBuffer<float4> accumulation;
[[vk::push_constant]] ConstantBuffer<FilmScatterParams> params;

// Adds each ray's sample to its pixel's weighted sum, a miss as the clear value as on the host, and
// writes the pixel's mean. A pixel no sample has weight for yet keeps what it holds.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityFilmScatter(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.count)
        return;
    const int fx = params.x + int(id.x % params.width);
    const int fy = params.y + int(id.x / params.width);
    if (fx < 0 || fy < 0 || uint(fx) >= params.filmWidth || uint(fy) >= params.filmHeight)
        return;
    const uint pixel = uint(fy) * params.filmWidth + uint(fx);
    const uint weightAt = params.filmWidth * params.filmHeight + pixel;
    const float weight = weights[id.x];
    const float4 sample = hitIds[kHitGeometry * params.count + id.x] == kNoHit ? asfloat(params.clearValue) : color[id.x];
    const float4 sum = accumulation[pixel] + weight * sample;
    const float total = accumulation[weightAt].x + weight;
    accumulation[pixel] = sum;
    accumulation[weightAt] = float4(total, 0.0, 0.0, 0.0);
    if (total > 0.0)
        film[pixel] = sum / total;
}
