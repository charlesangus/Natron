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
// A camera that moves over the shutter: x of the first row holds its time steps, none or one for a camera
// that does not, and kCameraRowsPerStep rows follow per step: the frustum fields in the camera's view space,
// as rows nearAtCentre, nearPerX, nearPerY, farAtCentre, farPerX, farPerY, then its view-to-world
// transform as the motion rows of UtilityTypes.slang hold one.
[[vk::binding(2, 0)]] StructuredBuffer<float4> cameraSteps;
[[vk::push_constant]] ConstantBuffer<RayGenParams> params;

static const uint kCameraRowsPerStep = 6 + kMotionRowsPerStep;

struct Frustum
{
    float3 nearAtCentre;
    float3 nearPerX;
    float3 nearPerY;
    float3 farAtCentre;
    float3 farPerX;
    float3 farPerY;
};

Frustum paramsFrustum()
{
    Frustum f;
    f.nearAtCentre = params.nearAtCentre;
    f.nearPerX = params.nearPerX;
    f.nearPerY = params.nearPerY;
    f.farAtCentre = params.farAtCentre;
    f.farPerX = params.farPerX;
    f.farPerY = params.farPerY;
    return f;
}

Frustum stepFrustum(uint first)
{
    Frustum f;
    f.nearAtCentre = cameraSteps[first].xyz;
    f.nearPerX = cameraSteps[first + 1].xyz;
    f.nearPerY = cameraSteps[first + 2].xyz;
    f.farAtCentre = cameraSteps[first + 3].xyz;
    f.farPerX = cameraSteps[first + 4].xyz;
    f.farPerY = cameraSteps[first + 5].xyz;
    return f;
}

MotionStep cameraStep(uint first)
{
    MotionStep step;
    step.stretchTop = cameraSteps[first + 6].xyz;
    step.stretchRest = cameraSteps[first + 7].xyz;
    step.translation = cameraSteps[first + 8].xyz;
    step.rotation = cameraSteps[first + 9];
    return step;
}

// The time stratum of a pixel's sample: consecutive samples step through the strata by about the golden
// fraction of them, a stride coprime with their count, so that any run of passes spreads over the
// shutter; each pixel starts at its own stratum.
float sampleTime(int pixelX, int pixelY, uint samplesPerSide, uint sampleIndex, uint seed)
{
    const uint n = max(samplesPerSide, 1u);
    const uint count = n * n;
    const uint pixelHash = pcgHash(uint(pixelY) ^ pcgHash(uint(pixelX) ^ pcgHash(seed ^ 0x5bd1e995u)));
    uint stride = max(1u, uint(float(count) * 0.6180339887 + 0.5));
    while (count > 1u) {
        uint a = stride;
        uint b = count;
        while (b != 0u) {
            const uint r = a % b;
            a = b;
            b = r;
        }
        if (a == 1u)
            break;
        ++stride;
    }
    const uint stratum = (sampleIndex * stride + pixelHash) % count;
    const float jitter = unitFloat(pcgHash(pixelHash ^ (sampleIndex * 0x9e3779b9u)));
    return (float(stratum) + jitter) / float(count);
}

struct CameraRay
{
    float3 origin;
    float3 direction;
    float tMax;
};

// Rays start on the near plane and end on the far plane, so a hit is clipped to the camera's range
// without a test, and its t over the ray's tMax gives the depth.
CameraRay frustumRay(Frustum frustum, int pixelX, int pixelY, float2 offset)
{
    const float ndcX = 2.0 * (float(pixelX - params.windowX) + 0.5 + offset.x) / float(params.windowWidth) - 1.0;
    const float ndcY = 2.0 * (float(pixelY - params.windowY) + 0.5 + offset.y) / float(params.windowHeight) - 1.0;
    const float3 origin = frustum.nearAtCentre + ndcX * frustum.nearPerX + ndcY * frustum.nearPerY;
    const float3 end = frustum.farAtCentre + ndcX * frustum.farPerX + ndcY * frustum.farPerY;
    const float3 span = end - origin;
    CameraRay ray;
    ray.tMax = length(span);
    ray.origin = origin;
    ray.direction = span / ray.tMax;
    return ray;
}

