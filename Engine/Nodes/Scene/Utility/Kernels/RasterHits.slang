import UtilityTypes;
import RasterDraw;

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

    float3 origin;
    float3 direction;
    float tMax;
    cameraRay(params.nearAtCentre, params.nearPerX, params.nearPerY, params.farAtCentre, params.farPerX, params.farPerY,
              float2(params.jitterX, params.jitterY), int2(params.windowWidth, params.windowHeight),
              int2(column, params.batchRow + row), origin, direction, tMax);

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
        const Uint4Table tables = Uint4Table(params.tables);
        const uint4 placement = tables[1 + drawnPlacement];
        float3 corners[3];
        worldCorners(tables, params.time, placement, drawnPrimitive, corners);

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
