import UtilityTypes;

// KernelFilmPickParams in UtilityRenderBuffer.cpp mirrors this. A batch is a run of whole rows of the
// window starting at its pixel first, its pixels in the order the ray generation made its rays, one ray each.
struct FilmPickParams
{
    uint first;
    uint count;
    // Whether this is the frame's first pass, whose samples replace what the picks hold.
    uint fresh;
    uint padding0;
};

// kFilmPickWords words a pixel: the heaviest sample's weight, whether it hit, then its depth, prim id and
// instance id, which mean nothing for a miss.
static const uint kFilmPickWords = 5;

// The geometry array comes first in the hit ids, so the first count values tell hit from miss.
[[vk::binding(0, 0)]] StructuredBuffer<uint> hitIds;
[[vk::binding(1, 0)]] StructuredBuffer<float> weights;
[[vk::binding(2, 0)]] StructuredBuffer<float> depth;
[[vk::binding(3, 0)]] StructuredBuffer<int> primId;
[[vk::binding(4, 0)]] StructuredBuffer<int> instanceId;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> picks;
[[vk::push_constant]] ConstantBuffer<FilmPickParams> params;

// Keeps each pixel's heaviest sample, the first of equal ones, as UtilityRenderBuffer::addSample() does
// for an unfiltered buffer: a mean of depths or ids would be a value no surface has.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityFilmPick(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.count)
        return;
    const uint at = (params.first + id.x) * kFilmPickWords;
    const float weight = weights[id.x];
    const float heaviest = (params.fresh != 0) ? -1.0 : asfloat(picks[at]);
    if (weight <= heaviest) {
        if (params.fresh != 0)
            picks[at] = asuint(-1.0f);
        return;
    }
    const bool hit = hitIds[kHitGeometry * params.count + id.x] != kNoHit;
    picks[at] = asuint(weight);
    picks[at + 1] = hit ? 1u : 0u;
    picks[at + 2] = asuint(depth[id.x]);
    picks[at + 3] = asuint(primId[id.x]);
    picks[at + 4] = asuint(instanceId[id.x]);
}
