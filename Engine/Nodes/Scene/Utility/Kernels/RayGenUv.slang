import UtilityTypes;
import PixelSampling;

// RayGenParams in RayGen.slang, of which a UV unwrap reads only the sampling and the tile.
struct RayGenParams
{
    float3 nearAtCentre;
    float filterWidth;
    float3 nearPerX;
    uint sampleIndex;
    float3 nearPerY;
    uint seed;
    float3 farAtCentre;
    uint samplesPerSide;
    float3 farPerX;
    uint filter;
    float3 farPerY;
    uint mapping;
    int windowX;
    int windowY;
    int windowWidth;
    int windowHeight;
    int tileX;
    int tileY;
    int tileWidth;
    int tileHeight;
};

// WavefrontUnwrapGridField and kWavefrontUnwrapTriangleFloats in Wavefront.h.
static const uint kGridX = 0;
static const uint kGridY = 1;
static const uint kGridColumns = 2;
static const uint kGridRows = 3;
static const uint kGridCellSize = 4;
static const uint kGridGeometry = 5;
static const uint kGridInstance = 6;
static const uint kGridHeader = 8;
static const uint kTriangleFloat4s = 5;

// Lets a sample on an edge two triangles share land in either despite rounding.
static const float kEdgeTolerance = 1e-5;

[[vk::binding(0, 0)]] RWStructuredBuffer<float> rays;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> weights;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> hitValues;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> hitIds;
// The addresses of the unwrap's grid and of its triangles, which grow with the unwrapped mesh.
[[vk::binding(4, 0)]] StructuredBuffer<uint2> unwrapAddresses;
[[vk::push_constant]] ConstantBuffer<RayGenParams> params;

property IntTable unwrapGrid { get { return IntTable(unwrapAddresses[0]); } }
property Float4Table unwrapTriangles { get { return Float4Table(unwrapAddresses[1]); } }

// The last of the sample's grid cell's candidates under it, as the index of its triangle, or kNoHit.
uint triangleUnder(float2 position, out float2 barycentrics)
{
    barycentrics = float2(0.0);

    const int cellSize = unwrapGrid[kGridCellSize];
    const int columns = unwrapGrid[kGridColumns];
    const int rows = unwrapGrid[kGridRows];
    const float2 cell = floor((position - float2(float(unwrapGrid[kGridX]), float(unwrapGrid[kGridY]))) / float(cellSize));

    if (cell.x < 0.0 || cell.y < 0.0 || cell.x >= float(columns) || cell.y >= float(rows))
        return kNoHit;

    const uint cells = uint(columns) * uint(rows);
    const uint index = uint(cell.y) * uint(columns) + uint(cell.x);
    const int end = unwrapGrid[kGridHeader + index + 1];
    const uint listed = kGridHeader + cells + 1;
    uint found = kNoHit;

    for (int c = unwrapGrid[kGridHeader + index]; c < end; ++c) {
        const uint candidate = uint(unwrapGrid[listed + uint(c)]);
        const float4 ab = unwrapTriangles[candidate * kTriangleFloat4s];
        const float2 c2 = unwrapTriangles[candidate * kTriangleFloat4s + 1].xy;
        const float2 toB = ab.zw - ab.xy;
        const float2 toC = c2 - ab.xy;
        const float2 toSample = position - ab.xy;
        const float area = toB.x * toC.y - toB.y * toC.x;
        const float towardB = (toSample.x * toC.y - toSample.y * toC.x) / area;
        const float towardC = (toB.x * toSample.y - toB.y * toSample.x) / area;

        if (towardB >= -kEdgeTolerance && towardC >= -kEdgeTolerance && towardB + towardC <= 1.0 + kEdgeTolerance) {
            found = candidate;
            barycentrics = float2(towardB, towardC);
        }
    }

    return found;
}

// Each sample finds the triangle its own position in the tile lands on and hits that triangle there,
// tracing nothing, so that no other surface nor fold of the prim can come between. Its ray arrives along
// the normal and ends on the surface, so that a hole in the surface leaves the sample empty. A sample no
// triangle is under misses.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityRayGenUv(uint3 id : SV_DispatchThreadID)
{
    const uint width = uint(params.tileWidth);
    const uint count = width * uint(params.tileHeight);
    if (id.x >= count)
        return;
    const int pixelX = params.tileX + int(id.x % width);
    const int pixelY = params.tileY + int(id.x / width);
    const float2 t = samplePosition(pixelX, pixelY, params.samplesPerSide, params.sampleIndex, params.seed);
    const float2 offset = sampleOffset(t, params.samplesPerSide, params.filterWidth);
    const float2 position = float2(float(pixelX) + 0.5 + offset.x, float(pixelY) + 0.5 + offset.y);
    float2 barycentrics;
    const uint primitive = triangleUnder(position, barycentrics);
    const bool covered = primitive != kNoHit;
    float3 origin = float3(0.0);
    float3 direction = float3(0.0, 0.0, -1.0);
    float reach = 0.0;

    if (covered) {
        const float4 corner0 = unwrapTriangles[primitive * kTriangleFloat4s + 2];
        const float4 corner1 = unwrapTriangles[primitive * kTriangleFloat4s + 3];
        const float4 corner2 = unwrapTriangles[primitive * kTriangleFloat4s + 4];
        const float3 normal = float3(corner0.w, corner1.w, corner2.w);

        barycentrics = max(barycentrics, float2(0.0));
        barycentrics /= max(1.0, barycentrics.x + barycentrics.y);
        reach = unwrapTriangles[primitive * kTriangleFloat4s + 1].z;
        origin = corner0.xyz + barycentrics.x * (corner1.xyz - corner0.xyz) + barycentrics.y * (corner2.xyz - corner0.xyz) + reach * normal;
        direction = -normal;
    }
    weights[id.x] = sampleWeight(t, params.samplesPerSide, params.filter);
    rays[kRayOriginX * count + id.x] = origin.x;
    rays[kRayOriginY * count + id.x] = origin.y;
    rays[kRayOriginZ * count + id.x] = origin.z;
    rays[kRayDirectionX * count + id.x] = direction.x;
    rays[kRayDirectionY * count + id.x] = direction.y;
    rays[kRayDirectionZ * count + id.x] = direction.z;
    rays[kRayTMin * count + id.x] = covered ? 0.0 : 1.0;
    rays[kRayTMax * count + id.x] = covered ? reach : 0.0;
    rays[kRayTime * count + id.x] = 0.5;
    hitValues[kHitT * count + id.x] = covered ? reach : 1.0 / 0.0;
    hitValues[kHitU * count + id.x] = barycentrics.x;
    hitValues[kHitV * count + id.x] = barycentrics.y;
    hitIds[kHitGeometry * count + id.x] = covered ? uint(unwrapGrid[kGridGeometry]) : kNoHit;
    hitIds[kHitInstance * count + id.x] = covered ? uint(unwrapGrid[kGridInstance]) : kNoHit;
    hitIds[kHitPrimitive * count + id.x] = primitive;
}
