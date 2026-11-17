import UtilityTypes;
import PixelSampling;

// RayGenParams in RayGen.slang, of which a UV unwrap reads only the sampling, the window and the tile.
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

// Four float4 per pixel of the window, as WavefrontUnwrap::texels lays them out.
static const uint kTexelFloat4s = 4;

[[vk::binding(0, 0)]] RWStructuredBuffer<float> rays;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> weights;
[[vk::binding(2, 0)]] StructuredBuffer<float4> unwrapTexels;
[[vk::push_constant]] ConstantBuffer<RayGenParams> params;

// Each sample's ray crosses the surface where the sample lands on it, from the side its normal faces.
// A pixel the prim does not cover gets a ray that hits nothing.
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
    const uint texel = (uint(pixelY - params.windowY) * uint(params.windowWidth) + uint(pixelX - params.windowX)) * kTexelFloat4s;
    const float4 centre = unwrapTexels[texel];
    const float4 perX = unwrapTexels[texel + 1];
    const float4 perY = unwrapTexels[texel + 2];
    const float3 normal = unwrapTexels[texel + 3].xyz;
    const float reach = perX.w;
    const float3 surface = centre.xyz + offset.x * perX.xyz + offset.y * perY.xyz;
    const bool covered = centre.w != 0.0;
    const float3 origin = covered ? surface + reach * normal : float3(0.0);
    const float3 direction = covered ? -normal : float3(0.0, 0.0, -1.0);
    weights[id.x] = sampleWeight(t, params.samplesPerSide, params.filter);
    rays[kRayOriginX * count + id.x] = origin.x;
    rays[kRayOriginY * count + id.x] = origin.y;
    rays[kRayOriginZ * count + id.x] = origin.z;
    rays[kRayDirectionX * count + id.x] = direction.x;
    rays[kRayDirectionY * count + id.x] = direction.y;
    rays[kRayDirectionZ * count + id.x] = direction.z;
    rays[kRayTMin * count + id.x] = covered ? 0.0 : 1.0;
    rays[kRayTMax * count + id.x] = covered ? 2.0 * reach : 0.0;
}
