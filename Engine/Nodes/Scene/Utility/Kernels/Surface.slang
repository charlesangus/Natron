module Surface;

import UtilityTypes;
import Texture;
import Project3D;

// The three table indices of a triangle's corners for a value bound per vertex or per corner.
public uint3 cornerIndices(StructuredBuffer<int> triangles, GeometryRecord geometry, uint offset, uint binding, uint primitive)
{
    if (binding == kColorBindingVertex) {
        const uint triangle = geometry.triangleOffset + 3 * primitive;
        return uint3(offset + uint(triangles[triangle]), offset + uint(triangles[triangle + 1]), offset + uint(triangles[triangle + 2]));
    }
    const uint corner0 = offset + 3 * primitive;
    return uint3(corner0, corner0 + 1, corner0 + 2);
}

public float4 surfaceColor(StructuredBuffer<float4> colors, StructuredBuffer<int> triangles, GeometryRecord geometry, uint primitive, float u, float v)
{
    const uint offset = geometry.colorOffset;
    const uint binding = geometry.colorBinding & kColorBindingMask;

    if (binding == kColorBindingConstant)
        return colors[offset];
    if (binding == kColorBindingTriangle)
        return colors[offset + primitive];

    const uint3 corners = cornerIndices(triangles, geometry, offset, binding, primitive);
    return (1.0 - u - v) * colors[corners.x] + u * colors[corners.y] + v * colors[corners.z];
}

// Whether the geometry has the displayColor, or the displayOpacity, primvar that surfaceColor() returns.
public bool hasDisplayColor(GeometryRecord geometry)
{
    return (geometry.colorBinding & kColorNoDisplayColor) == 0;
}

public bool hasDisplayOpacity(GeometryRecord geometry)
{
    return (geometry.colorBinding & kColorNoDisplayOpacity) == 0;
}

public void cornerSts(StructuredBuffer<float2> sts, StructuredBuffer<int> triangles, GeometryRecord geometry, uint primitive, out float2 st0, out float2 st1, out float2 st2)
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
    const uint3 corners = cornerIndices(triangles, geometry, offset, geometry.stBinding, primitive);
    st0 = sts[corners.x];
    st1 = sts[corners.y];
    st2 = sts[corners.z];
}

// The world positions of a hit triangle's corners, through the instance's object-to-world rows.
public void triangleCorners(StructuredBuffer<float4> points, StructuredBuffer<int> triangles, GeometryRecord geometry, uint primitive,
                            float4 o0, float4 o1, float4 o2, float4 o3, out float3 p0, out float3 p1, out float3 p2)
{
    const uint triangle = geometry.triangleOffset + 3 * primitive;
    p0 = transformRow(float4(points[geometry.pointOffset + uint(triangles[triangle])].xyz, 1.0), o0, o1, o2, o3).xyz;
    p1 = transformRow(float4(points[geometry.pointOffset + uint(triangles[triangle + 1])].xyz, 1.0), o0, o1, o2, o3).xyz;
    p2 = transformRow(float4(points[geometry.pointOffset + uint(triangles[triangle + 2])].xyz, 1.0), o0, o1, o2, o3).xyz;
}

public int textureOf(MaterialRecord material, uint slot)
{
    if (slot < 4)
        return material.textures0[slot];
    if (slot < 8)
        return material.textures1[slot - 4];
    return material.textures2[slot - 8];
}

// footprint: the base 2 log of the pixel's width in texture coordinates, before any UsdTransform2d.
public float4 readTexture(StructuredBuffer<TextureBinding> bindings, StructuredBuffer<uint4> images, StructuredBuffer<float4> texels,
                          int binding, bool hasSt, float2 st, float footprint)
{
    if (!hasSt)
        return bindingFallback(bindings, binding);

    const TextureBinding b = bindings[binding];
    return sampleBinding(bindings, images, texels, binding, transformSt(b, st), footprint + transformFootprint(b));
}

