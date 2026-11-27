import UtilityTypes;

// KernelVisibilityParams in UtilityKernels.h mirrors this. One draw covers the instances of one geometry: its
// triangles come from the index buffer the draw binds, and each instance's entry in the instances table, from
// the draw's first instance on, holds the placement a hit reports and the first row of its transform in rows.
//
// A transform's first row holds its step count in x: 0 for an instance that holds still, followed by the four
// rows of its row-vector object-to-world matrix, or two or more, followed by that many steps as stepAtTime()
// reads them. A deforming geometry's points hold pointSteps steps of pointCount points, one after another.
struct VisibilityParams
{
    // The rows of the row-vector view-projection, into Vulkan's clip space: y runs down the attachment and depth
    // from 0 at the near plane to w at the far one.
    float4 viewProjection0;
    float4 viewProjection1;
    float4 viewProjection2;
    float4 viewProjection3;
    // The pass's sub-pixel offset in normalized device coordinates, applied after the projection so that it moves
    // every depth alike.
    float2 jitter;
    // From 0 at the shutter's opening to 1 at its closing, as a ray's time.
    float time;
    uint pointCount;
    uint2 points;
    uint2 instances;
    uint2 rows;
    uint pointSteps;
    uint unused;
};
[[vk::push_constant]] ConstantBuffer<VisibilityParams> params;

struct VisibilityVarying
{
    float4 position : SV_Position;
    nointerpolation uint placement : PLACEMENT;
};

float3 pointAt(FloatTable points, uint index)
{
    return float3(points[3 * index], points[3 * index + 1], points[3 * index + 2]);
}

[shader("vertex")]
VisibilityVarying visibilityVertex(uint vertex : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID)
{
    const UintTable instances = UintTable(params.instances);
    const uint placement = instances[2 * instance];
    const uint first = instances[2 * instance + 1];

    const FloatTable points = FloatTable(params.points);
    float3 local;
    if (params.pointSteps > 1) {
        uint span;
        float fraction;
        motionSpan(params.time, params.pointSteps, span, fraction);
        const uint from = span * params.pointCount + vertex;
        local = lerp(pointAt(points, from), pointAt(points, from + params.pointCount), fraction);
    } else {
        local = pointAt(points, vertex);
    }

    const Float4Table rows = Float4Table(params.rows);
    float4 o0, o1, o2, o3;
    if (asuint(rows[first].x) == 0) {
        o0 = rows[first + 1];
        o1 = rows[first + 2];
        o2 = rows[first + 3];
        o3 = rows[first + 4];
    } else {
        float4 w0, w1, w2, w3;
        composeStep(stepAtTime(rows, first, params.time), o0, o1, o2, o3, w0, w1, w2, w3);
    }
    const float4 world = transformRow(float4(local, 1.0), o0, o1, o2, o3);
    float4 clip = transformRow(world, params.viewProjection0, params.viewProjection1, params.viewProjection2,
                               params.viewProjection3);
    clip.xy += params.jitter * clip.w;

    VisibilityVarying o;
    o.position = clip;
    o.placement = placement;
    return o;
}

// The placement and the triangle, which an indexed draw numbers from 0 in index-buffer order as the geometry's
// triangle structure does, so the pair is the instance and primitive a ray's hit reports.
[shader("fragment")]
uint2 visibilityFragment(VisibilityVarying input, uint primitive : SV_PrimitiveID) : SV_Target
{
    return uint2(input.placement, primitive);
}
