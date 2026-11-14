module Lights;

import UtilityTypes;
import Texture;

static const float kPi = 3.14159265358979;

// A direction toward a light, and what arrives along it divided by the probability density of having
// picked it: radiance over the solid-angle pdf, or irradiance at normal incidence for a delta light.
public struct LightSample
{
    public float3 direction;
    // To the sampled point; kLightAtInfinity for distant and dome lights.
    public float distance;
    public float3 weight;
};

public static const float kLightAtInfinity = 3.0e38;

// Two numbers in [0, 1) that differ per pixel, light and sample, and repeat from render to render.
public float2 lightRandom(uint pixel, uint light, uint seed)
{
    uint h = pixel * 0x9e3779b9u ^ (light + 0x7f4a7c15u) * 0x85ebca6bu ^ seed * 0xc2b2ae35u;
    float2 r;

    h = (h ^ (h >> 16)) * 0x7feb352du;
    h = (h ^ (h >> 15)) * 0x846ca68bu;
    h ^= h >> 16;
    r.x = float(h >> 8) * (1.0 / 16777216.0);
    h = (h ^ 0x68e31da4u) * 0x2c1b3c6du;
    h = (h ^ (h >> 12)) * 0x297a2d39u;
    h ^= h >> 15;
    r.y = float(h >> 8) * (1.0 / 16777216.0);
    return r;
}

float3 toLight(LightRecord light, float3 v)
{
    return float3(dot(v, light.axisX.xyz), dot(v, light.axisY.xyz), dot(v, light.axisZ.xyz));
}

float3 fromLight(LightRecord light, float3 v)
{
    return v.x * light.axisX.xyz + v.y * light.axisY.xyz + v.z * light.axisZ.xyz;
}

// A unit vector at cosTheta from axis, at an angle phi about it.
float3 aroundAxis(float3 axis, float cosTheta, float phi)
{
    const float3 helper = abs(axis.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    const float3 t = normalize(cross(helper, axis));
    const float3 b = cross(axis, t);
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));

    return sinTheta * (cos(phi) * t + sin(phi) * b) + cosTheta * axis;
}

// The first of count CDF values at offset above u, so a value in [0, count).
uint searchCdf(StructuredBuffer<float> data, uint offset, uint count, float u)
{
    uint low = 0;
    uint high = count;

    while (low < high) {
        const uint middle = (low + high) / 2;

        if (data[offset + middle] <= u)
            low = middle + 1;
        else
            high = middle;
    }
    return min(low, count - 1);
}

float cdfStep(StructuredBuffer<float> data, uint offset, uint index)
{
    return data[offset + index] - (index == 0 ? 0.0 : data[offset + index - 1]);
}

float iesTable(LightRecord light, StructuredBuffer<float> data, uint theta, uint phi)
{
    const uint thetaCount = light.data.w & 0xffffu;

    return data[light.kind.w + phi * thetaCount + theta];
}

// The IES table's candela toward local, the light-space direction from the light to the point lit,
// measured from the light's -Z axis.
float iesFactor(LightRecord light, StructuredBuffer<float> data, float3 local)
{
    const uint thetaCount = light.data.w & 0xffffu;
    const uint phiCount = light.data.w >> 16;
    float theta = acos(clamp(-local.z, -1.0, 1.0));
    const float scale = light.shaping.w;

    if (scale > 0.0)
        theta = theta / scale;
    else if (scale < 0.0)
        theta = (theta - kPi) / -scale + kPi;
    if (!(theta >= 0.0) || theta > kPi || thetaCount < 2 || phiCount < 1)
        return 0.0;

    float phi = atan2(local.y, local.x);
    if (phi < 0.0)
        phi += 2.0 * kPi;

    const float x = theta / kPi * float(thetaCount - 1);
    const uint t0 = min(thetaCount - 2, uint(x));
    const float ft = x - float(t0);
    const float y = phi / (2.0 * kPi) * float(phiCount);
    const uint p0 = min(phiCount - 1, uint(y));
    const uint p1 = (p0 + 1) % phiCount;
    const float fp = phiCount > 1 ? y - float(p0) : 0.0;
    const float a = lerp(iesTable(light, data, t0, p0), iesTable(light, data, t0 + 1, p0), ft);
    const float b = lerp(iesTable(light, data, t0, p1), iesTable(light, data, t0 + 1, p1), ft);

    return lerp(a, b, fp) * light.focusTint.w;
}