// The texture footprint of a pixel over a triangle by its ray cone: the pixel's width scaled from world
// to texture units by the triangle's areas, and stretched by the obliquity of the hit, as a GPU's
// trilinear lookup takes the longer axis of the footprint.
public float stFootprint(float width, float3 dp1, float3 dp2, float2 dst1, float2 dst2, float cosine)
{
    const float worldArea = length(cross(dp1, dp2));
    const float stArea = abs(dst1.x * dst2.y - dst1.y * dst2.x);

    if (!(worldArea > 0.0) || !(stArea > 0.0) || !(width > 0.0))
        return kFinestLevel;
    return log2(width) + 0.5 * log2(stArea / worldArea) - log2(max(cosine, 1e-3));
}

// A PreviewSurface's opacity at a hit, before its opacityThreshold. An input reading displayOpacity keeps
// its reader's fallback in its constant, for geometry without the primvar.
public float surfaceOpacity(StructuredBuffer<TextureBinding> bindings, StructuredBuffer<uint4> images, StructuredBuffer<float4> texels,
                            MaterialRecord material, GeometryRecord geometry, float4 display, bool hasSt, float2 st, float footprint)
{
    const bool fromDisplay = (material.kind.z & kMaterialOpacityFromDisplayOpacity) != 0 && hasDisplayOpacity(geometry);
    const float constant = fromDisplay ? display.w : material.coverage.x;
    const int binding = textureOf(material, kSlotOpacity);

    return binding == kNoTexture ? constant : readTexture(bindings, images, texels, binding, hasSt, st, footprint).x;
}

// Whether a material can leave a hit without a surface, so that only hits on such materials need
// surfaceAbsent().
public bool mayLeaveHoles(MaterialRecord material)
{
    return material.kind.x == kMaterialProject3D || (material.kind.x == kMaterialPreviewSurface && material.coverage.y > 0.0);
}

// Whether a hit is on no surface at all: under a PreviewSurface's opacityThreshold, or where Project3D
// discards. width is the pixel's world width at the hit, 0 for a ray with no footprint.
public bool surfaceAbsent(StructuredBuffer<float4> colors,
                          StructuredBuffer<int> triangles,
                          StructuredBuffer<float4> points,
                          StructuredBuffer<float4> instanceTransforms,
                          StructuredBuffer<float2> sts,
                          StructuredBuffer<TextureBinding> bindings,
                          StructuredBuffer<uint4> images,
                          StructuredBuffer<float4> texels,
                          StructuredBuffer<ProjectionRecord> projections,
                          GeometryRecord geometry,
                          MaterialRecord material,
                          uint instance,
                          uint primitive,
                          float u,
                          float v,
                          float3 position,
                          float3 direction,
                          float width)
{
    const uint row = instance * kInstanceTransformRows;
    const float4 o0 = instanceTransforms[row];
    const float4 o1 = instanceTransforms[row + 1];
    const float4 o2 = instanceTransforms[row + 2];
    const float4 o3 = instanceTransforms[row + 3];
    float3 p0;
    float3 p1;
    float3 p2;

    triangleCorners(points, triangles, geometry, primitive, o0, o1, o2, o3, p0, p1, p2);

    const float3 outward = normalize(cross(p1 - p0, p2 - p0));

    if (material.kind.x == kMaterialProject3D) {
        float4 color;

        return !evaluateProject3D(projections[material.kind.y], bindings, images, texels, position, outward, direction, width, o0, o1, o2, o3,
                                  instanceTransforms[row + 4], instanceTransforms[row + 5], instanceTransforms[row + 6], instanceTransforms[row + 7], color);
    }

    const bool hasSt = geometry.stBinding != kStBindingNone;
    float2 st = float2(0.0, 0.0);
    float2 st0 = st;
    float2 st1 = st;
    float2 st2 = st;

    if (hasSt) {
        cornerSts(sts, triangles, geometry, primitive, st0, st1, st2);
        st = (1.0 - u - v) * st0 + u * st1 + v * st2;
    }

    const float footprint = stFootprint(width, p1 - p0, p2 - p0, st1 - st0, st2 - st0, abs(dot(outward, direction)));
    const float4 display = surfaceColor(colors, triangles, geometry, primitive, u, v);

    return surfaceOpacity(bindings, images, texels, material, geometry, display, hasSt, st, footprint) < material.coverage.y;
}
