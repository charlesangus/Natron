import pixelwrap;

// Floats ordered as integers: devices may flush denormals to zero in a float comparison, while
// the CPU orders a denormal below or above zero. -0 and +0 share key 0.
int clampOrderKey(float f)
{
    const uint u = asuint(f);
    return (u & 0x80000000u) != 0 ? -int(u & 0x7fffffffu) : int(u);
}

bool clampIsNaN(float f)
{
    return (asuint(f) & 0x7fffffffu) > 0x7f800000u;
}

// a < b as IEEE orders them, false when either is a NaN.
bool clampLess(float a, float b)
{
    return !clampIsNaN(a) && !clampIsNaN(b) && clampOrderKey(a) < clampOrderKey(b);
}

struct ClampOp : IPixelOp
{
    float4 minTest;
    float4 maxTest;
    float4 minValue;
    float4 maxValue;
    float4 minClampTo;
    float4 maxClampTo;
    uint flags;

    // Parameter block, in words: minTest, maxTest, minValue, maxValue, minClampTo, maxClampTo (4
    // each), flags. The tests are the knob values rounded so that a float v compares against them
    // as it does against the double knob on the CPU; the values are the ones written.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        minTest = wrapParamFloat4(params, 0);
        maxTest = wrapParamFloat4(params, 4);
        minValue = wrapParamFloat4(params, 8);
        maxValue = wrapParamFloat4(params, 12);
        minClampTo = wrapParamFloat4(params, 16);
        maxClampTo = wrapParamFloat4(params, 20);
        flags = params[24];
    }

    float4 apply(float4 v, int2 xy)
    {
        const bool minEnable = (flags & 1) != 0;
        const bool maxEnable = (flags & 2) != 0;
        const bool minToEnable = (flags & 4) != 0;
        const bool maxToEnable = (flags & 8) != 0;
        [ForceUnroll]
        for (uint c = 0; c < 4; ++c)
        {
            const float x = v[c];
            // A NaN fails both tests and passes through, as in the engine kernel's if / else if.
            if (minEnable && clampLess(x, minTest[c]))
                v[c] = minToEnable ? minClampTo[c] : minValue[c];
            else if (maxEnable && clampLess(maxTest[c], x))
                v[c] = maxToEnable ? maxClampTo[c] : maxValue[c];
        }
        return v;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void clamp_wrapped(uint3 tid : SV_DispatchThreadID)
{
    ClampOp op;
    runWrapped<ClampOp>(op, tid);
}
