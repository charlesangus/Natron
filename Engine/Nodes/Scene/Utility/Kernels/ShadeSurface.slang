import UtilityTypes;
import Texture;
import PreviewSurface;
import Project3D;
import Lights;
import Surface;

// KernelShadeParams in UtilityKernels.h mirrors this. projection holds the row-vector projection terms
// that decide clip z and w from view z: clip z = A * z + B, clip w = C * z + D. pixelFootprint holds a
// pixel's world width on the near plane and on the far plane. lighting: the first light this pass
// samples, how many it samples, the 32-bit words per link mask, and the scene's light count. pixels: the
// frame position of the first hit's pixel, the width of the rows the hits cover, and the sample seed.
// bounce: the bounce the pass shades, 0 for camera rays; the bounces the frame traces in all; the most
// a path takes by reflection and by refraction.
struct ShadeParams
{
    float4 projection;
    float viewZAtNear;
    float viewZAtFar;
    uint count;
    uint padding;
    float4 pixelFootprint;
    uint4 lighting;
    uint4 pixels;
    uint4 bounce;
};

[[vk::binding(0, 0)]] StructuredBuffer<float> rays;
[[vk::binding(1, 0)]] StructuredBuffer<float> hitSurface;
[[vk::binding(2, 0)]] StructuredBuffer<uint> hitIds;
[[vk::binding(3, 0)]] StructuredBuffer<GeometryRecord> geometries;
[[vk::binding(4, 0)]] StructuredBuffer<float4> colors;
[[vk::binding(5, 0)]] StructuredBuffer<int> triangles;
[[vk::binding(6, 0)]] StructuredBuffer<int> instanceIds;
[[vk::binding(7, 0)]] RWStructuredBuffer<float4> colorOut;
[[vk::binding(8, 0)]] RWStructuredBuffer<float> depthOut;
[[vk::binding(9, 0)]] RWStructuredBuffer<int> primIdOut;
[[vk::binding(10, 0)]] RWStructuredBuffer<int> instanceIdOut;
[[vk::binding(11, 0)]] StructuredBuffer<MaterialRecord> materials;
[[vk::binding(12, 0)]] StructuredBuffer<TextureBinding> textureBindings;
[[vk::binding(13, 0)]] StructuredBuffer<uint4> images;
[[vk::binding(14, 0)]] StructuredBuffer<float4> texels;
[[vk::binding(15, 0)]] StructuredBuffer<ProjectionRecord> projections;
[[vk::binding(16, 0)]] StructuredBuffer<float4> points;
[[vk::binding(17, 0)]] StructuredBuffer<float4> instanceTransforms;
[[vk::binding(18, 0)]] StructuredBuffer<float2> sts;
[[vk::binding(19, 0)]] StructuredBuffer<LightRecord> lights;
[[vk::binding(20, 0)]] StructuredBuffer<float> lightData;
// Per instance, the light-link mask then the shadow-link mask, lighting.z words each; bit l is light l.
[[vk::binding(21, 0)]] StructuredBuffer<uint> linkMasks;
// One shadow ray per hit per light of the pass, light-major: eight arrays of lightBatch * count values.
[[vk::binding(22, 0)]] RWStructuredBuffer<float> shadowRays;
// What each shadow ray's light adds to its pixel when the ray is not blocked, premultiplied.
[[vk::binding(23, 0)]] RWStructuredBuffer<float4> shadowContributions;
[[vk::binding(24, 0)]] RWStructuredBuffer<uint> shadowLights;
// GeometryRecord::normalBinding indexes these; w is unused.
[[vk::binding(25, 0)]] StructuredBuffer<float4> normals;
// After the first bounce, what each ray's path carries.
[[vk::binding(26, 0)]] StructuredBuffer<PathState> paths;
// Each ray's continuation, count rays in eight arrays with a negative tMax where the path ends, and what
// it carries; written only when a bounce follows this one.
[[vk::binding(27, 0)]] RWStructuredBuffer<float> nextRays;
[[vk::binding(28, 0)]] RWStructuredBuffer<PathState> nextPaths;
[[vk::push_constant]] ConstantBuffer<ShadeParams> params;

