import UtilityTypes;
import PixelSampling;

// KernelRayGenParams in UtilityKernels.h mirrors this. The frustum is given as world positions on the
// near and far planes: each is the value at the window's centre plus how far it moves per unit of
// normalised device x and y. Perspective and orthographic cameras are both linear in those terms.
//
// A pixel takes samplesPerSide squared samples, one per call, over a square filterWidth pixels wide
// centred on it; sampleIndex says which.
//
// mapping picks the projection. The spherical and cylindrical ones reuse the frustum fields: nearAtCentre
// is the camera's position, nearPerX, nearPerY and farAtCentre its unit right, up and forward axes,
// farPerX holds the near and far distances, and farPerY the vertical range of a cylindrical projection,
// as slopes for a perspective camera or heights for an orthographic one, with z set for orthographic.
// Their window is the display window, which spans 0 to 1 of the projection; overscan lies beyond it.
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

static const float kPi = 3.14159265358979;

// KernelRayMapping in UtilityKernels.h.
static const uint kMappingCamera = 0;
static const uint kMappingSpherical = 1;
static const uint kMappingCylindrical = 2;

[[vk::binding(0, 0)]] RWStructuredBuffer<float> rays;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> weights;
[[vk::push_constant]] ConstantBuffer<RayGenParams> params;

struct CameraRay
{
    float3 origin;
    float3 direction;
    float tMax;
};

// Rays start on the near plane and end on the far plane, so a hit is clipped to the camera's range
// without a test, and its t over the ray's tMax gives the depth.
CameraRay frustumRay(int pixelX, int pixelY, float2 offset)
{
    const float ndcX = 2.0 * (float(pixelX - params.windowX) + 0.5 + offset.x) / float(params.windowWidth) - 1.0;
    const float ndcY = 2.0 * (float(pixelY - params.windowY) + 0.5 + offset.y) / float(params.windowHeight) - 1.0;
    const float3 origin = params.nearAtCentre + ndcX * params.nearPerX + ndcY * params.nearPerY;
    const float3 end = params.farAtCentre + ndcX * params.farPerX + ndcY * params.farPerY;
    const float3 span = end - origin;
    CameraRay ray;
    ray.tMax = length(span);
    ray.origin = origin;
    ray.direction = span / ray.tMax;
    return ray;
}

// The rays of the other projections run from the near to the far distance, measured along each ray,
// so that the shading's depth reads as the distance from the camera.
CameraRay mappedRay(int pixelX, int pixelY, float2 offset)
{
    const float u = (float(pixelX - params.windowX) + 0.5 + offset.x) / float(params.windowWidth);
    const float v = (float(pixelY - params.windowY) + 0.5 + offset.y) / float(params.windowHeight);
    const float longitude = 2.0 * kPi * (u - 0.5);
    const float3 right = params.nearPerX;
    const float3 up = params.nearPerY;
    const float3 forward = params.farAtCentre;
    const float3 around = sin(longitude) * right + cos(longitude) * forward;
    float3 apex = params.nearAtCentre;
    float3 direction;
    if (params.mapping == kMappingSpherical) {
        const float latitude = kPi * (v - 0.5);
        direction = cos(latitude) * around + sin(latitude) * up;
    } else {
        const float height = lerp(params.farPerY.x, params.farPerY.y, v);
        if (params.farPerY.z != 0.0) {
            apex += height * up;
            direction = around;
        } else {
            direction = normalize(around + height * up);
        }
    }
    const float nearDistance = params.farPerX.x;
    const float farDistance = params.farPerX.y;
    CameraRay ray;
    ray.origin = apex + nearDistance * direction;
    ray.direction = direction;
    ray.tMax = farDistance - nearDistance;
    return ray;
}

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
    const float2 t = samplePosition(pixelX, pixelY, params.samplesPerSide, params.sampleIndex, params.seed);
    const float2 offset = sampleOffset(t, params.samplesPerSide, params.filterWidth);
    const CameraRay ray = params.mapping == kMappingCamera ? frustumRay(pixelX, pixelY, offset) : mappedRay(pixelX, pixelY, offset);
    weights[id.x] = sampleWeight(t, params.samplesPerSide, params.filter);
    rays[kRayOriginX * count + id.x] = ray.origin.x;
    rays[kRayOriginY * count + id.x] = ray.origin.y;
    rays[kRayOriginZ * count + id.x] = ray.origin.z;
    rays[kRayDirectionX * count + id.x] = ray.direction.x;
    rays[kRayDirectionY * count + id.x] = ray.direction.y;
    rays[kRayDirectionZ * count + id.x] = ray.direction.z;
    rays[kRayTMin * count + id.x] = 0.0;
    rays[kRayTMax * count + id.x] = ray.tMax;
}
