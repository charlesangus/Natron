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
//
// The instances that move or deform over the shutter are boxes instead, in one structure for each of the
// shutter's segments, whose address movers holds in the first two words of its entry. Each box bounds a mover's
// sweep over its segment, and its primitive index picks the mover's record in moverRecords from the row
// moverFirst lists for it. A ray tests the boxes of the segment its time falls in, and each box it enters at
// the mover's exact transform and points at that time.
struct IntersectParams
{
    uint2 structures;
    uint2 order;
    // The shadow-link masks of the casters, which grow with the instances and the lights.
    uint2 casterMasks;
    uint2 movers;
    uint2 moverRecords;
    uint2 moverFirst;
    uint structureCount;
    uint stride;
    uint first;
    uint end;
    uint maskStride;
    uint maskOffset;
    // Zero when nothing moves.
    uint segments;
    uint unused;
};

// A mover's record, in float4 rows bit-cast from words where they hold words: its slot, dense geometry index,
// flags and the row of its motion header; then for a rigid geometry the address of a structure holding just
// the geometry's triangles, or for a deforming one the addresses of its points, all its time steps' one after
// another, and of its triangles; then its point count; then for a deforming geometry one row per segment with
// the address of a structure of boxes, one per triangle, each bounding the triangle's sweep over the segment.
// The motion header and steps are as stepAtTime() reads them, two steps or more.
static const uint kMoverDeforms = 1;
static const uint kMoverSweptRow = 3;

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

    return (params.structureCount != 0 || params.segments != 0) && allFinite(desc.Origin) && allFinite(desc.Direction)
        && any(desc.Direction != float3(0.0)) && !isnan(desc.TMax) && desc.TMin <= desc.TMax;
}

struct MoverHit
{
    float t;
    float2 barycentrics;
    uint primitive;
};

float3 pointAt(FloatTable points, uint index)
{
    return float3(points[3 * index], points[3 * index + 1], points[3 * index + 2]);
}

// Moeller-Trumbore, with barycentrics as a ray query reports them: the hit is p0 + u (p1 - p0) + v (p2 - p0).
bool triangleHit(RayDesc ray, float3 p0, float3 p1, float3 p2, float tMax, out float t, out float2 barycentrics)
{
    t = 0.0;
    barycentrics = float2(0.0);
    const float3 e1 = p1 - p0;
    const float3 e2 = p2 - p0;
    const float3 pv = cross(ray.Direction, e2);
    const float determinant = dot(e1, pv);
    if (determinant == 0.0 || isnan(determinant))
        return false;
    const float inverse = 1.0 / determinant;
    const float3 s = ray.Origin - p0;
    const float u = dot(s, pv) * inverse;
    if (!(u >= 0.0 && u <= 1.0))
        return false;
    const float3 q = cross(s, e1);
    const float v = dot(ray.Direction, q) * inverse;
    if (!(v >= 0.0 && u + v <= 1.0))
        return false;
    t = dot(e2, q) * inverse;
    barycentrics = float2(u, v);
    return t >= ray.TMin && t <= tMax;
}

uint segmentAt(float time)
{
    uint span;
    float fraction;
    motionSpan(time, params.segments + 1, span, fraction);
    return span;
}