static const float4 kUnsupportedColor = float4(1.0, 0.0, 1.0, 1.0);
// lightRandom()'s light numbers for what is not a light: choosing a hit's continuation and drawing its
// direction.
static const uint kRandomLobeChoice = 0xfffffff0u;
static const uint kRandomLobeSample = 0xfffffff1u;
// The kinds of continuation, for the depth each counts against.
static const uint kBounceReflection = 0;
static const uint kBounceRefraction = 1;
static const uint kBounceStraight = 2;

// Window depth, the convention the host reads every delegate's depth AOV in: 0 at the near plane, 1 at
// the far plane, through the camera's projection. View z is linear along a ray, projected depth is not.
float windowDepth(float alongRay)
{
    const float viewZ = params.viewZAtNear + alongRay * (params.viewZAtFar - params.viewZAtNear);
    const float clipZ = params.projection.x * viewZ + params.projection.y;
    const float clipW = params.projection.z * viewZ + params.projection.w;

    return 0.5 * clipZ / clipW + 0.5;
}

float3 colorInput(MaterialRecord material, uint slot, float3 constant, bool hasSt, float2 st, float footprint)
{
    const int binding = textureOf(material, slot);
    return binding == kNoTexture ? constant : readTexture(textureBindings, images, texels, binding, hasSt, st, footprint).xyz;
}

float floatInput(MaterialRecord material, uint slot, float constant, bool hasSt, float2 st, float footprint)
{
    const int binding = textureOf(material, slot);
    return binding == kNoTexture ? constant : readTexture(textureBindings, images, texels, binding, hasSt, st, footprint).x;
}

// The pixel's width where the ray meets the surface, at alongRay of the way from the near plane to the
// far plane.
float pixelWidth(float alongRay)
{
    return lerp(params.pixelFootprint.x, params.pixelFootprint.y, alongRay);
}

PathState pathOf(uint index)
{
    if (params.bounce.x == 0) {
        PathState path;
        path.throughput = float4(1.0, 1.0, 1.0, 1.0);
        path.counters = uint4(index, 0, 0, 0);
        return path;
    }
    return paths[index];
}

// Every bounce draws its own light samples and directions.
uint bounceSeed()
{
    return params.pixels.w ^ (params.bounce.x * 0x632be5abu);
}

uint pixelKey(PathState path)
{
    const uint index = path.counters.x;
    const uint pixelX = params.pixels.x + index % params.pixels.z;
    const uint pixelY = params.pixels.y + index / params.pixels.z;

    return (pixelY << 16) ^ pixelX;
}

bool writesContinuations()
{
    return params.lighting.x == 0 && params.bounce.x < params.bounce.y;
}

// Whether the path may still reflect, which is then how it sees the dome's specular highlights.
bool mayReflect(PathState path)
{
    return path.counters.y < params.bounce.y && path.counters.z < params.bounce.z;
}

void writeContinuation(uint index, float3 origin, float3 direction, float tMax, PathState path)
{
    const uint n = params.count;

    nextRays[kRayOriginX * n + index] = origin.x;
    nextRays[kRayOriginY * n + index] = origin.y;
    nextRays[kRayOriginZ * n + index] = origin.z;
    nextRays[kRayDirectionX * n + index] = direction.x;
    nextRays[kRayDirectionY * n + index] = direction.y;
    nextRays[kRayDirectionZ * n + index] = direction.z;
    nextRays[kRayTMin * n + index] = 0.0;
    nextRays[kRayTMax * n + index] = tMax;
    nextPaths[index] = path;
}

void endPath(uint index, PathState path)
{
    if (writesContinuations())
        writeContinuation(index, float3(0.0), float3(0.0, 0.0, 1.0), -1.0, path);
}

// Camera rays write the pixel; later bounces add to it what their path carries.
void addColor(uint index, PathState path, float4 value)
{
    if (params.bounce.x == 0)
        colorOut[index] = value;
    else
        colorOut[index] += float4(path.throughput.xyz * value.xyz, path.throughput.w * value.w);
}

