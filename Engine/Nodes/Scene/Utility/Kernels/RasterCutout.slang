import UtilityTypes;
import Texture;
import Project3D;
import Surface;
import RasterDraw;

// KernelCutoutParams in UtilityKernels.h mirrors this: a visibility draw's parameters, then the address of
// GpuTracer::RasterScene::hitTables and the window pixel of the attachment's first texel, its column and its
// row in the window.
struct CutoutParams
{
    VisibilityParams draw;
    uint2 hitTables;
    int2 origin;
};

// KernelCutoutFrame in UtilityKernels.h mirrors this: the pass's camera rays as RasterHits makes them, and
// a pixel's world width on the near plane and on the far plane.
struct CutoutFrame
{
    float3 nearAtCentre;
    float jitterX;
    float3 nearPerX;
    float jitterY;
    float3 nearPerY;
    int windowWidth;
    float3 farAtCentre;
    int windowHeight;
    float3 farPerX;
    uint padding0;
    float3 farPerY;
    uint padding1;
    float4 pixelFootprint;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint2> tableAddresses;
[[vk::binding(1, 0)]] StructuredBuffer<MaterialRecord> materials;
[[vk::binding(2, 0)]] StructuredBuffer<TextureBinding> textureBindings;
[[vk::binding(3, 0)]] StructuredBuffer<uint4> images;
[[vk::binding(4, 0)]] StructuredBuffer<ProjectionRecord> projections;
[[vk::binding(5, 0)]] StructuredBuffer<CutoutFrame> frames;
[[vk::push_constant]] ConstantBuffer<CutoutParams> params;

property GeometryTable geometries { get { return GeometryTable(tableAddresses[kTableGeometries]); } }
property Float4Table colors { get { return Float4Table(tableAddresses[kTableColors]); } }
property IntTable triangles { get { return IntTable(tableAddresses[kTableTriangles]); } }
property Float4Table points { get { return Float4Table(tableAddresses[kTablePoints]); } }
property Float4Table instanceTransforms { get { return Float4Table(tableAddresses[kTableInstanceTransforms]); } }
property Float2Table sts { get { return Float2Table(tableAddresses[kTableSts]); } }
property Float4Table texels { get { return Float4Table(tableAddresses[kTableTexels]); } }
property UintTable instanceMotion { get { return UintTable(tableAddresses[kTableInstanceMotion]); } }
property Float4Table motionRows { get { return Float4Table(tableAddresses[kTableMotionRows]); } }

[shader("vertex")]
VisibilityVarying cutoutVertex(uint vertex : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID)
{
    return visibilityVertexOf(params.draw, vertex, instance);
}

// As visibilityFragment(), but a texel where the surface is absent, under a PreviewSurface's opacityThreshold
// or where Project3D discards, is not drawn, so that the depth test lets the next surface behind it through
// and the visibility buffer needs no ray traced past the hole. The test is the one ResolveHits makes, on the
// hit RasterHits will rebuild for the texel.
[shader("fragment")]
uint2 cutoutFragment(VisibilityVarying input, uint primitive : SV_PrimitiveID) : SV_Target
{
    const CutoutFrame frame = frames[0];
    const Uint4Table tables = Uint4Table(params.hitTables);
    const uint4 placement = tables[1 + input.placement];
    const GeometryRecord geometry = geometries[placement.x];
    const MaterialRecord material = materials[geometry.materialIndex];

    if (!mayLeaveHoles(material))
        return uint2(input.placement, primitive);

    float3 origin;
    float3 direction;
    float tMax;
    cameraRay(frame.nearAtCentre, frame.nearPerX, frame.nearPerY, frame.farAtCentre, frame.farPerX, frame.farPerY,
              float2(frame.jitterX, frame.jitterY), int2(frame.windowWidth, frame.windowHeight),
              params.origin + int2(input.position.xy), origin, direction, tMax);

    float3 corners[3];
    worldCorners(tables, params.draw.time, placement, primitive, corners);

    float t;
    float2 barycentrics;
    if (planeHit(origin, direction, tMax, corners[0], corners[1], corners[2], t, barycentrics)) {
        const float width = lerp(frame.pixelFootprint.x, frame.pixelFootprint.y, t / tMax);

        if (surfaceAbsent(colors, triangles, points, instanceTransforms, instanceMotion, motionRows, sts, textureBindings, images,
                          texels, projections, geometry, material, input.placement, primitive, barycentrics.x, barycentrics.y,
                          origin + t * direction, direction, width, params.draw.time))
            discard;
    }
    return uint2(input.placement, primitive);
}
