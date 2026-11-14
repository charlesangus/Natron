import UtilityTypes;

// KernelShadeLightsParams in UtilityKernels.h mirrors this: count hits, each with lightBatch shadow rays
// queued light-major by ShadeSurface.
struct ShadeLightsParams
{
    uint count;
    uint lightBatch;
    uint padding0;
    uint padding1;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> colorOut;
[[vk::binding(1, 0)]] StructuredBuffer<float4> shadowContributions;
// 1 where the shadow ray met an occluder, as Tracer::occluded and utilityOccluded write it.
[[vk::binding(2, 0)]] StructuredBuffer<uint> blocked;
[[vk::push_constant]] ConstantBuffer<ShadeLightsParams> params;

// Adds the contributions of the shadow rays nothing blocked. Coverage is ShadeSurface's, so alpha stays.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityShadeLights(uint3 id : SV_DispatchThreadID)
{
    const uint count = params.count;

    if (id.x >= count)
        return;

    float3 sum = float3(0.0);

    for (uint j = 0; j < params.lightBatch; ++j) {
        const uint slot = j * count + id.x;

        if (blocked[slot] == 0)
            sum += shadowContributions[slot].xyz;
    }

    const float4 color = colorOut[id.x];
    colorOut[id.x] = float4(color.xyz + sum, color.w);
}
