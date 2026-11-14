module UtilityTypes;

// A batch of N rays is one float buffer holding eight arrays of N values, in this order. Tracer::RayBatch
// addresses the same memory.
public static const uint kRayOriginX = 0;
public static const uint kRayOriginY = 1;
public static const uint kRayOriginZ = 2;
public static const uint kRayDirectionX = 3;
public static const uint kRayDirectionY = 4;
public static const uint kRayDirectionZ = 5;
public static const uint kRayTMin = 6;
public static const uint kRayTMax = 7;
public static const uint kRayArrays = 8;

// Hits are two buffers of three arrays of N values each, as Tracer::HitBatch addresses them.
public static const uint kHitT = 0;
public static const uint kHitU = 1;
public static const uint kHitV = 2;

public static const uint kHitGeometry = 0;
public static const uint kHitInstance = 1;
public static const uint kHitPrimitive = 2;

public static const uint kNoHit = 0xffffffffu;

// UtilityScene::ColorBinding
public static const uint kColorBindingConstant = 0;
public static const uint kColorBindingTriangle = 1;
public static const uint kColorBindingVertex = 2;
public static const uint kColorBindingCorner = 3;

// A geometry without texture coordinates.
public static const uint kStBindingNone = 0xffffffffu;

// KernelGeometryRecord in UtilityKernels.h mirrors this. stBinding is a colour binding or kStBindingNone.
public struct GeometryRecord
{
    public uint colorOffset;
    public uint colorBinding;
    public int primId;
    public uint triangleOffset;
    public uint materialIndex;
    public uint stOffset;
    public uint stBinding;
    public uint pointOffset;
};

// UtilityMaterialDesc::Kind
public static const uint kMaterialPreviewSurface = 0;
public static const uint kMaterialUnsupported = 1;
public static const uint kMaterialProject3D = 2;

// MaterialRecord::kind.z bits.
public static const uint kMaterialDiffuseFromDisplayColor = 1;
public static const uint kMaterialOpacityFromDisplayOpacity = 2;
public static const uint kMaterialEmissiveFromDisplayColor = 4;

// The texture slots of a material, UtilityMaterialDesc::Slot.
public static const uint kSlotDiffuseColor = 0;
public static const uint kSlotEmissiveColor = 1;
public static const uint kSlotSpecularColor = 2;
public static const uint kSlotMetallic = 3;
public static const uint kSlotRoughness = 4;
public static const uint kSlotClearcoat = 5;
public static const uint kSlotClearcoatRoughness = 6;
public static const uint kSlotOpacity = 7;
public static const uint kSlotNormal = 8;
public static const uint kSlotIor = 9;

public static const int kNoTexture = -1;

// A texture lookup footprint that always reads the full-size level.
public static const float kFinestLevel = -1.0e30;

// KernelMaterialRecord mirrors this. kind: kind, projection index, flags. textures: a texture binding
// per slot, or kNoTexture.
public struct MaterialRecord
{
    public float4 diffuseColor;
    public float4 emissiveColor;
    // w: useSpecularWorkflow.
    public float4 specularColor;
    // metallic, roughness, clearcoat, clearcoatRoughness.
    public float4 surface;
    // opacity, opacityThreshold, ior.
    public float4 coverage;
    public uint4 kind;
    public int4 textures0;
    public int4 textures1;
    public int4 textures2;
};

// UtilityTextureRef::Wrap
public static const int kWrapBlack = 0;
public static const int kWrapClamp = 1;
public static const int kWrapRepeat = 2;
public static const int kWrapMirror = 3;

// KernelTextureBinding mirrors this: one UsdUVTexture read. image: the image record, or kNoTexture for the
// fallback; whether it starts a run of 100 UDIM tiles; wrapS; wrapT. channels.x: the first channel the
// output reads.
public struct TextureBinding
{
    public float4 scale;
    public float4 bias;
    public float4 fallback;
    // UsdTransform2d: scale xy, translation zw.
    public float4 uvTransform;
    // cos and sin of the UsdTransform2d rotation.
    public float4 uvRotation;
    public int4 image;
    public uint4 channels;
};

// An image record: width, height, mip levels and the offset of its first texel. Texels are linear
// straight RGBA, rows from the bottom, each level after the one twice its size.
public static const uint kUdimTiles = 100;

// KernelProjectionRecord mirrors this: the inputs of natronProject3D.glslfx. Matrices are rows of
// row-vector GfMatrix4d values.
public struct ProjectionRecord
{
    public float4 worldToProjector0;
    public float4 worldToProjector1;
    public float4 worldToProjector2;
    public float4 worldToProjector3;
    public float4 geometryFrame0;
    public float4 geometryFrame1;
    public float4 geometryFrame2;
    public float4 geometryFrame3;
    public float4 geometryFrameInverse0;
    public float4 geometryFrameInverse1;
    public float4 geometryFrameInverse2;
    public float4 geometryFrameInverse3;
    public float4 plateTransform;
    public float4 projectorOrigin;
    // projectionSpace, projectOn, crop, clip.
    public int4 settings;
    // outside, the plate's texture binding.
    public int4 plate;
};

// Each instance has eight rows in the instance transform table: object to world, then its inverse.
public static const uint kInstanceTransformRows = 8;

public float4 transformRow(float4 v, float4 r0, float4 r1, float4 r2, float4 r3)
{
    return v.x * r0 + v.y * r1 + v.z * r2 + v.w * r3;
}

// UtilityLightDesc::Type
public static const uint kLightDistant = 0;
public static const uint kLightSphere = 1;
public static const uint kLightRect = 2;
public static const uint kLightDisk = 3;
public static const uint kLightCylinder = 4;
public static const uint kLightDome = 5;

// LightRecord::kind.y bits.
public static const uint kLightCastsShadows = 1;
public static const uint kLightIes = 2;
public static const uint kLightShaped = 4;
public static const uint kLightDomeImportance = 8;
public static const uint kLightPoint = 16;

// KernelLightRecord mirrors this: one light in world space, its axes unit length.
//   position: the light's origin; w the sphere's or cylinder's radius.
//   axisX: w half the rect's width, the disk's radius along X or half the cylinder's length.
//   axisY: w half the rect's height or the disk's radius along Y.
//   axisZ: w the cosine of the distant light's half angle, 1 for a delta direction.
//   radiance: emitted radiance, or intensity for a point; w the emitting area.
//   shaping: cone angle in radians, cone softness, focus, IES angle scale.
//   focusTint: w the factor on the IES table.
//   kind: type, flags, the dome's texture binding or kNoTexture, the IES table's offset in the light data.
//   data: the dome CDF's offset in the light data, its width and height, the IES table's theta count
//   in the low 16 bits and phi count in the high 16.
public struct LightRecord
{
    public float4 position;
    public float4 axisX;
    public float4 axisY;
    public float4 axisZ;
    public float4 radiance;
    public float4 shaping;
    public float4 focusTint;
    public uint4 kind;
    public uint4 data;
};
