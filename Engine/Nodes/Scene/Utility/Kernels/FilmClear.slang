// KernelFilmClearParams in UtilityRenderBuffer.cpp mirrors this. Pixels are copied as bits, so a clear
// value or a shaded colour lands in the film exactly as the host would have written it. The
// accumulation holds, per pixel, the weighted sum of its samples and then, width * height on, the sum
// of their weights in x.
struct FilmClearParams
{
    uint4 value;
    uint width;
    uint height;
    uint padding0;
    uint padding1;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<uint4> film;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> accumulation;
[[vk::push_constant]] ConstantBuffer<FilmClearParams> params;

[shader("compute")]
[numthreads(8, 8, 1)]
void utilityFilmClear(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.width || id.y >= params.height)
        return;
    const uint pixel = id.y * params.width + id.x;
    film[pixel] = params.value;
    accumulation[pixel] = float4(0.0, 0.0, 0.0, 0.0);
    accumulation[params.width * params.height + pixel] = float4(0.0, 0.0, 0.0, 0.0);
}
