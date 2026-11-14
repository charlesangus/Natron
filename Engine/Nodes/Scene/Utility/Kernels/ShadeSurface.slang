import UtilityTypes;
import Texture;
import PreviewSurface;
import Project3D;

// KernelShadeParams in UtilityKernels.h mirrors this. projection holds the row-vector projection terms
// that decide clip z and w from view z: clip z = A * z + B, clip w = C * z + D. pixelFootprint holds a
// pixel's world width on the near plane and on the far plane.
struct ShadeParams
{
    float4 projection;
    float viewZAtNear;
    float viewZAtFar;
    uint count;
    uint padding;
    float4 pixelFootprint;
};

[[vk::binding(0, 0)]] StructuredBuffer<float> rays;
[[vk::binding(1, 0)]] StructuredBuffer<float> hitSurface;
[[vk::binding(2, 0)]] StructuredBuffer<uint> hitIds;
[[vk::binding(3, 0)]] StructuredBuffer<GeometryRecord> geometries;
[[vk::binding(4, 0)]] StructuredBuffer<float4> colors;
[[vk::binding(5, 0)]] StructuredBuffer<int> triangles;
[[vk::binding(6, 0)]] StructuredBuffer<int> instanceIds;
[[vk::binding(7, 0)]] RWStructuredBuffer<float4> colorOut;
[[vk::binding(8, 0)]] RWStructuredBuffer<float> depthOut;
[[vk::binding(9, 0)]] RWStructuredBuffer<int> primIdOut;
[[vk::binding(10, 0)]] RWStructuredBuffer<int> instanceIdOut;
[[vk::binding(11, 0)]] StructuredBuffer<MaterialRecord> materials;
[[vk::binding(12, 0)]] StructuredBuffer<TextureBinding> textureBindings;
[[vk::binding(13, 0)]] StructuredBuffer<uint4> images;
[[vk::binding(14, 0)]] StructuredBuffer<float4> texels;
[[vk::binding(15, 0)]] StructuredBuffer<ProjectionRecord> projections;
[[vk::binding(16, 0)]] StructuredBuffer<float4> points;
[[vk::binding(17, 0)]] StructuredBuffer<float4> instanceTransforms;
[[vk::binding(18, 0)]] StructuredBuffer<float2> sts;
[[vk::push_constant]] ConstantBuffer<ShadeParams> params;

static const float4 kUnsupportedColor = float4(1.0, 0.0, 1.0, 1.0);

// The three table indices of a triangle's corners for a value bound per vertex or per corner.
uint3 cornerIndices(GeometryRecord geometry, uint offset, uint binding, uint primitive)
{
    if (binding == kColorBindingVertex) {
        const uint triangle = geometry.triangleOffset + 3 * primitive;
        return uint3(offset + uint(triangles[triangle]), offset + uint(triangles[triangle + 1]), offset + uint(triangles[triangle + 2]));
    }
    const uint corner0 = offset + 3 * primitive;
    return uint3(corner0, corner0 + 1, corner0 + 2);
}

float4 surfaceColor(GeometryRecord geometry, uint primitive, float u, float v)
{
    const uint offset = geometry.colorOffset;

    if (geometry.colorBinding == kColorBindingConstant)
        return colors[offset];
    if (geometry.colorBinding == kColorBindingTriangle)
        return colors[offset + primitive];

    const uint3 corners = cornerIndices(geometry, offset, geometry.colorBinding, primitive);
    return (1.0 - u - v) * colors[corners.x] + u * colors[corners.y] + v * colors[corners.z];
}

void cornerSts(GeometryRecord geometry, uint primitive, out float2 st0, out float2 st1, out float2 st2)
{
    const uint offset = geometry.stOffset;

    if (geometry.stBinding == kColorBindingConstant) {
        st0 = sts[offset];
        st1 = st0;
        st2 = st0;
        return;
    }
    if (geometry.stBinding == kColorBindingTriangle) {
        st0 = sts[offset + primitive];
        st1 = st0;
        st2 = st0;
        return;
    }
    const uint3 corners = cornerIndices(geometry, offset, geometry.stBinding, primitive);
    st0 = sts[corners.x];
    st1 = sts[corners.y];
    st2 = sts[corners.z];
}