// The rays of the other projections run from the near to the far distance, measured along each ray,
// so that the shading's depth reads as the distance from the camera.
CameraRay mappedRay(Frustum frustum, int pixelX, int pixelY, float2 offset)
{
    const float u = (float(pixelX - params.windowX) + 0.5 + offset.x) / float(params.windowWidth);
    const float v = (float(pixelY - params.windowY) + 0.5 + offset.y) / float(params.windowHeight);
    const float longitude = 2.0 * kPi * (u - 0.5);
    const float3 right = frustum.nearPerX;
    const float3 up = frustum.nearPerY;
    const float3 forward = frustum.farAtCentre;
    const float3 around = sin(longitude) * right + cos(longitude) * forward;
    float3 apex = frustum.nearAtCentre;
    float3 direction;
    if (params.mapping == kMappingSpherical) {
        const float latitude = kPi * (v - 0.5);
        direction = cos(latitude) * around + sin(latitude) * up;
    } else {
        const float height = lerp(frustum.farPerY.x, frustum.farPerY.y, v);
        if (frustum.farPerY.z != 0.0) {
            apex += height * up;
            direction = around;
        } else {
            direction = normalize(around + height * up);
        }
    }
    const float nearDistance = frustum.farPerX.x;
    const float farDistance = frustum.farPerX.y;
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
    const float time = sampleTime(pixelX, pixelY, params.samplesPerSide, params.sampleIndex, params.seed);
    const uint cameraStepCount = asuint(cameraSteps[0].x);
    CameraRay ray;
    if (cameraStepCount <= 1) {
        const Frustum frustum = paramsFrustum();
        ray = params.mapping == kMappingCamera ? frustumRay(frustum, pixelX, pixelY, offset) : mappedRay(frustum, pixelX, pixelY, offset);
    } else {
        // The camera's frustum and its place at the ray's time, from the steps either side of it.
        uint span;
        float fraction;
        motionSpan(time, cameraStepCount, span, fraction);
        const uint first = 1 + span * kCameraRowsPerStep;
        const uint next = first + kCameraRowsPerStep;
        const Frustum a = stepFrustum(first);
        const Frustum b = stepFrustum(next);
        Frustum frustum;
        frustum.nearAtCentre = lerp(a.nearAtCentre, b.nearAtCentre, fraction);
        frustum.nearPerX = lerp(a.nearPerX, b.nearPerX, fraction);
        frustum.nearPerY = lerp(a.nearPerY, b.nearPerY, fraction);
        frustum.farAtCentre = lerp(a.farAtCentre, b.farAtCentre, fraction);
        frustum.farPerX = lerp(a.farPerX, b.farPerX, fraction);
        frustum.farPerY = lerp(a.farPerY, b.farPerY, fraction);
        const CameraRay local = params.mapping == kMappingCamera ? frustumRay(frustum, pixelX, pixelY, offset) : mappedRay(frustum, pixelX, pixelY, offset);
        float4 o0;
        float4 o1;
        float4 o2;
        float4 o3;
        float4 w0;
        float4 w1;
        float4 w2;
        float4 w3;
        composeStep(blendSteps(cameraStep(first), cameraStep(next), fraction), o0, o1, o2, o3, w0, w1, w2, w3);
        const float3 origin = transformRow(float4(local.origin, 1.0), o0, o1, o2, o3).xyz;
        const float3 end = transformRow(float4(local.origin + local.tMax * local.direction, 1.0), o0, o1, o2, o3).xyz;
        const float3 span3 = end - origin;
        ray.origin = origin;
        ray.tMax = length(span3);
        ray.direction = span3 / ray.tMax;
    }
    weights[id.x] = sampleWeight(t, params.samplesPerSide, params.filter);
    rays[kRayOriginX * count + id.x] = ray.origin.x;
    rays[kRayOriginY * count + id.x] = ray.origin.y;
    rays[kRayOriginZ * count + id.x] = ray.origin.z;
    rays[kRayDirectionX * count + id.x] = ray.direction.x;
    rays[kRayDirectionY * count + id.x] = ray.direction.y;
    rays[kRayDirectionZ * count + id.x] = ray.direction.z;
    rays[kRayTMin * count + id.x] = 0.0;
    rays[kRayTMax * count + id.x] = ray.tMax;
    rays[kRayTime * count + id.x] = time;
}
