module UtilityTypes;

// A batch of N rays is one float buffer holding nine arrays of N values, in this order. Tracer::RayBatch
// addresses the same memory. A ray's time runs from 0 at the shutter's opening to 1 at its closing.
public static const uint kRayOriginX = 0;
public static const uint kRayOriginY = 1;
public static const uint kRayOriginZ = 2;
public static const uint kRayDirectionX = 3;
public static const uint kRayDirectionY = 4;
public static const uint kRayDirectionZ = 5;
public static const uint kRayTMin = 6;
public static const uint kRayTMax = 7;
public static const uint kRayTime = 8;
public static const uint kRayArrays = 9;

// Hits are two buffers of three arrays of N values each, as Tracer::HitBatch addresses them.
public static const uint kHitT = 0;
public static const uint kHitU = 1;
public static const uint kHitV = 2;

public static const uint kHitGeometry = 0;
public static const uint kHitInstance = 1;
public static const uint kHitPrimitive = 2;

public static const uint kNoHit = 0xffffffffu;

// A table read through its address rather than a descriptor, which the storage buffer range would bound: a
// scene's points or triangles alone can run past it. The address is two words, low first, because the 64-bit
// form needs shaderInt64, which the device does not enable; the C++ build reads host addresses the same way.
// alignment is that of T, which the device's loads may assume.
public struct DeviceTable<T, let alignment : int>
{
    public uint2 address;

    public __init(uint2 a)
    {
        address = a;
    }

    public __subscript(uint index)->T
    {
        get
        {
            __target_switch
            {
            case spirv:
                T* base = spirv_asm {
                    OpCapability PhysicalStorageBufferAddresses;
                    OpExtension "SPV_KHR_physical_storage_buffer";
                    result:$$T* = OpBitcast $address
                };
                return loadAligned<alignment>(base + index);
            case cpp:
                T* base = bit_cast<T*>((uint64_t(address.y) << 32) | uint64_t(address.x));
                return base[index];
            }
        }
    }
};

public typealias Float4Table = DeviceTable<float4, 16>;
public typealias Uint4Table = DeviceTable<uint4, 16>;
public typealias Float2Table = DeviceTable<float2, 8>;
public typealias FloatTable = DeviceTable<float, 4>;
public typealias IntTable = DeviceTable<int, 4>;
public typealias UintTable = DeviceTable<uint, 4>;
public typealias GeometryTable = DeviceTable<GeometryRecord, 4>;

// The slots of the table address buffers ShadeSurface and ResolveHits bind; KernelTableSlot in
// UtilityKernels.h mirrors these.
public static const uint kTableGeometries = 0;
public static const uint kTableColors = 1;
public static const uint kTableTriangles = 2;
public static const uint kTableInstanceIds = 3;
public static const uint kTablePoints = 4;
public static const uint kTableInstanceTransforms = 5;
public static const uint kTableSts = 6;
public static const uint kTableNormals = 7;
public static const uint kTableLightData = 8;
public static const uint kTableLinkMasks = 9;
public static const uint kTableTexels = 10;
// Per instance, the first row of its steps in kTableMotionRows, or kNoMotion where it does not move.
public static const uint kTableInstanceMotion = 11;
public static const uint kTableMotionRows = 12;
public static const uint kTableCount = 13;

// UtilityScene::ColorBinding
public static const uint kColorBindingConstant = 0;
public static const uint kColorBindingTriangle = 1;
public static const uint kColorBindingVertex = 2;
public static const uint kColorBindingCorner = 3;
// GeometryRecord::colorBinding holds the binding in its low byte and these flags above it.
public static const uint kColorBindingMask = 0xffu;
public static const uint kColorNoDisplayColor = 0x100u;
public static const uint kColorNoDisplayOpacity = 0x200u;

// A geometry without texture coordinates.
public static const uint kStBindingNone = 0xffffffffu;
// A geometry without shading normals, shaded faceted.
public static const uint kNormalBindingNone = 0xffffffffu;

// KernelGeometryRecord in UtilityKernels.h mirrors this. stBinding is a colour binding or kStBindingNone,
// normalBinding one or kNormalBindingNone.
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
    public uint normalOffset;
    public uint normalBinding;
    // A deforming geometry holds its points, and its normals when they move with them, at each of
    // pointSteps or normalSteps times, the stride apart; a still one has one step.
    public uint pointSteps;
    public uint pointStride;
    public uint normalSteps;
    public uint normalStride;
};

// UtilityMaterialDesc::Kind
public static const uint kMaterialPreviewSurface = 0;
public static const uint kMaterialUnsupported = 1;
public static const uint kMaterialProject3D = 2;

// MaterialRecord::kind.z bits.
public static const uint kMaterialDiffuseFromDisplayColor = 1;
public static const uint kMaterialOpacityFromDisplayOpacity = 2;
public static const uint kMaterialEmissiveFromDisplayColor = 4;
public static const uint kMaterialOpacityPresence = 8;

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
    // The w components of diffuseColor, emissiveColor and coverage hold the constant tangent-space normal.
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

// Whether a surface's ior bends the rays through it; at an ior of 1 it is plain transparency.
public bool bendsLight(float ior)
{
    return abs(ior - 1.0) >= 1e-4;
}

