// KernelFilmClearParams in UtilityRenderBuffer.cpp mirrors this. Pixels are copied as bits, so a clear
// value or a shaded colour lands in the film exactly as the host would have written it.
struct FilmClearParams
{
    uint4 value;
    uint width;
    uint height;
    uint padding0;
    uint padding1;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<uint4> film;
[[vk::push_constant]] ConstantBuffer<FilmClearParams> params;

[shader("compute")]
[numthreads(8, 8, 1)]
void utilityFilmClear(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.width || id.y >= params.height)
        return;

    film[id.y * params.width + id.x] = params.value;
}
