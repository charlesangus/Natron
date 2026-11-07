import UtilityTypes;

// KernelShadeParams in UtilityKernels.h mirrors this. projection holds the row-vector projection terms
// that decide clip z and w from view z: clip z = A * z + B, clip w = C * z + D.
struct ShadeParams
{
    float4 projection;
    float viewZAtNear;
    float viewZAtFar;
    uint count;
    uint padding;
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
[[vk::push_constant]] ConstantBuffer<ShadeParams> params;

float4 surfaceColor(GeometryRecord geometry, uint primitive, float u, float v)
{
    const uint offset = geometry.colorOffset;

    if (geometry.colorBinding == kColorBindingConstant)
        return colors[offset];
    if (geometry.colorBinding == kColorBindingTriangle)
        return colors[offset + primitive];

    uint corner0 = offset + 3 * primitive;
    uint corner1 = corner0 + 1;
    uint corner2 = corner0 + 2;

    if (geometry.colorBinding == kColorBindingVertex) {
        const uint triangle = geometry.triangleOffset + 3 * primitive;

        corner0 = offset + uint(triangles[triangle]);
        corner1 = offset + uint(triangles[triangle + 1]);
        corner2 = offset + uint(triangles[triangle + 2]);
    }
    return (1.0 - u - v) * colors[corner0] + u * colors[corner1] + v * colors[corner2];
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

// A miss writes nothing; the film keeps the buffer's clear value there.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityShadeFlat(uint3 id : SV_DispatchThreadID)
{
    const uint count = params.count;

    if (id.x >= count)
        return;

    const uint geometryIndex = hitIds[kHitGeometry * count + id.x];

    if (geometryIndex == kNoHit)
        return;

    const GeometryRecord geometry = geometries[geometryIndex];
    const uint primitive = hitIds[kHitPrimitive * count + id.x];
    const float t = hitSurface[kHitT * count + id.x];
    const float u = hitSurface[kHitU * count + id.x];
    const float v = hitSurface[kHitV * count + id.x];
    const float4 color = surfaceColor(geometry, primitive, u, v);

    colorOut[id.x] = float4(color.xyz * color.w, color.w);
    depthOut[id.x] = windowDepth(t / rays[kRayTMax * count + id.x]);
    primIdOut[id.x] = geometry.primId;
    instanceIdOut[id.x] = instanceIds[hitIds[kHitInstance * count + id.x]];
}
