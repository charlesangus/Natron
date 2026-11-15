import UtilityTypes;

// KernelIntersectParams in GpuTracer.cpp mirrors this. Rays first to end of a batch whose arrays hold
// stride values each are traced. With a maskStride, an instance blocks an occlusion ray only where
// the bit of the ray's light is set in the maskStride words of casterMasks from
// maskOffset + instance * maskStride.
struct IntersectParams
{
    // The top-level structure's device address as low and high words; zero for a scene with nothing in it.
    uint2 scene;
    uint stride;
    uint first;
    uint end;
    uint maskStride;
    uint maskOffset;
};

// Both entry points share one layout, because the reflection the build reads describes every global of the
// module: the occlusion entry leaves bindings 1 and 2 alone and writes 1 or 0 to the first array of binding 3,
// and only it reads bindings 4 and 5.
[[vk::binding(0, 0)]] StructuredBuffer<float> rays;
// The dense geometry index of each of the snapshot's instances.
[[vk::binding(1, 0)]] StructuredBuffer<uint> instanceGeometry;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> hitValues;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> hitIds;
// The light of each occlusion ray, and the shadow-link masks of the casters.
[[vk::binding(4, 0)]] StructuredBuffer<uint> rayLights;
[[vk::binding(5, 0)]] StructuredBuffer<uint> casterMasks;
[[vk::push_constant]] ConstantBuffer<IntersectParams> params;

// A descriptor would need a binding kind GpuKernel's storage-buffer convention lacks. The address goes
// through two words because the 64-bit form needs shaderInt64, which the device does not enable.
RaytracingAccelerationStructure sceneAt(uint2 address)
{
    return spirv_asm {
        OpCapability RayQueryKHR;
        OpExtension "SPV_KHR_ray_query";
        result:$$RaytracingAccelerationStructure = OpConvertUToAccelerationStructureKHR $address
    };
}

float rayValue(uint array, uint ray)
{
    return rays[array * params.stride + ray];
}

bool allFinite(float3 value)
{
    return all(!isinf(value) && !isnan(value));
}

// Ray queries leave non-finite origins and directions, a negative tMin and tMin above tMax undefined, where
// Embree misses or traces from behind the origin. A negative tMin is clamped to the origin.
bool readRay(uint ray, out RayDesc desc)
{
    desc.Origin = float3(rayValue(kRayOriginX, ray), rayValue(kRayOriginY, ray), rayValue(kRayOriginZ, ray));
    desc.Direction = float3(rayValue(kRayDirectionX, ray), rayValue(kRayDirectionY, ray), rayValue(kRayDirectionZ, ray));
    desc.TMin = max(rayValue(kRayTMin, ray), 0.0);
    desc.TMax = rayValue(kRayTMax, ray);

    return any(params.scene != uint2(0, 0)) && allFinite(desc.Origin) && allFinite(desc.Direction)
        && any(desc.Direction != float3(0.0)) && !isnan(desc.TMax) && desc.TMin <= desc.TMax;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void utilityIntersect(uint3 id : SV_DispatchThreadID)
{
    const uint ray = params.first + id.x;

    if (ray >= params.end)
        return;

    float t = 1.0 / 0.0;
    float2 barycentrics = float2(0.0);
    uint geometry = kNoHit;
    uint instance = kNoHit;
    uint primitive = kNoHit;
    RayDesc desc;

    if (readRay(ray, desc)) {
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;

        query.TraceRayInline(sceneAt(params.scene), RAY_FLAG_NONE, 0xff, desc);
        while (query.Proceed()) {
        }
        if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            t = query.CommittedRayT();
            barycentrics = query.CommittedTriangleBarycentrics();
            instance = query.CommittedInstanceID();
            geometry = instanceGeometry[instance];
            primitive = query.CommittedPrimitiveIndex();
        }
    }

    hitValues[kHitT * params.stride + ray] = t;
    hitValues[kHitU * params.stride + ray] = barycentrics.x;
    hitValues[kHitV * params.stride + ray] = barycentrics.y;
    hitIds[kHitGeometry * params.stride + ray] = geometry;
    hitIds[kHitInstance * params.stride + ray] = instance;
    hitIds[kHitPrimitive * params.stride + ray] = primitive;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void utilityOccluded(uint3 id : SV_DispatchThreadID)
{
    const uint ray = params.first + id.x;

    if (ray >= params.end)
        return;

    uint blocked = 0;
    RayDesc desc;

    const bool valid = readRay(ray, desc);

    if (valid && params.maskStride == 0) {
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;

        query.TraceRayInline(sceneAt(params.scene), RAY_FLAG_NONE, 0xff, desc);
        while (query.Proceed()) {
        }
        if (query.CommittedStatus() != COMMITTED_NOTHING)
            blocked = 1;
    } else if (valid) {
        // Non-opaque, so each candidate comes back here to be kept or passed through.
        RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
        const uint light = rayLights[ray];

        query.TraceRayInline(sceneAt(params.scene), RAY_FLAG_NONE, 0xff, desc);
        while (query.Proceed()) {
            const uint caster = query.CandidateInstanceID();
            const uint word = casterMasks[params.maskOffset + caster * params.maskStride + light / 32];

            if (((word >> (light % 32)) & 1) != 0) {
                query.CommitNonOpaqueTriangleHit();
                query.Abort();
            }
        }
        if (query.CommittedStatus() != COMMITTED_NOTHING)
            blocked = 1;
    }

    hitIds[ray] = blocked;
}
