// Writes VkDrawIndexedIndirectCommand records for the raster draw tests: command k draws triangle k
// (indices 3k..3k+2) when k < drawn, and nothing otherwise.

RWStructuredBuffer<uint> rasterTestCommands;

struct RasterTestIndirectParams
{
    uint commands;
    uint drawn;
};
[[vk::push_constant]] ConstantBuffer<RasterTestIndirectParams> params;

[shader("compute")]
[numthreads(64, 1, 1)]
void rasterTestIndirect(uint3 tid : SV_DispatchThreadID)
{
    const uint k = tid.x;
    if (k >= params.commands)
        return;
    const uint base = k * 5;
    rasterTestCommands[base + 0] = k < params.drawn ? 3 : 0;
    rasterTestCommands[base + 1] = 1;
    rasterTestCommands[base + 2] = 3 * k;
    rasterTestCommands[base + 3] = 0;
    rasterTestCommands[base + 4] = 0;
}