// Window depth, the convention the host reads every delegate's depth AOV in: 0 at the near plane, 1 at
// the far plane, through the camera's projection. View z is linear along a ray, projected depth is not.
float windowDepth(float alongRay)
{
    const float viewZ = params.viewZAtNear + alongRay * (params.viewZAtFar - params.viewZAtNear);
    const float clipZ = params.projection.x * viewZ + params.projection.y;
    const float clipW = params.projection.z * viewZ + params.projection.w;

    return 0.5 * clipZ / clipW + 0.5;
}

int textureOf(MaterialRecord material, uint slot)
{
    if (slot < 4)
        return material.textures0[slot];
    if (slot < 8)
        return material.textures1[slot - 4];
    return material.textures2[slot - 8];
}

// footprint: the base 2 log of the pixel's width in texture coordinates, before any UsdTransform2d.
float4 readTexture(int binding, bool hasSt, float2 st, float footprint)
{
    if (!hasSt)
        return bindingFallback(textureBindings, binding);

    const TextureBinding b = textureBindings[binding];
    return sampleBinding(textureBindings, images, texels, binding, transformSt(b, st), footprint + transformFootprint(b));
}

float3 colorInput(MaterialRecord material, uint slot, float3 constant, bool hasSt, float2 st, float footprint)
{
    const int binding = textureOf(material, slot);
    return binding == kNoTexture ? constant : readTexture(binding, hasSt, st, footprint).xyz;
}

float floatInput(MaterialRecord material, uint slot, float constant, bool hasSt, float2 st, float footprint)
{
    const int binding = textureOf(material, slot);
    return binding == kNoTexture ? constant : readTexture(binding, hasSt, st, footprint).x;
}

// The pixel's width where the ray meets the surface, at alongRay of the way from the near plane to the
// far plane.
float pixelWidth(float alongRay)
{
    return lerp(params.pixelFootprint.x, params.pixelFootprint.y, alongRay);
}

// The texture footprint of a pixel over a triangle by its ray cone: the pixel's width scaled from world
// to texture units by the triangle's areas, and stretched by the obliquity of the hit, as a GPU's
// trilinear lookup takes the longer axis of the footprint.
float stFootprint(float width, float3 dp1, float3 dp2, float2 dst1, float2 dst2, float cosine)
{
    const float worldArea = length(cross(dp1, dp2));
    const float stArea = abs(dst1.x * dst2.y - dst1.y * dst2.x);

    if (!(worldArea > 0.0) || !(stArea > 0.0) || !(width > 0.0))
        return kFinestLevel;
    return log2(width) + 0.5 * log2(stArea / worldArea) - log2(max(cosine, 1e-3));
}

// Where nothing shows the pixel takes the film's clear values, as a miss leaves them.
void writeNothing(uint index)
{
    colorOut[index] = float4(0.0, 0.0, 0.0, 0.0);
    depthOut[index] = 1.0;
    primIdOut[index] = -1;
    instanceIdOut[index] = -1;
}

