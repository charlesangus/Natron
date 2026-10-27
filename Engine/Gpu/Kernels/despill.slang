import pixelwrap;

// The Despill node, with Despill.cpp written the same way, one rounding per statement, so the
// CPU and the GPU agree to float rounding.

static const uint kDespillHasAxis = 1u;
static const uint kDespillSpillMapAlpha = 2u;
static const uint kDespillLimitMax = 1u;
static const uint kDespillLimitMin = 2u;

// Row r of the rotation applied to rgb, summed in order with no fused multiply-add.
float despillRotated(float3 m, float3 rgb)
{
    precise float s = m.x * rgb.r;
    precise float g = m.y * rgb.g;
    s = s + g;
    precise float b = m.z * rgb.b;
    s = s + b;
    return s;
}

struct DespillOp : IPixelOp
{
    uint flags;
    uint limitKind;
    float3 rot[3];
    float2 weights;
    float expandScale;
    float spillScale;
    float strength;
    float3 move;

    // Parameter block, in words: flags (1 has an axis, 2 spill map to alpha), the limit kind
    // (0 weighted, 1 max, 2 min), the rotation's rows, the two weights, the expand scale, the
    // spill scale, the strength and the move.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        flags = params[0];
        limitKind = params[1];
        [ForceUnroll]
        for (uint r = 0; r < 3; ++r)
            rot[r] = float3(wrapParamFloat(params, 2 + r * 3), wrapParamFloat(params, 3 + r * 3), wrapParamFloat(params, 4 + r * 3));
        weights = float2(wrapParamFloat(params, 11), wrapParamFloat(params, 12));
        expandScale = wrapParamFloat(params, 13);
        spillScale = wrapParamFloat(params, 14);
        strength = wrapParamFloat(params, 15);
        move = float3(wrapParamFloat(params, 16), wrapParamFloat(params, 17), wrapParamFloat(params, 18));
    }

    float4 apply(float4 v, int2 xy)
    {
        if ((flags & kDespillHasAxis) == 0)
        {
            const float a = ((flags & kDespillSpillMapAlpha) != 0) ? 0.0 : v.a;
            return float4(v.rgb, a);
        }
        const float r = despillRotated(rot[0], v.rgb);
        const float g = despillRotated(rot[1], v.rgb);
        const float b = despillRotated(rot[2], v.rgb);
        precise float lim;
        if (limitKind == kDespillLimitMax)
            lim = (r > b) ? r : b;
        else if (limitKind == kDespillLimitMin)
            lim = (r < b) ? r : b;
        else
        {
            lim = weights.x * r;
            precise float lb = weights.y * b;
            lim = lim + lb;
        }
        precise float scaled = expandScale * lim;
        precise float m = g - scaled;
        // A NaN spill is none.
        const float positive = (m > 0.0) ? m : 0.0;
        precise float map = positive * spillScale;
        precise float amount = map * strength;
        float4 o = v;
        [ForceUnroll]
        for (uint c = 0; c < 3; ++c)
        {
            precise float d = amount * move[c];
            precise float w = v[c] + d;
            o[c] = w;
        }
        if ((flags & kDespillSpillMapAlpha) != 0)
            o.a = map;
        return o;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void despill_wrapped(uint3 tid : SV_DispatchThreadID)
{
    DespillOp op;
    runWrapped<DespillOp>(op, tid);
}
