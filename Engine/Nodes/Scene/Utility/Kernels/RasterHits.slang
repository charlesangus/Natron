import UtilityTypes;

// KernelRasterHitsParams in UtilityKernels.h mirrors this. One dispatch covers one tile of a batch: a rectangle of
// the visibility buffer RasterVisibility drew, tileWidth by tileHeight texels, the first of them the pixel tileX
// columns into the window and tileY rows into the batch. The batch spans the window's width from row batchRow on,
// and its arrays hold count values each, a pixel's at its row in the batch times windowWidth plus its column.
//
// Every pass shares one sub-pixel offset, jitter, and one time, so each pixel's camera ray goes through its
// centre plus jitter, as RayGen's frustumRay() makes it, and the visibility buffer drew with the same offset.
//
// tables is GpuTracer::RasterScene::hitTables, in uint4 rows: a header holding the address of the transforms;
// then one row per placement, its dense geometry index, the first row of its transform and the row of its
// geometry's record; then two rows per drawn geometry, the addresses of its points and of its triangles, then
// its point count and point steps.
struct RasterHitsParams
{
    float3 nearAtCentre;
    float time;
    float3 nearPerX;
    float weight;
    float3 nearPerY;
    float jitterX;
    float3 farAtCentre;
    float jitterY;
    float3 farPerX;
    uint count;
    float3 farPerY;
    int batchRow;
    int windowWidth;
    int windowHeight;
    int tileX;
    int tileY;
    int tileWidth;
    int tileHeight;
    uint2 tables;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<float> rays;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> weights;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> hitValues;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> hitIds;
// The tile's texels in rows, two words each: the placement and the triangle, or kNoHit where nothing was drawn.
[[vk::binding(4, 0)]] StructuredBuffer<uint> visibility;
[[vk::push_constant]] ConstantBuffer<RasterHitsParams> params;

float3 pointAt(FloatTable points, uint index)
{
    return float3(points[3 * index], points[3 * index + 1], points[3 * index + 2]);
}

// The triangle's corners in world space at the pass's time, as visibilityVertex() places them.
void worldCorners(uint4 placement, uint primitive, out float3 corners[3])
{
    const Uint4Table tables = Uint4Table(params.tables);
    const uint4 header = tables[0];
    const uint4 where = tables[placement.z];
    const uint4 counts = tables[placement.z + 1];
    const FloatTable points = FloatTable(where.xy);
    const UintTable triangles = UintTable(where.zw);
    const Float4Table rows = Float4Table(header.xy);
    const uint first = placement.y;

    float4 o0, o1, o2, o3;
    if (asuint(rows[first].x) == 0) {
        o0 = rows[first + 1];
        o1 = rows[first + 2];
        o2 = rows[first + 3];
        o3 = rows[first + 4];
    } else {
        float4 w0, w1, w2, w3;
        composeStep(stepAtTime(rows, first, params.time), o0, o1, o2, o3, w0, w1, w2, w3);
    }

    uint span = 0;
    float fraction = 0.0;
    if (counts.y > 1)
        motionSpan(params.time, counts.y, span, fraction);
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
bool planeHit(float3 origin, float3 direction, float tMax, float3 p0, float3 p1, float3 p2, out float t, out float2 barycentrics)
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

[shader("compute")]
[numthreads(64, 1, 1)]
void utilityRasterHits(uint3 id : SV_DispatchThreadID)
{
    const uint width = uint(params.tileWidth);
    if (id.x >= width * uint(params.tileHeight))
        return;
    const int column = params.tileX + int(id.x % width);
    const int row = params.tileY + int(id.x / width);
    const uint ray = uint(row) * uint(params.windowWidth) + uint(column);
    const uint count = params.count;

    const float ndcX = 2.0 * (float(column) + 0.5 + params.jitterX) / float(params.windowWidth) - 1.0;
    const float ndcY = 2.0 * (float(params.batchRow + row) + 0.5 + params.jitterY) / float(params.windowHeight) - 1.0;
    const float3 origin = params.nearAtCentre + ndcX * params.nearPerX + ndcY * params.nearPerY;
    const float3 end = params.farAtCentre + ndcX * params.farPerX + ndcY * params.farPerY;
    const float3 span = end - origin;
    const float tMax = length(span);
    const float3 direction = span / tMax;

    weights[ray] = params.weight;
    rays[kRayOriginX * count + ray] = origin.x;
    rays[kRayOriginY * count + ray] = origin.y;
    rays[kRayOriginZ * count + ray] = origin.z;
    rays[kRayDirectionX * count + ray] = direction.x;
    rays[kRayDirectionY * count + ray] = direction.y;
    rays[kRayDirectionZ * count + ray] = direction.z;
    rays[kRayTMin * count + ray] = 0.0;
    rays[kRayTMax * count + ray] = tMax;
    rays[kRayTime * count + ray] = params.time;

    float t = 1.0 / 0.0;
    float2 barycentrics = float2(0.0);
    uint geometry = kNoHit;
    uint instance = kNoHit;
    uint primitive = kNoHit;
    const uint drawnPlacement = visibility[2 * id.x];
    const uint drawnPrimitive = visibility[2 * id.x + 1];

    if (drawnPlacement != kNoHit) {
        const uint4 placement = Uint4Table(params.tables)[1 + drawnPlacement];
        float3 corners[3];
        worldCorners(placement, drawnPrimitive, corners);

        float hitT;
        float2 hitBarycentrics;
        if (planeHit(origin, direction, tMax, corners[0], corners[1], corners[2], hitT, hitBarycentrics)) {
            t = hitT;
            barycentrics = hitBarycentrics;
            geometry = placement.x;
            instance = drawnPlacement;
            primitive = drawnPrimitive;
        }
    }

    hitValues[kHitT * count + ray] = t;
    hitValues[kHitU * count + ray] = barycentrics.x;
    hitValues[kHitV * count + ray] = barycentrics.y;
    hitIds[kHitGeometry * count + ray] = geometry;
    hitIds[kHitInstance * count + ray] = instance;
    hitIds[kHitPrimitive * count + ray] = primitive;
}
