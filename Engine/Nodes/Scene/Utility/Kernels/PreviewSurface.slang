module PreviewSurface;

// The UsdPreviewSurface lobes as Storm's previewSurface.glslfx evaluates a light: Lambert diffuse, GGX
// specular with Schlick Fresnel and Smith visibility, and a clearcoat GGX lobe added on top. The
// constants, clamps and lobe composition are Storm's, so the two renderers agree.

static const float kPi = 3.14159265358979;
static const float kEpsilon = 0.001;

public struct PreviewSurfaceSample
{
    public float3 diffuseColor;
    public float3 specularColor;
    public float3 emissiveColor;
    public float3 normal;
    public float metallic;
    public float roughness;
    public float clearcoat;
    public float clearcoatRoughness;
    public float opacity;
    public float ior;
    public bool useSpecularWorkflow;
    public bool opacityPresence;
};

float schlickFresnel(float EdotH)
{
    return pow(max(0.0, 1.0 - EdotH), 5.0);
}

float normalDistribution(float roughness, float NdotH)
{
    const float alpha = roughness * roughness;
    const float alpha2 = alpha * alpha;
    float d = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    d *= d;
    d *= kPi;
    return (alpha2 + kEpsilon) / d;
}

float geometric(float roughness, float NdotL, float NdotE)
{
    const float alpha = roughness * roughness;
    const float k = alpha * 0.5;
    return NdotE / (NdotE * (1.0 - k) + k) * (NdotL / (NdotL * (1.0 - k) + k));
}

float3 directSpecular(float3 F0, float3 F90, float roughness, float fresnel, float NdotL, float NdotE, float NdotH)
{
    const float3 F = lerp(F0, F90, fresnel);
    return F * geometric(roughness, NdotL, NdotE) * normalDistribution(roughness, NdotH) / (4.0 * NdotL * NdotE + kEpsilon);
}

// The base specular lobe's reflectance at normal incidence and at grazing angles, and the diffuse
// colour the metallic workflow leaves.
public void specularReflectance(PreviewSurfaceSample s, out float3 F0, out float3 F90, out float3 diffuse)
{
    diffuse = s.diffuseColor;
    if (s.useSpecularWorkflow) {
        F0 = s.specularColor;
        F90 = float3(1.0, 1.0, 1.0);
        return;
    }
    const float R = (1.0 - s.ior) / (1.0 + s.ior);
    const float3 specColor = lerp(float3(1.0, 1.0, 1.0), s.diffuseColor, s.metallic);
    F0 = lerp(R * R * specColor, specColor, s.metallic);
    F90 = specColor;
    diffuse *= 1.0 - s.metallic;
}

// The reflected radiance per unit incoming radiance from L towards V, cosine included. V and L point
// away from the surface.
public float3 evaluatePreviewSurface(PreviewSurfaceSample s, float3 V, float3 L)
{
    return evaluatePreviewSurfaceLobes(s, V, L, true);
}

// evaluatePreviewSurface(), without the specular lobes when specular is false: for light that
// reflection rays already gather.
public float3 evaluatePreviewSurfaceLobes(PreviewSurfaceSample s, float3 V, float3 L, bool specular)
{
    const float3 N = s.normal;
    const float NdotL = max(0.0, dot(N, L));

    if (NdotL <= 0.0)
        return float3(0.0, 0.0, 0.0);

    const float NdotE = max(0.0, dot(N, V));
    const float3 H = normalize(L + V);
    const float NdotH = max(0.0, dot(N, H));
    const float EdotH = max(0.0, dot(V, H));
    const float roughness = max(0.001, s.roughness);
    const float clearcoatRoughness = max(0.001, s.clearcoatRoughness);
    const float fresnel = schlickFresnel(EdotH);
    float3 F0;
    float3 F90;
    float3 diffuse;
    specularReflectance(s, F0, F90, diffuse);

    float3 d = diffuse * (1.0 / kPi);
    const float3 s1 = directSpecular(F0, F90, roughness, fresnel, NdotL, NdotE, NdotH);
    d *= float3(1.0, 1.0, 1.0) - lerp(F0, F90, fresnel);

    if (!specular)
        return NdotL * d;

    float3 s2 = float3(0.0, 0.0, 0.0);
    if (s.clearcoat > 0.0) {
        const float R = (1.0 - s.ior) / (1.0 + s.ior);
        s2 = s.clearcoat * directSpecular(float3(R * R), float3(1.0, 1.0, 1.0), clearcoatRoughness, fresnel, NdotL, NdotE, NdotH);
    }
    return NdotL * (d + s1 + s2);
}

/**
 * @brief The specular lobe of a refractive surface's transmissive part, per unit incoming radiance from
 * L towards V, cosine included: GGX at the surface's roughness with the Fresnel of an uncoloured
 * dielectric of its ior, the reflectance its reflection rays carry, so that lights show highlights on
 * glass as on opaque surfaces.
 **/
public float3 evaluateDielectricSpecular(PreviewSurfaceSample s, float3 V, float3 L)
{
    const float3 N = s.normal;
    const float NdotL = max(0.0, dot(N, L));

    if (NdotL <= 0.0)
        return float3(0.0, 0.0, 0.0);

    const float NdotE = max(0.0, dot(N, V));
    const float3 H = normalize(L + V);
    const float NdotH = max(0.0, dot(N, H));
    const float EdotH = max(0.0, dot(V, H));
    const float R = (1.0 - s.ior) / (1.0 + s.ior);

    return NdotL * directSpecular(float3(R * R), float3(1.0, 1.0, 1.0), max(0.001, s.roughness), schlickFresnel(EdotH), NdotL, NdotE, NdotH);
}

