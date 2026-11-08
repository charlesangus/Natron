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

// KernelGeometryRecord in UtilityKernels.h mirrors this.
public struct GeometryRecord
{
    public uint colorOffset;
    public uint colorBinding;
    public int primId;
    public uint triangleOffset;
};