// KernelPathState in UtilityKernels.h mirrors this: what a ray after the camera's carries to its pixel.
// throughput scales the light the ray finds, w the share of the pixel's alpha what it hits adds.
// counters: the pixel's index in the batch, then the bounces taken in all, by reflection and by
// refraction.
public struct PathState
{
    public float4 throughput;
    public uint4 counters;
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

public static const uint kNoMotion = 0xffffffffu;

// UtilityScene::Decomposition: column-vector M = T * R * U. A moving instance's steps lie in the motion rows
// as kMotionRowsPerStep rows each: (U00, U01, U02, 0), (U11, U12, U22, 0), (T, 0), the quaternion (i, j, k, r).
public static const uint kMotionRowsPerStep = 4;

public struct MotionStep
{
    public float3 stretchTop;
    public float3 stretchRest;
    public float3 translation;
    public float4 rotation;
};

// The span of steps time steps a time falls in and how far into it, as UtilityScene::stepAt() finds them.
public void motionSpan(float time, uint steps, out uint span, out float fraction)
{
    const uint segments = max(steps, 2u) - 1u;
    const float scaled = clamp(time, 0.0, 1.0) * float(segments);
    span = min(uint(floor(scaled)), segments - 1u);
    fraction = scaled - float(span);
}

public float4 slerpRotation(float4 a, float4 b, float u)
{
    float cosine = dot(a, b);
    float4 to = b;
    if (cosine < 0.0) {
        to = -b;
        cosine = -cosine;
    }
    if (cosine > 0.9995)
        return normalize(lerp(a, to, u));
    const float theta = acos(min(cosine, 1.0));
    const float phi = u * theta;
    return cos(phi) * a + sin(phi) * normalize(to - cosine * a);
}

public MotionStep blendSteps(MotionStep a, MotionStep b, float u)
{
    MotionStep out;
    out.stretchTop = lerp(a.stretchTop, b.stretchTop, u);
    out.stretchRest = lerp(a.stretchRest, b.stretchRest, u);
    out.translation = lerp(a.translation, b.translation, u);
    out.rotation = slerpRotation(a.rotation, b.rotation, u);
    return out;
}

// The rotation's columns, in column vectors.
public void rotationColumns(float4 q, out float3 c0, out float3 c1, out float3 c2)
{
    const float i = q.x;
    const float j = q.y;
    const float k = q.z;
    const float r = q.w;
    const float n = dot(q, q);
    const float s = n > 0.0 ? 2.0 / n : 0.0;
    c0 = float3(1.0 - s * (j * j + k * k), s * (i * j + k * r), s * (i * k - j * r));
    c1 = float3(s * (i * j - k * r), 1.0 - s * (i * i + k * k), s * (j * k + i * r));
    c2 = float3(s * (i * k + j * r), s * (j * k - i * r), 1.0 - s * (i * i + j * j));
}

/**
 * The step's transform as the instance transform table holds one: o0 to o3 the rows of the row-vector
 * object-to-world matrix, w0 to w3 those of its inverse. The column-vector linear part L = R * U has
 * columns L_j = sum over x of U[x][j] * R_x, and row-vector rows that are those columns.
 **/
public void composeStep(MotionStep step, out float4 o0, out float4 o1, out float4 o2, out float4 o3,
                        out float4 w0, out float4 w1, out float4 w2, out float4 w3)
{
    float3 r0;
    float3 r1;
    float3 r2;
    rotationColumns(step.rotation, r0, r1, r2);
    const float3 l0 = step.stretchTop.x * r0;
    const float3 l1 = step.stretchTop.y * r0 + step.stretchRest.x * r1;
    const float3 l2 = step.stretchTop.z * r0 + step.stretchRest.y * r1 + step.stretchRest.z * r2;
    o0 = float4(l0, 0.0);
    o1 = float4(l1, 0.0);
    o2 = float4(l2, 0.0);
    o3 = float4(step.translation, 1.0);
    // The row-vector inverse's rows are the cofactor columns of the row-vector linear part over its
    // determinant, whose rows are l0, l1 and l2.
    const float3 c0 = cross(l1, l2);
    const float3 c1 = cross(l2, l0);
    const float3 c2 = cross(l0, l1);
    const float determinant = dot(l0, c0);
    const float scale = determinant != 0.0 ? 1.0 / determinant : 0.0;
    const float3 i0 = float3(c0.x, c1.x, c2.x) * scale;
    const float3 i1 = float3(c0.y, c1.y, c2.y) * scale;
    const float3 i2 = float3(c0.z, c1.z, c2.z) * scale;
    w0 = float4(i0, 0.0);
    w1 = float4(i1, 0.0);
    w2 = float4(i2, 0.0);
    w3 = float4(-(step.translation.x * i0 + step.translation.y * i1 + step.translation.z * i2), 1.0);
}

public MotionStep readStep(Float4Table rows, uint first)
{
    MotionStep step;
    const float4 a = rows[first];
    const float4 b = rows[first + 1];
    step.stretchTop = a.xyz;
    step.stretchRest = b.xyz;
    step.translation = rows[first + 2].xyz;
    step.rotation = rows[first + 3];
    return step;
}

// An instance's transform at a time, from the rows of its steps; the header row before them holds the
// step count in x.
public MotionStep stepAtTime(Float4Table rows, uint first, float time)
{
    const uint steps = asuint(rows[first].x);
    uint span;
    float fraction;
    motionSpan(time, steps, span, fraction);
    const uint at = first + 1 + span * kMotionRowsPerStep;
    return blendSteps(readStep(rows, at), readStep(rows, at + kMotionRowsPerStep), fraction);
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
