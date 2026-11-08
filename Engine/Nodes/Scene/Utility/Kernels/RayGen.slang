import UtilityTypes;

// KernelRayGenParams in UtilityKernels.h mirrors this. The frustum is given as world positions on the
// near and far planes: each is the value at the window's centre plus how far it moves per unit of
// normalised device x and y. Perspective and orthographic cameras are both linear in those terms.
struct RayGenParams
{
    float4 nearAtCentre;
    float4 nearPerX;
    float4 nearPerY;
    float4 farAtCentre;
    float4 farPerX;
    float4 farPerY;
    int windowX;
    int windowY;
    int windowWidth;
    int windowHeight;
    int tileX;
    int tileY;
    int tileWidth;
    int tileHeight;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<float> rays;
[[vk::push_constant]] ConstantBuffer<RayGenParams> params;

// Rays start on the near plane and end on the far plane, so a hit is clipped to the camera's range
// without a test, and its t over the ray's tMax gives the depth.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityRayGen(uint3 id : SV_DispatchThreadID)
{
    const uint width = uint(params.tileWidth);
    const uint count = width * uint(params.tileHeight);

    if (id.x >= count)
        return;

    const int pixelX = params.tileX + int(id.x % width);
    const int pixelY = params.tileY + int(id.x / width);
    const float ndcX = 2.0 * (float(pixelX - params.windowX) + 0.5) / float(params.windowWidth) - 1.0;
    const float ndcY = 2.0 * (float(pixelY - params.windowY) + 0.5) / float(params.windowHeight) - 1.0;
    const float3 origin = params.nearAtCentre.xyz + ndcX * params.nearPerX.xyz + ndcY * params.nearPerY.xyz;
    const float3 end = params.farAtCentre.xyz + ndcX * params.farPerX.xyz + ndcY * params.farPerY.xyz;
    const float3 span = end - origin;
    const float tMax = length(span);
    const float3 direction = span / tMax;

    rays[kRayOriginX * count + id.x] = origin.x;
    rays[kRayOriginY * count + id.x] = origin.y;
    rays[kRayOriginZ * count + id.x] = origin.z;
    rays[kRayDirectionX * count + id.x] = direction.x;
    rays[kRayDirectionY * count + id.x] = direction.y;
    rays[kRayDirectionZ * count + id.x] = direction.z;
    rays[kRayTMin * count + id.x] = 0.0;
    rays[kRayTMax * count + id.x] = tMax;
}