// Emission and reflected light, premultiplied by coverage. Light reaches only lit scenes, so a scene
// without lights shows emission alone.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityShadeSurface(uint3 id : SV_DispatchThreadID)
{
    const uint count = params.count;

    if (id.x >= count)
        return;

    const uint geometryIndex = hitIds[kHitGeometry * count + id.x];

    if (geometryIndex == kNoHit)
        return;

    const GeometryRecord geometry = geometries[geometryIndex];
    const MaterialRecord material = materials[geometry.materialIndex];
    const uint primitive = hitIds[kHitPrimitive * count + id.x];
    const uint instance = hitIds[kHitInstance * count + id.x];
    const float t = hitSurface[kHitT * count + id.x];
    const float u = hitSurface[kHitU * count + id.x];
    const float v = hitSurface[kHitV * count + id.x];
    const float3 origin = float3(rays[kRayOriginX * count + id.x], rays[kRayOriginY * count + id.x], rays[kRayOriginZ * count + id.x]);
    const float3 direction = float3(rays[kRayDirectionX * count + id.x], rays[kRayDirectionY * count + id.x], rays[kRayDirectionZ * count + id.x]);
    const float3 worldPosition = origin + t * direction;

    const uint row = instance * kInstanceTransformRows;
    const float4 o0 = instanceTransforms[row];
    const float4 o1 = instanceTransforms[row + 1];
    const float4 o2 = instanceTransforms[row + 2];
    const float4 o3 = instanceTransforms[row + 3];
    const uint triangle = geometry.triangleOffset + 3 * primitive;
    const float3 p0 = transformRow(float4(points[geometry.pointOffset + uint(triangles[triangle])].xyz, 1.0), o0, o1, o2, o3).xyz;
    const float3 p1 = transformRow(float4(points[geometry.pointOffset + uint(triangles[triangle + 1])].xyz, 1.0), o0, o1, o2, o3).xyz;
    const float3 p2 = transformRow(float4(points[geometry.pointOffset + uint(triangles[triangle + 2])].xyz, 1.0), o0, o1, o2, o3).xyz;
    const float3 outward = normalize(cross(p1 - p0, p2 - p0));
    const float alongRay = t / rays[kRayTMax * count + id.x];
    const float width = pixelWidth(alongRay);
    const float cosine = abs(dot(outward, direction));

    float4 shaded = kUnsupportedColor;

    if (material.kind.x == kMaterialProject3D) {
        const float4 w0 = instanceTransforms[row + 4];
        const float4 w1 = instanceTransforms[row + 5];
        const float4 w2 = instanceTransforms[row + 6];
        const float4 w3 = instanceTransforms[row + 7];

        if (!evaluateProject3D(projections[material.kind.y], textureBindings, images, texels, worldPosition, outward, direction, width, o0, o1, o2, o3, w0, w1, w2, w3, shaded)) {
            writeNothing(id.x);
            return;
        }
    } else if (material.kind.x == kMaterialPreviewSurface) {
        const bool hasSt = geometry.stBinding != kStBindingNone;
        float2 st = float2(0.0, 0.0);
        float2 st0 = st;
        float2 st1 = st;
        float2 st2 = st;

        if (hasSt) {
            cornerSts(geometry, primitive, st0, st1, st2);
            st = (1.0 - u - v) * st0 + u * st1 + v * st2;
        }

        const float footprint = stFootprint(width, p1 - p0, p2 - p0, st1 - st0, st2 - st0, cosine);

        const float4 display = surfaceColor(geometry, primitive, u, v);
        const uint flags = material.kind.z;
        const float3 diffuseConstant = (flags & kMaterialDiffuseFromDisplayColor) != 0 ? display.xyz : material.diffuseColor.xyz;
        const float opacityConstant = (flags & kMaterialOpacityFromDisplayOpacity) != 0 ? display.w : material.coverage.x;
        float opacity = floatInput(material, kSlotOpacity, opacityConstant, hasSt, st, footprint);
        const float threshold = material.coverage.y;

        if (threshold > 0.0) {
            if (opacity < threshold) {
                writeNothing(id.x);
                return;
            }
            opacity = 1.0;
        }

        const float3 geometric = dot(outward, direction) > 0.0 ? -outward : outward;
        float3 normal = geometric;
        const int normalTexture = textureOf(material, kSlotNormal);

        if (normalTexture != kNoTexture && hasSt)
            normal = applyNormalMap(geometric, readTexture(normalTexture, true, st, footprint).xyz, p1 - p0, p2 - p0, st1 - st0, st2 - st0);

        PreviewSurfaceSample surface;
        surface.diffuseColor = colorInput(material, kSlotDiffuseColor, diffuseConstant, hasSt, st, footprint);
        surface.specularColor = colorInput(material, kSlotSpecularColor, material.specularColor.xyz, hasSt, st, footprint);
        const float3 emissiveConstant = (flags & kMaterialEmissiveFromDisplayColor) != 0 ? display.xyz : material.emissiveColor.xyz;
        surface.emissiveColor = colorInput(material, kSlotEmissiveColor, emissiveConstant, hasSt, st, footprint);
        surface.normal = normal;
        surface.metallic = floatInput(material, kSlotMetallic, material.surface.x, hasSt, st, footprint);
        surface.roughness = floatInput(material, kSlotRoughness, material.surface.y, hasSt, st, footprint);
        surface.clearcoat = floatInput(material, kSlotClearcoat, material.surface.z, hasSt, st, footprint);
        surface.clearcoatRoughness = floatInput(material, kSlotClearcoatRoughness, material.surface.w, hasSt, st, footprint);
        surface.opacity = opacity;
        surface.ior = floatInput(material, kSlotIor, material.coverage.z, hasSt, st, footprint);
        surface.useSpecularWorkflow = material.specularColor.w != 0.0;

        const float3 radiance = surface.emissiveColor;
        shaded = float4(radiance * opacity, opacity);
    }

    colorOut[id.x] = shaded;
    depthOut[id.x] = windowDepth(alongRay);
    primIdOut[id.x] = geometry.primId;
    instanceIdOut[id.x] = instanceIds[instance];
}
