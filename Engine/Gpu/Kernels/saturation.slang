import pixelwrap;

struct SaturationOp : IPixelOp
{
    float saturation;
    uint flags;
    float3 weights;

    // Parameter block, in words: saturation, flags (1 clamp black, 2 clamp white, 4 luminance is
    // the channel maximum), then the three luminance weights.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        saturation = wrapParamFloat(params, 0);
        flags = params[1];
        weights = float3(wrapParamFloat(params, 2), wrapParamFloat(params, 3), wrapParamFloat(params, 4));
    }

    float4 apply(float4 v, int2 xy)
    {
        const float l = (flags & 4) != 0 ? max(max(v.r, v.g), v.b) : dot(weights, v.rgb);
        float4 o = v;
        o.rgb = (1.0 - saturation) * l + saturation * v.rgb;
        // Ordered like std::max(0, x) and std::min(1, x): a NaN takes the bound.
        [ForceUnroll]
        for (uint c = 0; c < 4; ++c)
        {
            float x = o[c];
            if ((flags & 1) != 0)
                x = (0.0 < x) ? x : 0.0;
            if ((flags & 2) != 0)
                x = (x < 1.0) ? x : 1.0;
            o[c] = x;
        }
        return o;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void saturation_wrapped(uint3 tid : SV_DispatchThreadID)
{
    SaturationOp op;
    runWrapped<SaturationOp>(op, tid);
}
