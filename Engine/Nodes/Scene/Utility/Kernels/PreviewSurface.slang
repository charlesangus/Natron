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

    float3 s2 = float3(0.0, 0.0, 0.0);
    if (s.clearcoat > 0.0) {
        const float R = (1.0 - s.ior) / (1.0 + s.ior);
        s2 = s.clearcoat * directSpecular(float3(R * R), float3(1.0, 1.0, 1.0), clearcoatRoughness, fresnel, NdotL, NdotE, NdotH);
    }
    return NdotL * (d + s1 + s2);
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
