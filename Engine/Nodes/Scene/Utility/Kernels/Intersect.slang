import UtilityTypes;

// KernelIntersectParams in GpuTracer.cpp mirrors this. Rays first to end of a batch whose arrays hold
// stride values each are traced. With a maskStride, an instance blocks an occlusion ray only where
// the bit of the ray's light is set in the maskStride words of the casterMasks table from
// maskOffset + instance * maskStride.
//
// The instances are split over structureCount top-level structures, since one tells at most 2^24 instances
// apart. Each entry of structures holds a structure's address as low and high words and the slot of its first
// instance; a hit's slot is that plus its custom index. With an order table, slots are listed in it by
// instance; without one, a slot is its instance.
struct IntersectParams
{
    uint2 structures;
    uint2 order;
    // The shadow-link masks of the casters, which grow with the instances and the lights.
    uint2 casterMasks;
    uint structureCount;
    uint stride;
    uint first;
    uint end;
    uint maskStride;
    uint maskOffset;
};

// Both entry points share one layout, because the reflection the build reads describes every global of the
// module: the occlusion entry leaves bindings 1 and 2 alone and writes 1 or 0 to the first array of binding 3,
// and only it reads binding 4.
[[vk::binding(0, 0)]] StructuredBuffer<float> rays;
// The dense geometry index of each of the snapshot's instances.
[[vk::binding(1, 0)]] StructuredBuffer<uint> instanceGeometry;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> hitValues;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> hitIds;
// The light of each occlusion ray.
[[vk::binding(4, 0)]] StructuredBuffer<uint> rayLights;
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

uint4 structureAt(uint index)
{
    return Uint4Table(params.structures)[index];
}

uint instanceAt(uint slot)
{
    return any(params.order != uint2(0, 0)) ? UintTable(params.order)[slot] : slot;
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

    return params.structureCount != 0 && allFinite(desc.Origin) && allFinite(desc.Direction)
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
        // Each structure's hit bounds the next one's search, which leaves the closest.
        for (uint s = 0; s < params.structureCount; ++s) {
            const uint4 structure = structureAt(s);
            RayQuery<RAY_FLAG_FORCE_OPAQUE> query;

            query.TraceRayInline(sceneAt(structure.xy), RAY_FLAG_NONE, 0xff, desc);
            while (query.Proceed()) {
            }
            if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                t = query.CommittedRayT();
                desc.TMax = t;
                barycentrics = query.CommittedTriangleBarycentrics();
                instance = instanceAt(structure.z + query.CommittedInstanceID());
                geometry = instanceGeometry[instance];
                primitive = query.CommittedPrimitiveIndex();
            }
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
    for (uint s = 0; valid && blocked == 0 && s < params.structureCount; ++s) {
        const uint4 structure = structureAt(s);

        if (params.maskStride == 0) {
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;

            query.TraceRayInline(sceneAt(structure.xy), RAY_FLAG_NONE, 0xff, desc);
            while (query.Proceed()) {
            }
            if (query.CommittedStatus() != COMMITTED_NOTHING)
                blocked = 1;
        } else {
            // Non-opaque, so each candidate comes back here to be kept or passed through.
            RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
            const uint light = rayLights[ray];
            const UintTable casterMasks = UintTable(params.casterMasks);

            query.TraceRayInline(sceneAt(structure.xy), RAY_FLAG_NONE, 0xff, desc);
            while (query.Proceed()) {
                const uint caster = instanceAt(structure.z + query.CandidateInstanceID());
                const uint word = casterMasks[params.maskOffset + caster * params.maskStride + light / 32];

                if (((word >> (light % 32)) & 1) != 0) {
                    query.CommitNonOpaqueTriangleHit();
                    query.Abort();
                }
            }
            if (query.CommittedStatus() != COMMITTED_NOTHING)
                blocked = 1;
        }
    }

    hitIds[ray] = blocked;
}
