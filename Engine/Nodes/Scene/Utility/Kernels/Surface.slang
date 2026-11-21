module Surface;

import UtilityTypes;
import Texture;
import Project3D;

// The three table indices of a triangle's corners for a value bound per vertex or per corner.
public uint3 cornerIndices(IntTable triangles, GeometryRecord geometry, uint offset, uint binding, uint primitive)
{
    if (binding == kColorBindingVertex) {
        const uint triangle = geometry.triangleOffset + 3 * primitive;
        return uint3(offset + uint(triangles[triangle]), offset + uint(triangles[triangle + 1]), offset + uint(triangles[triangle + 2]));
    }
    const uint corner0 = offset + 3 * primitive;
    return uint3(corner0, corner0 + 1, corner0 + 2);
}

public float4 surfaceColor(Float4Table colors, IntTable triangles, GeometryRecord geometry, uint primitive, float u, float v)
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

public void cornerSts(Float2Table sts, IntTable triangles, GeometryRecord geometry, uint primitive, out float2 st0, out float2 st1, out float2 st2)
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

// An instance's object-to-world rows o and world-to-object rows w at a time, as the instance transform
// table holds them.
public struct InstanceFrame
{
    public float4 o0;
    public float4 o1;
    public float4 o2;
    public float4 o3;
    public float4 w0;
    public float4 w1;
    public float4 w2;
    public float4 w3;
};

public InstanceFrame instanceFrame(Float4Table instanceTransforms, UintTable instanceMotion, Float4Table motionRows, uint instance, float time)
{
    InstanceFrame frame;
    const uint first = instanceMotion[instance];
    if (first != kNoMotion) {
        composeStep(stepAtTime(motionRows, first, time), frame.o0, frame.o1, frame.o2, frame.o3, frame.w0, frame.w1, frame.w2, frame.w3);
        return frame;
    }
    const uint row = instance * kInstanceTransformRows;
    frame.o0 = instanceTransforms[row];
    frame.o1 = instanceTransforms[row + 1];
    frame.o2 = instanceTransforms[row + 2];
    frame.o3 = instanceTransforms[row + 3];
    frame.w0 = instanceTransforms[row + 4];
    frame.w1 = instanceTransforms[row + 5];
    frame.w2 = instanceTransforms[row + 6];
    frame.w3 = instanceTransforms[row + 7];
    return frame;
}

// A value of a geometry's steps at a time: index's entry of the step before it and of the one after, blended.
public float3 stepValue(Float4Table values, uint offset, uint steps, uint stride, uint index, float time)
{
    if (steps <= 1)
        return values[offset + index].xyz;
    uint span;
    float fraction;
    motionSpan(time, steps, span, fraction);
    const uint at = offset + span * stride + index;
    return lerp(values[at].xyz, values[at + stride].xyz, fraction);
}

// The object-space shading normal at a hit, interpolated from the geometry's normals; zero without them.
public float3 surfaceNormal(Float4Table normals, IntTable triangles, GeometryRecord geometry, uint primitive, float u, float v, float time)
{
    const uint offset = geometry.normalOffset;
    const uint binding = geometry.normalBinding;

    if (binding == kNormalBindingNone)
        return float3(0.0, 0.0, 0.0);
    if (binding == kColorBindingConstant)
        return normals[offset].xyz;
    if (binding == kColorBindingTriangle)
        return normals[offset + primitive].xyz;

    if (geometry.normalSteps > 1) {
        const uint3 local = cornerIndices(triangles, geometry, 0, binding, primitive);
        const uint steps = geometry.normalSteps;
        const uint stride = geometry.normalStride;
        return (1.0 - u - v) * stepValue(normals, offset, steps, stride, local.x, time) + u * stepValue(normals, offset, steps, stride, local.y, time)
            + v * stepValue(normals, offset, steps, stride, local.z, time);
    }
    const uint3 corners = cornerIndices(triangles, geometry, offset, binding, primitive);
    return ((1.0 - u - v) * normals[corners.x] + u * normals[corners.y] + v * normals[corners.z]).xyz;
}

// The unit world shading normal for an object-space normal, through the instance's world-to-object
// rows (the inverse transpose), turned to geometric's side; geometric when there is none.
public float3 shadingNormal(float3 objectNormal, float4 w0, float4 w1, float4 w2, float3 geometric)
{
    const float3 world = float3(dot(w0.xyz, objectNormal), dot(w1.xyz, objectNormal), dot(w2.xyz, objectNormal));
    const float lengthW = length(world);

    if (!(lengthW > 1e-12) || isinf(lengthW))
        return geometric;

    const float3 unit = world / lengthW;
    return dot(unit, geometric) < 0.0 ? -unit : unit;
}