// Below this roughness a specular lobe reflects as a mirror.
public static const float kMirrorRoughness = 0.01;

// Smith's masking for the GGX distribution, which normalises the distribution of visible normals.
float smithMasking(float alpha, float NdotV)
{
    const float alpha2 = alpha * alpha;
    return 2.0 * NdotV / (NdotV + sqrt(alpha2 + (1.0 - alpha2) * NdotV * NdotV));
}

// One direction's factor of geometric().
float schlickMasking(float roughness, float NdotV)
{
    const float k = roughness * roughness * 0.5;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Two unit vectors that make an orthonormal frame with n.
void tangentFrame(float3 n, out float3 t, out float3 b)
{
    const float sign = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (sign + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = float3(c, sign + n.y * n.y * a, -n.y);
}

// A microfacet normal drawn from the GGX normals the local direction v sees, z being the surface
// normal, by the two random numbers u.
float3 sampleVisibleNormal(float3 v, float alpha, float2 u)
{
    const float3 vh = normalize(float3(alpha * v.x, alpha * v.y, v.z));
    const float lengthSquared = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lengthSquared > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lengthSquared) : float3(1.0, 0.0, 0.0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u.x);
    const float phi = 2.0 * kPi * u.y;
    const float p1 = r * cos(phi);
    const float s = 0.5 * (1.0 + vh.z);
    const float p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * r * sin(phi);
    const float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

public struct LobeSample
{
    public float3 direction;
    // The lobe's reflectance along direction, cosine included, over the probability of drawing it.
    public float3 weight;
};

/**
 * @brief A reflection from V off a specular lobe of evaluatePreviewSurface()'s, by the two random
 * numbers u: a mirror below kMirrorRoughness, otherwise a GGX visible normal. The weight carries the
 * lobe's Fresnel and Smith visibility; the GGX distribution itself is what the direction is drawn by.
 **/
public LobeSample sampleSpecular(float3 N, float3 V, float3 F0, float3 F90, float roughness, float2 u)
{
    LobeSample sample;
    sample.direction = N;
    sample.weight = float3(0.0, 0.0, 0.0);

    const float NdotV = dot(N, V);

    if (!(NdotV > 1e-6))
        return sample;
    if (roughness < kMirrorRoughness) {
        sample.direction = 2.0 * NdotV * N - V;
        sample.weight = lerp(F0, F90, schlickFresnel(NdotV));
        return sample;
    }

    const float alpha = roughness * roughness;
    float3 t;
    float3 b;
    tangentFrame(N, t, b);

    const float3 h = sampleVisibleNormal(float3(dot(V, t), dot(V, b), NdotV), alpha, u);
    const float3 H = normalize(h.x * t + h.y * b + h.z * N);
    const float VdotH = dot(V, H);
    const float3 L = 2.0 * VdotH * H - V;
    const float NdotL = dot(N, L);

    if (!(NdotL > 0.0) || !(VdotH > 0.0))
        return sample;
    sample.direction = L;
    sample.weight = lerp(F0, F90, schlickFresnel(VdotH)) * (schlickMasking(roughness, NdotL) * schlickMasking(roughness, NdotV) / smithMasking(alpha, NdotV));
    return sample;
}

/**
 * @brief The unpolarised Fresnel reflectance of a smooth interface, for a ray at cosine cosI to the
 * normal on its side, going into a medium 1 / eta times as dense. 1 past the critical angle, where
 * cosT is 0; otherwise cosT is the cosine of the refracted ray to the normal.
 **/
public float dielectricFresnel(float cosI, float eta, out float cosT)
{
    const float sinT2 = eta * eta * max(0.0, 1.0 - cosI * cosI);

    cosT = 0.0;
    if (sinT2 >= 1.0)
        return 1.0;
    cosT = sqrt(1.0 - sinT2);

    const float rs = (eta * cosI - cosT) / (eta * cosI + cosT);
    const float rp = (eta * cosT - cosI) / (eta * cosT + cosI);
    return 0.5 * (rs * rs + rp * rp);
}

// Tangent-space normal n (z out of the surface) through the frame of the triangle's positions and
// texture coordinates.
public float3 applyNormalMap(float3 geometric, float3 n, float3 dp1, float3 dp2, float2 duv1, float2 duv2)
{
    const float det = duv1.x * duv2.y - duv1.y * duv2.x;
    float3 T = float3(0.0, 0.0, 0.0);
    float3 B = float3(0.0, 0.0, 0.0);

    if (abs(det) >= 1e-12) {
        const float r = 1.0 / det;
        T = (dp1 * duv2.y - dp2 * duv1.y) * r;
        T = T - geometric * dot(geometric, T);
        B = (dp2 * duv1.x - dp1 * duv2.x) * r;
    }

    float lengthT = length(T);

    // Without texture coordinates the tangent frame has no defined orientation; a constant or fallback
    // normal still tilts the surface, about the triangle's first edge.
    if (lengthT < 1e-12) {
        T = dp1 - geometric * dot(geometric, dp1);
        lengthT = length(T);
        if (lengthT < 1e-12)
            return geometric;
        B = cross(geometric, T);
    }
    T = T / lengthT;

    float3 bitangent = cross(geometric, T);
    if (dot(bitangent, B) < 0.0)
        bitangent = -bitangent;

    const float3 mapped = n.x * T + n.y * bitangent + n.z * geometric;
    const float lengthM = length(mapped);
    return lengthM > 1e-12 ? mapped / lengthM : geometric;
}