void writeShadowRay(uint slot, uint light, float3 origin, float3 direction, float tMax, float3 contribution)
{
    const uint n = params.lighting.y * params.count;

    shadowRays[kRayOriginX * n + slot] = origin.x;
    shadowRays[kRayOriginY * n + slot] = origin.y;
    shadowRays[kRayOriginZ * n + slot] = origin.z;
    shadowRays[kRayDirectionX * n + slot] = direction.x;
    shadowRays[kRayDirectionY * n + slot] = direction.y;
    shadowRays[kRayDirectionZ * n + slot] = direction.z;
    shadowRays[kRayTMin * n + slot] = 0.0;
    shadowRays[kRayTMax * n + slot] = tMax;
    shadowContributions[slot] = float4(contribution, 0.0);
    shadowLights[slot] = light;
}

// A shadow ray the tracers skip, so the contribution is added unblocked.
void writeUnshadowed(uint slot, uint light, float3 contribution)
{
    writeShadowRay(slot, light, float3(0.0), float3(0.0, 0.0, 1.0), -1.0, contribution);
}

void writeNoShadows(uint index)
{
    for (uint j = 0; j < params.lighting.y; ++j)
        writeUnshadowed(j * params.count + index, params.lighting.x + j, float3(0.0));
}

bool linked(uint instance, uint light, uint mask)
{
    const uint words = params.lighting.z;

    return ((linkMasks[(2 * instance + mask) * words + light / 32] >> (light % 32)) & 1) != 0;
}

// One sample of each of the pass's lights, each queued with the shadow ray that decides whether it
// arrives.
void sampleLights(uint index, PathState path, uint instance, PreviewSurfaceSample surface, float3 position, float3 geometric, float3 toViewer)
{
    if (params.lighting.y == 0)
        return;

    const uint pixel = pixelKey(path);
    const uint seed = bounceSeed();
    const bool domeSpecular = !mayReflect(path);
    const float3 scaled = abs(position);
    const float offset = 1e-4 * max(1.0, max(scaled.x, max(scaled.y, scaled.z)));

    for (uint j = 0; j < params.lighting.y; ++j) {
        const uint l = params.lighting.x + j;
        const uint slot = j * params.count + index;

        if (l >= params.lighting.w || !linked(instance, l, 0)) {
            writeUnshadowed(slot, l, float3(0.0));
            continue;
        }

        const LightRecord light = lights[l];
        const LightSample s = sampleLight(light, lightData, textureBindings, images, texels, position, surface.normal, lightRandom(pixel, l, seed));
        const bool specular = domeSpecular || light.kind.x != kLightDome;
        const float3 contribution = s.weight * evaluatePreviewSurfaceLobes(surface, toViewer, s.direction, specular) * surface.opacity * path.throughput.xyz;

        if (!any(contribution > float3(0.0)) || any(isnan(contribution)) || any(isinf(contribution))) {
            writeUnshadowed(slot, l, float3(0.0));
            continue;
        }
        if ((light.kind.y & kLightCastsShadows) == 0) {
            writeUnshadowed(slot, l, contribution);
            continue;
        }

        const float3 origin = position + (dot(geometric, s.direction) >= 0.0 ? offset : -offset) * geometric;
        const float tMax = s.distance >= kLightAtInfinity ? kLightAtInfinity : s.distance * (1.0 - 1e-4);

        writeShadowRay(slot, l, origin, s.direction, tMax, contribution);
    }
}

// Where nothing shows the pixel takes the film's clear values, as a miss leaves them.
void writeNothing(uint index)
{
    colorOut[index] = float4(0.0, 0.0, 0.0, 0.0);
    depthOut[index] = 1.0;
    primIdOut[index] = -1;
    instanceIdOut[index] = -1;
}

// A miss sees every dome light behind the scene, opaque as Storm's sky dome draws it.
void writeBackground(uint index, PathState path)
{
    const uint count = params.count;
    const float3 direction = normalize(float3(rays[kRayDirectionX * count + index], rays[kRayDirectionY * count + index], rays[kRayDirectionZ * count + index]));
    float3 radiance = float3(0.0);
    bool dome = false;

    for (uint l = 0; l < params.lighting.w; ++l) {
        const LightRecord light = lights[l];

        if (light.kind.x == kLightDome) {
            dome = true;
            radiance += domeBackground(light, textureBindings, images, texels, direction);
        }
    }
    if (params.bounce.x == 0)
        writeNothing(index);
    if (dome)
        addColor(index, path, float4(radiance, 1.0));
}

