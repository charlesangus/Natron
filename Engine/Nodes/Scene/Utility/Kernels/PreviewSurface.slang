module PreviewSurface;

// The UsdPreviewSurface lobes as Storm's previewSurface.glslfx evaluates them: Lambert diffuse, GGX
// specular with Schlick Fresnel and Smith visibility, a clearcoat GGX lobe on top.

static const float kPi = 3.14159265358979;
static const float kEpsilon = 1e-4;

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

float schlickFresnel(float cosTheta)
{
    const float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    const float m2 = m * m;
    return m2 * m2 * m;
}

float ggxDistribution(float NdotH, float alpha)
{
    const float alpha2 = alpha * alpha;
    const float d = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / (kPi * d * d + kEpsilon);
}

float smithVisibility(float NdotL, float NdotV, float alpha)
{
    const float k = alpha * 0.5;
    const float gl = NdotL / (NdotL * (1.0 - k) + k);
    const float gv = NdotV / (NdotV * (1.0 - k) + k);
    return gl * gv / (4.0 * NdotL * NdotV + kEpsilon);
}

float3 specularLobe(float3 F0, float3 F90, float roughness, float NdotL, float NdotV, float NdotH, float VdotH, out float3 fresnel)
{
    const float alpha = max(roughness * roughness, 1e-3);
    fresnel = lerp(F0, F90, schlickFresnel(VdotH));
    return fresnel * ggxDistribution(NdotH, alpha) * smithVisibility(NdotL, NdotV, alpha);
}

// The reflectance at normal incidence and grazing angles of the base specular lobe, and the diffuse
// colour that is left.
public void specularReflectance(PreviewSurfaceSample s, out float3 F0, out float3 F90, out float3 diffuse)
{
    if (s.useSpecularWorkflow) {
        F0 = s.specularColor;
        F90 = float3(1.0, 1.0, 1.0);
        diffuse = s.diffuseColor;
        return;
    }
    const float r = (1.0 - s.ior) / (1.0 + s.ior);
    const float3 dielectric = float3(r * r, r * r, r * r);
    F0 = lerp(dielectric, s.diffuseColor, s.metallic);
    F90 = float3(1.0, 1.0, 1.0);
    diffuse = s.diffuseColor * (1.0 - s.metallic);
}

// The reflected radiance per unit incoming radiance from L towards V, cosine included. V and L point
// away from the surface.
public float3 evaluatePreviewSurface(PreviewSurfaceSample s, float3 V, float3 L)
{
    const float3 N = s.normal;
    const float NdotL = dot(N, L);
    const float NdotV = dot(N, V);

    if (NdotL <= 0.0 || NdotV <= 0.0)
        return float3(0.0, 0.0, 0.0);

    const float3 H = normalize(L + V);
    const float NdotH = max(dot(N, H), 0.0);
    const float VdotH = max(dot(V, H), 0.0);
    float3 F0;
    float3 F90;
    float3 diffuse;
    specularReflectance(s, F0, F90, diffuse);

    float3 fresnel;
    const float3 specular = specularLobe(F0, F90, s.roughness, NdotL, NdotV, NdotH, VdotH, fresnel);
    float3 result = diffuse * (1.0 / kPi) * (float3(1.0, 1.0, 1.0) - fresnel) + specular;

    if (s.clearcoat > 0.0) {
        float3 coatFresnel;
        const float3 coat = specularLobe(float3(0.04, 0.04, 0.04), float3(1.0, 1.0, 1.0), s.clearcoatRoughness, NdotL, NdotV, NdotH, VdotH, coatFresnel);
        result = result * (float3(1.0, 1.0, 1.0) - s.clearcoat * coatFresnel) + s.clearcoat * coat;
    }
    return result * NdotL;
}

// Tangent-space normal n (z out of the surface) through the frame of the triangle's positions and
// texture coordinates; the geometric normal when the coordinates give no frame.
public float3 applyNormalMap(float3 geometric, float3 n, float3 dp1, float3 dp2, float2 duv1, float2 duv2)
{
    const float det = duv1.x * duv2.y - duv1.y * duv2.x;

    if (abs(det) < 1e-12)
        return geometric;

    const float r = 1.0 / det;
    float3 T = (dp1 * duv2.y - dp2 * duv1.y) * r;
    T = T - geometric * dot(geometric, T);

    const float lengthT = length(T);

    if (lengthT < 1e-12)
        return geometric;
    T = T / lengthT;

    float3 B = cross(geometric, T);
    if (dot(B, (dp2 * duv1.x - dp1 * duv2.x) * r) < 0.0)
        B = -B;

    const float3 mapped = n.x * T + n.y * B + n.z * geometric;
    const float lengthM = length(mapped);
    return lengthM > 1e-12 ? mapped / lengthM : geometric;
}