// The closest hit within world.TMax on a mover at the ray's time.
bool hitMover(uint mover, RayDesc world, float time, out uint slot, out uint geometry, out MoverHit hit)
{
    const Float4Table rows = Float4Table(params.moverRecords);
    const uint first = UintTable(params.moverFirst)[mover];
    const uint4 head = asuint(rows[first]);
    const uint4 where = asuint(rows[first + 1]);

    slot = head.x;
    geometry = head.y;
    hit.t = world.TMax;
    hit.barycentrics = float2(0.0);
    hit.primitive = kNoHit;

    float4 o0, o1, o2, o3, w0, w1, w2, w3;
    composeStep(stepAtTime(rows, head.w, time), o0, o1, o2, o3, w0, w1, w2, w3);
    // An affine map keeps the ray's parameter, so the object-space hit distances are the world's.
    RayDesc local;
    local.Origin = world.Origin.x * w0.xyz + world.Origin.y * w1.xyz + world.Origin.z * w2.xyz + w3.xyz;
    local.Direction = world.Direction.x * w0.xyz + world.Direction.y * w1.xyz + world.Direction.z * w2.xyz;
    local.TMin = world.TMin;
    local.TMax = world.TMax;

    if ((head.z & kMoverDeforms) == 0) {
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;

        query.TraceRayInline(sceneAt(where.xy), RAY_FLAG_NONE, 0xff, local);
        while (query.Proceed()) {
        }
        if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            return false;
        hit.t = query.CommittedRayT();
        hit.barycentrics = query.CommittedTriangleBarycentrics();
        hit.primitive = query.CommittedPrimitiveIndex();
        return true;
    }

    uint span;
    float fraction;
    motionSpan(time, params.segments + 1, span, fraction);
    const uint pointCount = asuint(rows[first + 2].x);
    const FloatTable points = FloatTable(where.xy);
    const UintTable triangles = UintTable(where.zw);
    const uint4 swept = asuint(rows[first + kMoverSweptRow + span]);
    const uint from = span * pointCount;
    const uint to = from + pointCount;
    bool found = false;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;

    query.TraceRayInline(sceneAt(swept.xy), RAY_FLAG_NONE, 0xff, local);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE)
            continue;

        const uint triangle = query.CandidatePrimitiveIndex();
        float3 corners[3];
        for (uint c = 0; c < 3; ++c) {
            const uint index = triangles[3 * triangle + c];
            corners[c] = lerp(pointAt(points, from + index), pointAt(points, to + index), fraction);
        }

        float t;
        float2 barycentrics;
        if (triangleHit(local, corners[0], corners[1], corners[2], hit.t, t, barycentrics)) {
            query.CommitProceduralPrimitiveHit(t);
            hit.t = t;
            hit.barycentrics = barycentrics;
            hit.primitive = triangle;
            found = true;
        }
    }
    return found;
}

// Leaves the closest mover hit within desc.TMax in the outputs.
void intersectMovers(inout RayDesc desc, float time, inout float t, inout float2 barycentrics, inout uint geometry,
                     inout uint instance, inout uint primitive)
{
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;

    query.TraceRayInline(sceneAt(Uint4Table(params.movers)[segmentAt(time)].xy), RAY_FLAG_NONE, 0xff, desc);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE)
            continue;

        uint slot;
        uint moverGeometry;
        MoverHit hit;
        if (hitMover(query.CandidatePrimitiveIndex(), desc, time, slot, moverGeometry, hit)) {
            query.CommitProceduralPrimitiveHit(hit.t);
            desc.TMax = hit.t;
            t = hit.t;
            barycentrics = hit.barycentrics;
            geometry = moverGeometry;
            instance = slot;
            primitive = hit.primitive;
        }
    }
}

bool casts(uint caster, uint light)
{
    const uint word = UintTable(params.casterMasks)[params.maskOffset + caster * params.maskStride + light / 32];
    return ((word >> (light % 32)) & 1) != 0;
}

bool moversBlock(RayDesc desc, float time, uint light)
{
    bool blocked = false;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;

    query.TraceRayInline(sceneAt(Uint4Table(params.movers)[segmentAt(time)].xy), RAY_FLAG_NONE, 0xff, desc);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE)
            continue;

        const uint mover = query.CandidatePrimitiveIndex();
        const uint first = UintTable(params.moverFirst)[mover];
        if (params.maskStride != 0 && !casts(asuint(Float4Table(params.moverRecords)[first].x), light))
            continue;

        uint slot;
        uint geometry;
        MoverHit hit;
        if (hitMover(mover, desc, time, slot, geometry, hit)) {
            blocked = true;
            query.Abort();
        }
    }
    return blocked;
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
        if (params.segments != 0)
            intersectMovers(desc, rayValue(kRayTime, ray), t, barycentrics, geometry, instance, primitive);
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

            query.TraceRayInline(sceneAt(structure.xy), RAY_FLAG_NONE, 0xff, desc);
            while (query.Proceed()) {
                if (casts(instanceAt(structure.z + query.CandidateInstanceID()), light)) {
                    query.CommitNonOpaqueTriangleHit();
                    query.Abort();
                }
            }
            if (query.CommittedStatus() != COMMITTED_NOTHING)
                blocked = 1;
        }
    }

    if (valid && blocked == 0 && params.segments != 0
        && moversBlock(desc, rayValue(kRayTime, ray), params.maskStride != 0 ? rayLights[ray] : 0))
        blocked = 1;

    hitIds[ray] = blocked;
}
