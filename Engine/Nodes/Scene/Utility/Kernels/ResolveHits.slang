import UtilityTypes;
import Surface;

// KernelResolveParams in UtilityKernels.h mirrors this. pixelFootprint is ShadeParams', for camera rays.
struct ResolveParams
{
    float4 pixelFootprint;
    uint count;
    uint flags;
    // The 32-bit words of each link mask.
    uint linkWords;
    uint padding;
};

// ResolveParams::flags.
// Pending rays take their hit from nextHitSurface and nextHitIds; otherwise every ray's hit is new.
static const uint kResolveMerge = 1;
// The last pass: a hit still on no surface becomes a miss rather than being traced past again.
static const uint kResolveLast = 2;
// The rays are shadow rays, whose verdict goes to blocked.
static const uint kResolveShadow = 4;
// Shadow rays pass through the casters their light is not shadow-linked to.
static const uint kResolveShadowLinked = 8;

[[vk::binding(0, 0)]] StructuredBuffer<float> rays;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> hitSurface;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> hitIds;
[[vk::binding(3, 0)]] StructuredBuffer<float> nextHitSurface;
[[vk::binding(4, 0)]] StructuredBuffer<uint> nextHitIds;
// The rays of the next trace: a pending ray again from past its hit, the others only a negative tMax,
// which the tracers skip whatever the rest holds and which tells the next pass the ray is settled.
[[vk::binding(5, 0)]] RWStructuredBuffer<float> retrace;
// For shadow rays, 1 where a surface is in the way, as Tracer::occluded writes it.
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> blocked;
[[vk::binding(7, 0)]] StructuredBuffer<uint> rayLights;
[[vk::binding(8, 0)]] StructuredBuffer<GeometryRecord> geometries;
[[vk::binding(9, 0)]] StructuredBuffer<float4> colors;
[[vk::binding(10, 0)]] StructuredBuffer<int> triangles;
[[vk::binding(11, 0)]] StructuredBuffer<MaterialRecord> materials;
[[vk::binding(12, 0)]] StructuredBuffer<TextureBinding> textureBindings;
[[vk::binding(13, 0)]] StructuredBuffer<uint4> images;
[[vk::binding(14, 0)]] StructuredBuffer<float4> texels;
[[vk::binding(15, 0)]] StructuredBuffer<ProjectionRecord> projections;
[[vk::binding(16, 0)]] StructuredBuffer<float4> points;
[[vk::binding(17, 0)]] StructuredBuffer<float4> instanceTransforms;
[[vk::binding(18, 0)]] StructuredBuffer<float2> sts;
[[vk::binding(19, 0)]] StructuredBuffer<uint> linkMasks;
// For shadow rays, what each adds to its pixel unblocked, dimmed by each transmissive surface it goes through.
[[vk::binding(20, 0)]] RWStructuredBuffer<float4> shadowContributions;
[[vk::push_constant]] ConstantBuffer<ResolveParams> params;

float rayValue(uint array, uint ray)
{
    return rays[array * params.count + ray];
}

void copyRay(uint ray, float tMin, float tMax)
{
    const uint count = params.count;

    for (uint a = 0; a < kRayTMin; ++a)
        retrace[a * count + ray] = rays[a * count + ray];
    retrace[kRayTMin * count + ray] = tMin;
    retrace[kRayTMax * count + ray] = tMax;
}

void writeMiss(uint ray)
{
    const uint count = params.count;

    hitSurface[kHitT * count + ray] = 1.0 / 0.0;
    hitSurface[kHitU * count + ray] = 0.0;
    hitSurface[kHitV * count + ray] = 0.0;
    hitIds[kHitGeometry * count + ray] = kNoHit;
    hitIds[kHitInstance * count + ray] = kNoHit;
    hitIds[kHitPrimitive * count + ray] = kNoHit;
}

bool shadowLinked(uint instance, uint light)
{
    const uint words = params.linkWords;

    return ((linkMasks[(2 * instance + 1) * words + light / 32] >> (light % 32)) & 1) != 0;
}

