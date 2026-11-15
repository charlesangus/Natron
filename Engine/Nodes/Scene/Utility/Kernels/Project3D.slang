module Project3D;

import UtilityTypes;
import Texture;

// A world position in the projector's clip space. In geometry space that is the world as it was
// around the object where the projection was set up, however it moved since.
float4 projectorClip(ProjectionRecord p, float3 worldPosition, float4 w0, float4 w1, float4 w2, float4 w3)
{
    float4 reference = float4(worldPosition, 1.0);

    if (p.settings.x == 1) {
        const float4 local = transformRow(reference, w0, w1, w2, w3);
        reference = transformRow(local, p.geometryFrame0, p.geometryFrame1, p.geometryFrame2, p.geometryFrame3);
    }
    return transformRow(reference, p.worldToProjector0, p.worldToProjector1, p.worldToProjector2, p.worldToProjector3);
}

float2 plateUv(ProjectionRecord p, float4 c)
{
    const float2 frame = c.xy / ((c.w > 0.0) ? c.w : 1.0);
    return frame * p.plateTransform.xy + p.plateTransform.zw;
}

// The plate footprint of a pixel pixelWidth wide in the world: the plate area two steps across the
// surface cover, stretched by the obliquity of the ray the way stFootprint is.
float plateFootprint(ProjectionRecord p, float3 worldPosition, float3 outward, float3 direction, float pixelWidth, float2 uv, float4 w0, float4 w1, float4 w2, float4 w3)
{
    if (!(pixelWidth > 0.0))
        return kFinestLevel;

    const float3 helper = abs(outward.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    const float3 tangent = normalize(cross(outward, helper));
    const float3 bitangent = cross(outward, tangent);
    const float2 du = plateUv(p, projectorClip(p, worldPosition + pixelWidth * tangent, w0, w1, w2, w3)) - uv;
    const float2 dv = plateUv(p, projectorClip(p, worldPosition + pixelWidth * bitangent, w0, w1, w2, w3)) - uv;
    const float area = abs(du.x * dv.y - du.y * dv.x);

    if (!(area > 0.0))
        return kFinestLevel;
    return 0.5 * log2(area) - log2(max(abs(dot(outward, direction)), 1e-3));
}

// natronProject3D.glslfx's surface, at a hit's world position. outward is the world normal of the face
// as its winding gives it, direction the ray's and pixelWidth the pixel's world width at the hit;
// objectToWorld and worldToObject are the hit instance's rows. Returns false where Storm discards the
// fragment, which then shows nothing.
public bool evaluateProject3D(ProjectionRecord p,
                              StructuredBuffer<TextureBinding> bindings,
                              StructuredBuffer<uint4> images,
                              StructuredBuffer<float4> texels,
                              float3 worldPosition,
                              float3 outward,
                              float3 direction,
                              float pixelWidth,
                              float4 o0, float4 o1, float4 o2, float4 o3,
                              float4 w0, float4 w1, float4 w2, float4 w3,
                              out float4 color)
{
    color = float4(0.0, 0.0, 0.0, 1.0);

    const bool geometrySpace = p.settings.x == 1;
    const float4 c = projectorClip(p, worldPosition, w0, w1, w2, w3);
    const float2 frame = c.xy / ((c.w > 0.0) ? c.w : 1.0);
    const float2 uv = plateUv(p, c);
    const bool outsideIsTransparent = p.plate.x == 1;
    const int projectOn = p.settings.y;

    if (projectOn != 0) {
        float4 origin = p.projectorOrigin;

        if (geometrySpace) {
            const float4 local = transformRow(origin, p.geometryFrameInverse0, p.geometryFrameInverse1, p.geometryFrameInverse2, p.geometryFrameInverse3);
            origin = transformRow(local, o0, o1, o2, o3);
        }

        const float3 toProjector = origin.xyz - worldPosition * origin.w;
        const bool facing = dot(outward, toProjector) > 0.0;

        if (facing != (projectOn == 1))
            return !outsideIsTransparent;
    }
    if (c.w <= 0.0)
        return !outsideIsTransparent;

    const float depth = c.z / c.w;

    if (p.settings.w != 0 && (depth < -1.0 || depth > 1.0))
        return !outsideIsTransparent;
    if (p.settings.z != 0 && (frame.x < 0.0 || frame.y < 0.0 || frame.x > 1.0 || frame.y > 1.0))
        return false;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
        return false;

    const float4 plate = p.plate.y >= 0 ? sampleBinding(bindings, images, texels, p.plate.y, uv, plateFootprint(p, worldPosition, outward, direction, pixelWidth, uv, w0, w1, w2, w3)) : float4(0.0, 0.0, 0.0, 1.0);

    if (plate.w <= 0.0)
        return false;
    color = float4(plate.xyz * plate.w, plate.w);
    return true;
}
