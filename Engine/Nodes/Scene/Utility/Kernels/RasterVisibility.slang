import UtilityTypes;
import RasterDraw;

[[vk::push_constant]] ConstantBuffer<VisibilityParams> params;

[shader("vertex")]
VisibilityVarying visibilityVertex(uint vertex : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID)
{
    return visibilityVertexOf(params, vertex, instance);
}

// The placement and the triangle, which an indexed draw numbers from 0 in index-buffer order as the geometry's
// triangle structure does, so the pair is the instance and primitive a ray's hit reports.
[shader("fragment")]
uint2 visibilityFragment(VisibilityVarying input, uint primitive : SV_PrimitiveID) : SV_Target
{
    return uint2(input.placement, primitive);
}