float largest(float3 v)
{
    return max(v.x, max(v.y, v.z));
}

/**
 * @brief Queues the one ray a hit on a PreviewSurface goes on with, picked among its specular lobes,
 * the reflection off its transmissive part's fresnel and the transmission through it, each in
 * proportion to an estimate of what it carries. A lobe whose depth is spent is not picked, so the
 * light it would have found is lost. entering tells a ray going into the surface's inside from one
 * leaving it, by which side of the triangle it meets.
 **/
void continuePath(uint index, PathState path, PreviewSurfaceSample surface, float3 position, float3 direction, float3 geometric, bool entering)
{
    const float3 I = normalize(direction);
    const float3 V = -I;
    const float3 N = dot(surface.normal, V) > 1e-4 ? surface.normal : geometric;
    const float NdotV = dot(N, V);
    const bool totalLeft = path.counters.y < params.bounce.y;
    const bool canReflect = totalLeft && path.counters.z < params.bounce.z;
    const bool canRefract = totalLeft && path.counters.w < params.bounce.w;
    const float opacity = saturate(surface.opacity);
    const float transparency = 1.0 - opacity;
    const bool bends = surface.ior > 0.0 && bendsLight(surface.ior);
    const float eta = bends ? (entering ? 1.0 / surface.ior : surface.ior) : 1.0;
    float cosT = 1.0;
    const float fresnel = bends ? dielectricFresnel(NdotV, eta, cosT) : 0.0;
    const bool total = fresnel >= 1.0;
    float3 F0;
    float3 F90;
    float3 diffuse;
    specularReflectance(surface, F0, F90, diffuse);

    const float facing = pow(max(0.0, 1.0 - NdotV), 5.0);
    const float R = (1.0 - surface.ior) / (1.0 + surface.ior);
    const float3 tint = bends ? surface.diffuseColor : float3(1.0);
    float q[4];

    q[0] = canReflect ? opacity * largest(lerp(F0, F90, facing)) : 0.0;
    q[1] = canReflect && surface.clearcoat > 0.0 ? opacity * surface.clearcoat * lerp(R * R, 1.0, facing) : 0.0;
    q[2] = (total ? canRefract : canReflect) ? transparency * fresnel : 0.0;
    q[3] = (bends ? canRefract : totalLeft) && !total ? transparency * (1.0 - fresnel) * max(1.0, largest(tint)) : 0.0;

    const float sum = q[0] + q[1] + q[2] + q[3];

    if (!(sum > 0.0) || !(NdotV > 0.0)) {
        endPath(index, path);
        return;
    }

    const uint pixel = pixelKey(path);
    const uint seed = bounceSeed();
    const float choice = lightRandom(pixel, kRandomLobeChoice, seed).x * sum;
    const float2 u = lightRandom(pixel, kRandomLobeSample, seed);
    uint lobe = 3;
    float below = 0.0;

    for (uint k = 0; k < 3; ++k) {
        if (q[k] > 0.0 && choice < below + q[k]) {
            lobe = k;
            break;
        }
        below += q[k];
    }
    while (lobe > 0 && !(q[lobe] > 0.0))
        --lobe;

    const float probability = q[lobe] / sum;
    float3 next = V;
    float3 weight = float3(0.0);
    float alpha = 0.0;
    uint kind = kBounceReflection;

    if (lobe == 0) {
        const LobeSample drawn = sampleSpecular(N, V, F0, F90, surface.roughness, u);
        next = drawn.direction;
        weight = opacity * drawn.weight;
    } else if (lobe == 1) {
        const LobeSample drawn = sampleSpecular(N, V, float3(R * R), float3(1.0), surface.clearcoatRoughness, u);
        next = drawn.direction;
        weight = opacity * surface.clearcoat * drawn.weight;
    } else if (lobe == 2) {
        next = 2.0 * NdotV * N - V;
        weight = float3(transparency * fresnel);
        kind = total ? kBounceRefraction : kBounceReflection;
    } else {
        next = bends ? normalize(eta * I + (eta * NdotV - cosT) * N) : I;
        weight = transparency * (1.0 - fresnel) * tint;
        alpha = transparency;
        kind = bends ? kBounceRefraction : kBounceStraight;
    }

    const bool through = lobe == 3;
    const float side = dot(geometric, next);

    if (through ? !(side < 0.0) : !(side > 0.0)) {
        endPath(index, path);
        return;
    }

    PathState continued = path;
    continued.throughput = float4(path.throughput.xyz * weight / probability, path.throughput.w * alpha / probability);
    continued.counters.y += 1;
    if (kind == kBounceReflection)
        continued.counters.z += 1;
    else if (kind == kBounceRefraction)
        continued.counters.w += 1;

    if (any(isnan(continued.throughput)) || any(isinf(continued.throughput)) || !(any(continued.throughput > float4(0.0)))) {
        endPath(index, path);
        return;
    }

    const float3 scaled = abs(position);
    const float offset = 1e-4 * max(1.0, max(scaled.x, max(scaled.y, scaled.z)));
    const float3 origin = position + (through ? -offset : offset) * geometric;

    writeContinuation(index, origin, next, kLightAtInfinity, continued);
}

