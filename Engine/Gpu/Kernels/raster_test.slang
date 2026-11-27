// Flat-coloured triangles whose vertices are pixel positions pulled from a storage buffer, for the
// raster pipeline tests.

StructuredBuffer<float2> rasterTestPositions;

struct RasterTestParams
{
    float2 viewport;
    float2 pad;
    float4 colour;
};
[[vk::push_constant]] ConstantBuffer<RasterTestParams> params;

struct RasterTestVarying
{
    float4 position : SV_Position;
};

[shader("vertex")]
RasterTestVarying rasterTestVertex(uint vertex : SV_VulkanVertexID)
{
    // Vulkan's clip space has y pointing down, so pixel rows map onto attachment rows unflipped.
    const float2 p = rasterTestPositions[vertex];
    RasterTestVarying o;
    o.position = float4(p / params.viewport * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 rasterTestFragment(RasterTestVarying input) : SV_Target
{
    return params.colour;
}