// The world positions of a hit triangle's corners at a time, through the instance's object-to-world rows.
public void triangleCorners(Float4Table points, IntTable triangles, GeometryRecord geometry, uint primitive, float time,
                            float4 o0, float4 o1, float4 o2, float4 o3, out float3 p0, out float3 p1, out float3 p2)
{
    const uint triangle = geometry.triangleOffset + 3 * primitive;
    const uint offset = geometry.pointOffset;
    const uint steps = geometry.pointSteps;
    const uint stride = geometry.pointStride;
    p0 = transformRow(float4(stepValue(points, offset, steps, stride, uint(triangles[triangle]), time), 1.0), o0, o1, o2, o3).xyz;
    p1 = transformRow(float4(stepValue(points, offset, steps, stride, uint(triangles[triangle + 1]), time), 1.0), o0, o1, o2, o3).xyz;
    p2 = transformRow(float4(stepValue(points, offset, steps, stride, uint(triangles[triangle + 2]), time), 1.0), o0, o1, o2, o3).xyz;
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
public float4 readTexture(StructuredBuffer<TextureBinding> bindings, StructuredBuffer<uint4> images, Float4Table texels,
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
public float surfaceOpacity(StructuredBuffer<TextureBinding> bindings, StructuredBuffer<uint4> images, Float4Table texels,
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
public bool surfaceAbsent(Float4Table colors,
                          IntTable triangles,
                          Float4Table points,
                          Float4Table instanceTransforms,
                          UintTable instanceMotion,
                          Float4Table motionRows,
                          Float2Table sts,
                          StructuredBuffer<TextureBinding> bindings,
                          StructuredBuffer<uint4> images,
                          Float4Table texels,
                          StructuredBuffer<ProjectionRecord> projections,
                          GeometryRecord geometry,
                          MaterialRecord material,
                          uint instance,
                          uint primitive,
                          float u,
                          float v,
                          float3 position,
                          float3 direction,
                          float width,
                          float time)
{
    const InstanceFrame frame = instanceFrame(instanceTransforms, instanceMotion, motionRows, instance, time);
    float3 p0;
    float3 p1;
    float3 p2;

    triangleCorners(points, triangles, geometry, primitive, time, frame.o0, frame.o1, frame.o2, frame.o3, p0, p1, p2);

    const float3 outward = normalize(cross(p1 - p0, p2 - p0));

    if (material.kind.x == kMaterialProject3D) {
        float4 color;

        return !evaluateProject3D(projections[material.kind.y], bindings, images, texels, position, outward, direction, width, frame.o0, frame.o1, frame.o2, frame.o3,
                                  frame.w0, frame.w1, frame.w2, frame.w3, color);
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

/**
 * @brief The share of a shadow ray's light that goes on through a hit, with no caustics: none through
 * an opaque surface or one with an opacityThreshold, 1 - opacity through the rest. UsdPreviewSurface
 * has no transmission colour, and its diffuseColor is what the opaque part scatters, so the light
 * through is untinted.
 **/
public float3 surfaceTransmission(Float4Table colors,
                                  IntTable triangles,
                                  Float2Table sts,
                                  StructuredBuffer<TextureBinding> bindings,
                                  StructuredBuffer<uint4> images,
                                  Float4Table texels,
                                  GeometryRecord geometry,
                                  MaterialRecord material,
                                  uint primitive,
                                  float u,
                                  float v)
{
    if (material.kind.x != kMaterialPreviewSurface || material.coverage.y > 0.0)
        return float3(0.0);

    const bool hasSt = geometry.stBinding != kStBindingNone;
    float2 st = float2(0.0, 0.0);

    if (hasSt) {
        float2 st0;
        float2 st1;
        float2 st2;

        cornerSts(sts, triangles, geometry, primitive, st0, st1, st2);
        st = (1.0 - u - v) * st0 + u * st1 + v * st2;
    }

    const float4 display = surfaceColor(colors, triangles, geometry, primitive, u, v);
    const float opacity = surfaceOpacity(bindings, images, texels, material, geometry, display, hasSt, st, kFinestLevel);

    if (!(opacity < 1.0))
        return float3(0.0);
    return float3(1.0 - max(opacity, 0.0));
}
