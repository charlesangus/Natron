// KeyerOp::row() in Engine/Nodes/Keyer/Keyer.cpp: the alpha is the trapezoid of the key value,
// RGB passes through. KeyerGpuParams there mirrors the parameter block: flags, the three key
// weights and the ordered range A, B, C, D, with one rounding per statement as the CPU computes.
// Flag bits 1 and 2 hold the key channel plus one; 0 keys by the weights.
import pixelwrap;

static const uint kKeyerLumaMax = 1u;
static const uint kKeyerChannelShift = 1u;
static const uint kKeyerChannelMask = 3u;

// std::max, whose NaN handling GLSL max leaves undefined.
float keyerMax(float a, float b)
{
    return (a < b) ? b : a;
}

struct KeyerOp : IPixelOp
{
    uint flags;
    float3 weights;
    float4 range;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        flags = params[0];
        weights = float3(wrapParamFloat(params, 1), wrapParamFloat(params, 2), wrapParamFloat(params, 3));
        range = wrapParamFloat4(params, 4);
    }

    float keyValue(float3 rgb)
    {
        uint channel = (flags >> kKeyerChannelShift) & kKeyerChannelMask;
        if (channel == 1u)
            return rgb.r;
        if (channel == 2u)
            return rgb.g;
        if (channel == 3u)
            return rgb.b;
        if ((flags & kKeyerLumaMax) != 0u)
            return keyerMax(keyerMax(rgb.r, rgb.g), rgb.b);
        precise float s = weights.x * rgb.r;
        precise float g = weights.y * rgb.g;
        s = s + g;
        precise float b = weights.z * rgb.b;
        s = s + b;
        return s;
    }

    float matte(float k)
    {
        if (k >= range.y && k <= range.z)
            return 1.0;
        if (k <= range.x || k >= range.w)
            return 0.0;
        if (k < range.y)
        {
            precise float num = k - range.x;
            precise float den = range.y - range.x;
            precise float o = num / den;
            return o;
        }
        precise float num = range.w - k;
        precise float den = range.w - range.z;
        precise float o = num / den;
        return o;
    }

    float4 apply(float4 v, int2 xy)
    {
        return float4(v.rgb, matte(keyValue(v.rgb)));
    }
};

[shader("compute")]
[numthreads(8, 8, 1)]
void keyer_wrapped(uint3 tid : SV_DispatchThreadID)
{
    KeyerOp op;
    runWrapped<KeyerOp>(op, tid);
}
