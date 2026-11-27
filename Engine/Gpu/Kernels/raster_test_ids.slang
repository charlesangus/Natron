// Triangles whose vertices are a pixel position and a depth pulled from a storage buffer, writing the tag
// of their triangle and its primitive id as uint ids, for the visibility raster tests.

StructuredBuffer<float3> rasterIdsVertices;
StructuredBuffer<uint> rasterIdsTags;

struct RasterIdsParams
{
    float2 viewport;
    float2 pad;
};
[[vk::push_constant]] ConstantBuffer<RasterIdsParams> params;

struct RasterIdsVarying
{
    float4 position : SV_Position;
};

[shader("vertex")]
RasterIdsVarying rasterIdsVertex(uint vertex : SV_VulkanVertexID)
{
    const float3 p = rasterIdsVertices[vertex];
    RasterIdsVarying o;
    o.position = float4(p.xy / params.viewport * 2.0 - 1.0, p.z, 1.0);
    return o;
}

[shader("fragment")]
uint2 rasterIdsFragment(RasterIdsVarying input, uint primitive : SV_PrimitiveID) : SV_Target
{
    return uint2(rasterIdsTags[primitive], primitive);
}