// Whether the ray's hit is one the ray goes on through: a surface's hole, or for a shadow ray a caster
// its light ignores.
bool passesThrough(uint ray, uint geometryIndex)
{
    const uint count = params.count;
    const uint instance = hitIds[kHitInstance * count + ray];

    if ((params.flags & kResolveShadowLinked) != 0 && !shadowLinked(instance, rayLights[ray]))
        return true;

    const GeometryRecord geometry = geometries[geometryIndex];
    const MaterialRecord material = materials[geometry.materialIndex];

    if (!mayLeaveHoles(material))
        return false;

    const float t = hitSurface[kHitT * count + ray];
    const float3 origin = float3(rayValue(kRayOriginX, ray), rayValue(kRayOriginY, ray), rayValue(kRayOriginZ, ray));
    const float3 direction = float3(rayValue(kRayDirectionX, ray), rayValue(kRayDirectionY, ray), rayValue(kRayDirectionZ, ray));
    float width = 0.0;

    if ((params.flags & kResolveShadow) == 0)
        width = lerp(params.pixelFootprint.x, params.pixelFootprint.y, t / rayValue(kRayTMax, ray));

    return surfaceAbsent(colors, triangles, points, instanceTransforms, sts, textureBindings, images, texels, projections, geometry, material,
                         instance, hitIds[kHitPrimitive * count + ray], hitSurface[kHitU * count + ray], hitSurface[kHitV * count + ray],
                         origin + t * direction, direction, width);
}

// For a shadow ray, whether its hit lets some of the light through, which then dims its contribution.
bool transmits(uint ray, uint geometryIndex)
{
    const uint count = params.count;
    const GeometryRecord geometry = geometries[geometryIndex];
    const MaterialRecord material = materials[geometry.materialIndex];
    const float3 transmission = surfaceTransmission(colors, triangles, sts, textureBindings, images, texels, geometry, material,
                                                    hitIds[kHitPrimitive * count + ray], hitSurface[kHitU * count + ray], hitSurface[kHitV * count + ray]);

    if (!any(transmission > float3(0.0)))
        return false;
    shadowContributions[ray] = float4(shadowContributions[ray].xyz * transmission, shadowContributions[ray].w);
    return true;
}

// One pass over a batch's hits: each hit on a hole is queued to be traced again from just past it, so
// that after a pass per hole the hits are those of the surfaces the rays really meet. A shadow ray goes
// on through transmissive surfaces too, its light dimmed by each.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityResolveHits(uint3 id : SV_DispatchThreadID)
{
    const uint count = params.count;
    const uint ray = id.x;

    if (ray >= count)
        return;

    if ((params.flags & kResolveMerge) != 0) {
        if (!(retrace[kRayTMax * count + ray] >= 0.0))
            return;
        for (uint a = 0; a < 3; ++a) {
            hitSurface[a * count + ray] = nextHitSurface[a * count + ray];
            hitIds[a * count + ray] = nextHitIds[a * count + ray];
        }
    }

    const bool shadow = (params.flags & kResolveShadow) != 0;
    const uint geometryIndex = hitIds[kHitGeometry * count + ray];

    if (geometryIndex != kNoHit && (passesThrough(ray, geometryIndex) || (shadow && transmits(ray, geometryIndex)))) {
        if ((params.flags & kResolveLast) != 0) {
            writeMiss(ray);
        } else {
            const float t = hitSurface[kHitT * count + ray];
            const float3 origin = float3(rayValue(kRayOriginX, ray), rayValue(kRayOriginY, ray), rayValue(kRayOriginZ, ray));
            const float3 direction = float3(rayValue(kRayDirectionX, ray), rayValue(kRayDirectionY, ray), rayValue(kRayDirectionZ, ray));
            const float3 scaled = abs(origin + t * direction);
            // Far enough past the hit that the tracer's rounding cannot find the same triangle again.
            const float step = 1e-5 * max(1.0, max(t, max(scaled.x, max(scaled.y, scaled.z))));

            copyRay(ray, t + step, rayValue(kRayTMax, ray));
            if (shadow)
                blocked[ray] = 0;
            return;
        }
    }

    retrace[kRayTMin * count + ray] = 0.0;
    retrace[kRayTMax * count + ray] = -1.0;
    if (shadow)
        blocked[ray] = hitIds[kHitGeometry * count + ray] != kNoHit ? 1 : 0;
}
