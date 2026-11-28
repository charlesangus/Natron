module RasterDraw;

import UtilityTypes;

// KernelVisibilityParams in UtilityKernels.h mirrors this. One draw covers the instances of one geometry: its
// triangles come from the index buffer the draw binds, and each instance's entry in the instances table, from
// the draw's first instance on, holds the placement a hit reports and the first row of its transform in rows.
//
// A transform's first row holds its step count in x: 0 for an instance that holds still, followed by the four
// rows of its row-vector object-to-world matrix, or two or more, followed by that many steps as stepAtTime()
// reads them. A deforming geometry's points hold pointSteps steps of pointCount points, one after another.
public struct VisibilityParams
{
    // The rows of the row-vector view-projection, into Vulkan's clip space: y runs down the attachment and depth
    // from 0 at the near plane to w at the far one.
    public float4 viewProjection0;
    public float4 viewProjection1;
    public float4 viewProjection2;
    public float4 viewProjection3;
    // The pass's sub-pixel offset in normalized device coordinates, applied after the projection so that it moves
    // every depth alike.
    public float2 jitter;
    // From 0 at the shutter's opening to 1 at its closing, as a ray's time.
    public float time;
    public uint pointCount;
    public uint2 points;
    public uint2 instances;
    public uint2 rows;
    public uint pointSteps;
    public uint unused;
};

public struct VisibilityVarying
{
    public float4 position : SV_Position;
    public nointerpolation uint placement : PLACEMENT;
};

float3 pointAt(FloatTable points, uint index)
{
    return float3(points[3 * index], points[3 * index + 1], points[3 * index + 2]);
}

void objectToWorld(Float4Table rows, uint first, float time, out float4 o0, out float4 o1, out float4 o2, out float4 o3)
{
    if (asuint(rows[first].x) == 0) {
        o0 = rows[first + 1];
        o1 = rows[first + 2];
        o2 = rows[first + 3];
        o3 = rows[first + 4];
    } else {
        float4 w0, w1, w2, w3;
        composeStep(stepAtTime(rows, first, time), o0, o1, o2, o3, w0, w1, w2, w3);
    }
}

public VisibilityVarying visibilityVertexOf(VisibilityParams params, uint vertex, uint instance)
{
    const UintTable instances = UintTable(params.instances);
    const uint placement = instances[2 * instance];
    const uint first = instances[2 * instance + 1];

    const FloatTable points = FloatTable(params.points);
    float3 local;
    if (params.pointSteps > 1) {
        uint span;
        float fraction;
        motionSpan(params.time, params.pointSteps, span, fraction);
        const uint from = span * params.pointCount + vertex;
        local = lerp(pointAt(points, from), pointAt(points, from + params.pointCount), fraction);
    } else {
        local = pointAt(points, vertex);
    }

    float4 o0, o1, o2, o3;
    objectToWorld(Float4Table(params.rows), first, params.time, o0, o1, o2, o3);
    const float4 world = transformRow(float4(local, 1.0), o0, o1, o2, o3);
    float4 clip = transformRow(world, params.viewProjection0, params.viewProjection1, params.viewProjection2,
                               params.viewProjection3);
    clip.xy += params.jitter * clip.w;

    VisibilityVarying o;
    o.position = clip;
    o.placement = placement;
    return o;
}

// The camera ray through a window pixel's centre plus jitter, in pixels, as RayGen's frustumRay() makes it:
// from the near plane to the far one, whose points at the window's centre and steps per unit of normalized
// device coordinates the six vectors give.
public void cameraRay(float3 nearAtCentre, float3 nearPerX, float3 nearPerY, float3 farAtCentre, float3 farPerX, float3 farPerY,
                      float2 jitter, int2 window, int2 pixel, out float3 origin, out float3 direction, out float tMax)
{
    const float ndcX = 2.0 * (float(pixel.x) + 0.5 + jitter.x) / float(window.x) - 1.0;
    const float ndcY = 2.0 * (float(pixel.y) + 0.5 + jitter.y) / float(window.y) - 1.0;
    origin = nearAtCentre + ndcX * nearPerX + ndcY * nearPerY;
    const float3 end = farAtCentre + ndcX * farPerX + ndcY * farPerY;
    const float3 span = end - origin;
    tMax = length(span);
    direction = span / tMax;
}

// The triangle's corners in world space at a time, as visibilityVertexOf() places them. tables is
// GpuTracer::RasterScene::hitTables, in uint4 rows: a header holding the address of the transforms; then one
// row per placement, its dense geometry index, the first row of its transform and the row of its geometry's
// record; then two rows per drawn geometry, the addresses of its points and of its triangles, then its point
// count and point steps.
public void worldCorners(Uint4Table tables, float time, uint4 placement, uint primitive, out float3 corners[3])
{
    const uint4 header = tables[0];
    const uint4 where = tables[placement.z];
    const uint4 counts = tables[placement.z + 1];
    const FloatTable points = FloatTable(where.xy);
    const UintTable triangles = UintTable(where.zw);

    float4 o0, o1, o2, o3;
    objectToWorld(Float4Table(header.xy), placement.y, time, o0, o1, o2, o3);

    uint span = 0;
    float fraction = 0.0;
    if (counts.y > 1)
        motionSpan(time, counts.y, span, fraction);
    for (uint c = 0; c < 3; ++c) {
        const uint index = triangles[3 * primitive + c];
        float3 local;
        if (counts.y > 1) {
            const uint from = span * counts.x + index;
            local = lerp(pointAt(points, from), pointAt(points, from + counts.x), fraction);
        } else {
            local = pointAt(points, index);
        }
        corners[c] = transformRow(float4(local, 1.0), o0, o1, o2, o3).xyz;
    }
}

// The ray's crossing of the triangle's plane, with barycentrics as a ray query reports them. The raster's
// coverage decided that the ray hits this triangle, so a crossing just past an edge by rounding is pulled back
// onto it; only a ray along the plane misses.
public bool planeHit(float3 origin, float3 direction, float tMax, float3 p0, float3 p1, float3 p2, out float t, out float2 barycentrics)
{
    t = 0.0;
    barycentrics = float2(0.0);
    const float3 e1 = p1 - p0;
    const float3 e2 = p2 - p0;
    const float3 pv = cross(direction, e2);
    const float determinant = dot(e1, pv);
    if (determinant == 0.0 || isnan(determinant))
        return false;
    const float inverse = 1.0 / determinant;
    const float3 s = origin - p0;
    const float3 q = cross(s, e1);
    float u = max(dot(s, pv) * inverse, 0.0);
    float v = max(dot(direction, q) * inverse, 0.0);
    const float sum = u + v;
    if (sum > 1.0) {
        u /= sum;
        v /= sum;
    }
    t = clamp(dot(e2, q) * inverse, 0.0, tMax);
    barycentrics = float2(u, v);
    return true;
}