// Emission, premultiplied by coverage, and the shadow rays of the pass's lights, whose unblocked
// contributions ShadeLights adds. Only the pass that samples the first light writes the AOVs and the
// continuations, so the later passes leave the lighting already added alone. Camera rays write the
// AOVs; later bounces only add to the colour, scaled by what their path carries.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityShadeSurface(uint3 id : SV_DispatchThreadID)
{
    const uint count = params.count;

    if (id.x >= count)
        return;

    const bool writesAovs = params.lighting.x == 0;
    const PathState path = pathOf(id.x);

    if (params.bounce.x > 0 && !(rays[kRayTMax * count + id.x] >= 0.0)) {
        writeNoShadows(id.x);
        endPath(id.x, path);
        return;
    }

    const uint geometryIndex = hitIds[kHitGeometry * count + id.x];

    if (geometryIndex == kNoHit) {
        writeNoShadows(id.x);
        if (writesAovs)
            writeBackground(id.x, path);
        endPath(id.x, path);
        return;
    }

    const GeometryRecord geometry = geometries[geometryIndex];
    const MaterialRecord material = materials[geometry.materialIndex];
    const uint primitive = hitIds[kHitPrimitive * count + id.x];
    const uint instance = hitIds[kHitInstance * count + id.x];
    const float t = hitSurface[kHitT * count + id.x];
    const float u = hitSurface[kHitU * count + id.x];
    const float v = hitSurface[kHitV * count + id.x];
    const float3 origin = float3(rays[kRayOriginX * count + id.x], rays[kRayOriginY * count + id.x], rays[kRayOriginZ * count + id.x]);
    const float3 direction = float3(rays[kRayDirectionX * count + id.x], rays[kRayDirectionY * count + id.x], rays[kRayDirectionZ * count + id.x]);
    const float3 worldPosition = origin + t * direction;

    const uint row = instance * kInstanceTransformRows;
    const float4 o0 = instanceTransforms[row];
    const float4 o1 = instanceTransforms[row + 1];
    const float4 o2 = instanceTransforms[row + 2];
    const float4 o3 = instanceTransforms[row + 3];
    float3 p0;
    float3 p1;
    float3 p2;

    triangleCorners(points, triangles, geometry, primitive, o0, o1, o2, o3, p0, p1, p2);

    const float3 outward = normalize(cross(p1 - p0, p2 - p0));
    const float alongRay = t / rays[kRayTMax * count + id.x];
    // Past the first bounce a ray has no pixel footprint, and reads textures at their finest.
    const float width = params.bounce.x == 0 ? pixelWidth(alongRay) : 0.0;
    const float cosine = abs(dot(outward, direction));

    float4 shaded = kUnsupportedColor;

    if (material.kind.x == kMaterialProject3D) {
        const float4 w0 = instanceTransforms[row + 4];
        const float4 w1 = instanceTransforms[row + 5];
        const float4 w2 = instanceTransforms[row + 6];
        const float4 w3 = instanceTransforms[row + 7];

        writeNoShadows(id.x);
        endPath(id.x, path);
        if (!evaluateProject3D(projections[material.kind.y], textureBindings, images, texels, worldPosition, outward, direction, width, o0, o1, o2, o3, w0, w1, w2, w3, shaded)) {
            if (writesAovs)
                writeBackground(id.x, path);
            return;
        }
    } else if (material.kind.x == kMaterialPreviewSurface) {
        const bool hasSt = geometry.stBinding != kStBindingNone;
        float2 st = float2(0.0, 0.0);
        float2 st0 = st;
        float2 st1 = st;
        float2 st2 = st;

        if (hasSt) {
            cornerSts(sts, triangles, geometry, primitive, st0, st1, st2);
            st = (1.0 - u - v) * st0 + u * st1 + v * st2;
        }

        const float footprint = stFootprint(width, p1 - p0, p2 - p0, st1 - st0, st2 - st0, cosine);

        const float4 display = surfaceColor(colors, triangles, geometry, primitive, u, v);
        const uint flags = material.kind.z;
        // An input reading displayColor keeps its reader's fallback in its constant, for geometry without the primvar.
        const bool displayColor = hasDisplayColor(geometry);
        const float3 diffuseConstant = (flags & kMaterialDiffuseFromDisplayColor) != 0 && displayColor ? display.xyz : material.diffuseColor.xyz;
        float opacity = surfaceOpacity(textureBindings, images, texels, material, geometry, display, hasSt, st, footprint);
        const float threshold = material.coverage.y;

        if (threshold > 0.0) {
            if (opacity < threshold) {
                writeNoShadows(id.x);
                if (writesAovs)
                    writeBackground(id.x, path);
                endPath(id.x, path);
                return;
            }
            opacity = 1.0;
        }

        const float3 geometric = dot(outward, direction) > 0.0 ? -outward : outward;
        const float3 smooth = shadingNormal(surfaceNormal(normals, triangles, geometry, primitive, u, v),
                                            instanceTransforms[row + 4], instanceTransforms[row + 5], instanceTransforms[row + 6], geometric);
        const float3 normalConstant = float3(material.diffuseColor.w, material.emissiveColor.w, material.coverage.w);
        const float3 tangentNormal = colorInput(material, kSlotNormal, normalConstant, hasSt, st, footprint);
        const float3 normal = applyNormalMap(smooth, tangentNormal, p1 - p0, p2 - p0, st1 - st0, st2 - st0);

        PreviewSurfaceSample surface;
        surface.diffuseColor = colorInput(material, kSlotDiffuseColor, diffuseConstant, hasSt, st, footprint);
        surface.specularColor = colorInput(material, kSlotSpecularColor, material.specularColor.xyz, hasSt, st, footprint);
        const float3 emissiveConstant = (flags & kMaterialEmissiveFromDisplayColor) != 0 && displayColor ? display.xyz : material.emissiveColor.xyz;
        surface.emissiveColor = colorInput(material, kSlotEmissiveColor, emissiveConstant, hasSt, st, footprint);
        surface.normal = normal;
        surface.metallic = floatInput(material, kSlotMetallic, material.surface.x, hasSt, st, footprint);
        surface.roughness = floatInput(material, kSlotRoughness, material.surface.y, hasSt, st, footprint);
        surface.clearcoat = floatInput(material, kSlotClearcoat, material.surface.z, hasSt, st, footprint);
        surface.clearcoatRoughness = floatInput(material, kSlotClearcoatRoughness, material.surface.w, hasSt, st, footprint);
        surface.opacity = opacity;
        surface.ior = floatInput(material, kSlotIor, material.coverage.z, hasSt, st, footprint);
        surface.useSpecularWorkflow = material.specularColor.w != 0.0;

        const float3 radiance = surface.emissiveColor;
        shaded = float4(radiance * opacity, opacity);
        sampleLights(id.x, path, instance, surface, worldPosition, geometric, -direction);
        if (writesContinuations())
            continuePath(id.x, path, surface, worldPosition, direction, geometric, dot(outward, direction) < 0.0);
    } else {
        writeNoShadows(id.x);
        endPath(id.x, path);
    }

    if (!writesAovs)
        return;
    addColor(id.x, path, shaded);
    if (params.bounce.x > 0)
        return;
    depthOut[id.x] = windowDepth(alongRay);
    primIdOut[id.x] = geometry.primId;
    instanceIdOut[id.x] = instanceIds[instance];
}