// The cone, focus and IES profile of the light toward emitted, the world direction from the point on
// the light to the point lit.
float3 shapingFactor(LightRecord light, StructuredBuffer<float> data, float3 emitted)
{
    const uint flags = light.kind.y;
    float3 factor = float3(1.0);
    const float3 local = toLight(light, emitted);

    if ((flags & kLightShaped) != 0) {
        const float cosOff = clamp(-local.z, -1.0, 1.0);
        const float off = acos(cosOff);
        const float cutoff = light.shaping.x;
        const float softness = clamp(light.shaping.y, 0.0, 1.0);

        if (off > cutoff)
            return float3(0.0);
        if (softness > 0.0)
            factor *= 1.0 - smoothstep((1.0 - softness) * cutoff, cutoff, off);

        const float focus = light.shaping.z;

        if (focus > 0.0) {
            const float f = pow(max(0.0, cosOff), focus);
            factor *= (1.0 - f) * light.focusTint.xyz + f;
        }
    }
    if ((flags & kLightIes) != 0)
        factor *= iesFactor(light, data, local);
    return factor;
}

// Light-space direction of a lat-long texel coordinate: +Y up, longitude decreasing with u.
float3 latLongDirection(float2 uv)
{
    const float lat = (uv.y - 0.5) * kPi;
    const float lon = kPi * (1.0 - 2.0 * uv.x);

    return float3(sin(lon) * cos(lat), sin(lat), cos(lon) * cos(lat));
}

float3 domeRadiance(LightRecord light, StructuredBuffer<TextureBinding> bindings, StructuredBuffer<uint4> images, StructuredBuffer<float4> texels, float2 uv)
{
    const int binding = asint(light.kind.z);

    if (binding == kNoTexture)
        return light.radiance.xyz;
    return light.radiance.xyz * sampleBinding(bindings, images, texels, binding, uv, kFinestLevel).xyz;
}

LightSample noLight()
{
    LightSample s;
    s.direction = float3(0.0, 0.0, 1.0);
    s.distance = 0.0;
    s.weight = float3(0.0);
    return s;
}

// Sampling an area light uniformly by area: the solid-angle pdf is distance squared over area times
// the cosine at the light.
LightSample areaSample(LightRecord light, StructuredBuffer<float> data, float3 position, float3 onLight, float3 lightNormal)
{
    const float3 toward = onLight - position;
    const float distance = length(toward);

    if (!(distance > 0.0))
        return noLight();

    LightSample s;
    s.direction = toward / distance;
    s.distance = distance;

    const float cosLight = dot(-s.direction, lightNormal);

    if (!(cosLight > 0.0))
        return noLight();
    s.weight = light.radiance.xyz * shapingFactor(light, data, -s.direction) * (light.radiance.w * cosLight / (distance * distance));
    return s;
}

/**
 * @brief Picks a direction toward light from position, where the surface's normal is normal, by the
 * two random numbers u.
 **/
public LightSample sampleLight(LightRecord light, StructuredBuffer<float> data, StructuredBuffer<TextureBinding> bindings, StructuredBuffer<uint4> images,
                               StructuredBuffer<float4> texels, float3 position, float3 normal, float2 u)
{
    const uint type = light.kind.x;
    const uint flags = light.kind.y;
    const float3 centre = light.position.xyz;

    if (type == kLightDistant) {
        const float cosMax = light.axisZ.w;
        LightSample s;

        s.distance = kLightAtInfinity;
        if (cosMax >= 1.0) {
            s.direction = light.axisZ.xyz;
            s.weight = light.radiance.xyz;
            return s;
        }
        s.direction = aroundAxis(light.axisZ.xyz, 1.0 - u.x * (1.0 - cosMax), 2.0 * kPi * u.y);
        s.weight = light.radiance.xyz * (2.0 * kPi * (1.0 - cosMax));
        return s;
    }

    if (type == kLightSphere) {
        const float3 toward = centre - position;
        const float d2 = dot(toward, toward);
        const float distance = sqrt(d2);
        const float radius = light.position.w;

        if (!(distance > 0.0))
            return noLight();

        LightSample s;
        const float3 axis = toward / distance;

        if ((flags & kLightPoint) != 0 || !(radius > 0.0)) {
            s.direction = axis;
            s.distance = distance;
            s.weight = light.radiance.xyz * shapingFactor(light, data, -axis) / d2;
            return s;
        }
        if (distance <= radius)
            return noLight();

        const float sin2Max = radius * radius / d2;
        const float cosMax = sqrt(max(0.0, 1.0 - sin2Max));

        s.direction = aroundAxis(axis, 1.0 - u.x * (1.0 - cosMax), 2.0 * kPi * u.y);

        const float along = dot(s.direction, toward);
        s.distance = along - sqrt(max(0.0, radius * radius - (d2 - along * along)));
        s.weight = light.radiance.xyz * shapingFactor(light, data, -s.direction) * (2.0 * kPi * (1.0 - cosMax));
        return s;
    }

    if (type == kLightRect) {
        const float3 onLight = centre + (2.0 * u.x - 1.0) * light.axisX.w * light.axisX.xyz + (2.0 * u.y - 1.0) * light.axisY.w * light.axisY.xyz;
        return areaSample(light, data, position, onLight, -light.axisZ.xyz);
    }

    if (type == kLightDisk) {
        const float r = sqrt(u.x);
        const float phi = 2.0 * kPi * u.y;
        const float3 onLight = centre + r * (cos(phi) * light.axisX.w * light.axisX.xyz + sin(phi) * light.axisY.w * light.axisY.xyz);
        return areaSample(light, data, position, onLight, -light.axisZ.xyz);
    }

    if (type == kLightCylinder) {
        const float phi = 2.0 * kPi * u.y;
        const float3 around = cos(phi) * light.axisY.xyz + sin(phi) * light.axisZ.xyz;
        const float3 onLight = centre + (2.0 * u.x - 1.0) * light.axisX.w * light.axisX.xyz + light.position.w * around;
        return areaSample(light, data, position, onLight, around);
    }

    if (type == kLightDome) {
        LightSample s;
        s.distance = kLightAtInfinity;

        if ((flags & kLightDomeImportance) == 0) {
            const float cosTheta = sqrt(1.0 - u.x);
            s.direction = aroundAxis(normal, cosTheta, 2.0 * kPi * u.y);
            if (!(cosTheta > 0.0))
                return noLight();

            const float3 local = toLight(light, s.direction);
            const float2 uv = float2(0.5 * (1.0 - atan2(local.x, local.z) / kPi), asin(clamp(local.y, -1.0, 1.0)) / kPi + 0.5);
            s.weight = domeRadiance(light, bindings, images, texels, uv) * (kPi / cosTheta);
            return s;
        }

        const uint offset = light.data.x;
        const uint width = light.data.y;
        const uint height = light.data.z;
        const uint marginal = offset + width * height;
        const uint row = searchCdf(data, marginal, height, u.y);
        const float rowProbability = cdfStep(data, marginal, row);
        const float rowStart = row == 0 ? 0.0 : data[marginal + row - 1];
        const float fy = rowProbability > 0.0 ? clamp((u.y - rowStart) / rowProbability, 0.0, 1.0) : 0.5;
        const uint conditional = offset + row * width;
        const uint column = searchCdf(data, conditional, width, u.x);
        const float columnProbability = cdfStep(data, conditional, column);
        const float columnStart = column == 0 ? 0.0 : data[conditional + column - 1];
        const float fx = columnProbability > 0.0 ? clamp((u.x - columnStart) / columnProbability, 0.0, 1.0) : 0.5;
        const float2 uv = float2((float(column) + fx) / float(width), (float(row) + fy) / float(height));
        const float cosLat = cos((uv.y - 0.5) * kPi);
        const float pdf = rowProbability * columnProbability * float(width * height) / (2.0 * kPi * kPi * max(cosLat, 1e-6));

        if (!(pdf > 0.0))
            return noLight();
        s.direction = fromLight(light, latLongDirection(uv));
        s.weight = domeRadiance(light, bindings, images, texels, uv) / pdf;
        return s;
    }

    return noLight();
}
