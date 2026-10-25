// A whole-pixel translation of the source, as runResampleCpu's offset form reads it: every
// channel of each output pixel is the source pixel it shifts from, or zero outside the source.
// Pixels travel as their bits, so the copy is exact whatever they hold. With a matte, each pixel
// is then weighed as ResampleMatte describes. GpuOffsetParams in Engine/Gpu/GpuResample.h mirrors
// OffsetParams.

// The destination's offset addresses the output origin. Threads inside [validX1, validX2) x
// [validY1, validY2) read the source, whose offset addresses the pixel thread (validX1, validY1)
// reads; the others write zero. With kOffsetFlagMatte, offsetWeights holds a (hi, lo, black)
// triple per output column from the origin, then from rowWeights one per output row.
struct OffsetParams
{
    uint width;
    uint height;
    uint dstOffset;
    uint dstStride;
    uint srcOffset;
    uint srcStride;
    uint validX1;
    uint validY1;
    uint validX2;
    uint validY2;
    uint nComps;
    uint flags;
    uint rowWeights;
};

static const uint kOffsetFlagMatte = 1;

[[vk::binding(0, 0)]] StructuredBuffer<uint> offsetSrc;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> offsetDst;
[[vk::binding(2, 0)]] StructuredBuffer<float> offsetWeights;
[[vk::push_constant]] ConstantBuffer<OffsetParams> offsetParams;

// A weight as two floats, hi + lo with |lo| at most half an ulp of hi.
struct Weight
{
    float hi;
    float lo;
};

Weight weightAt(uint index)
{
    Weight w;
    w.hi = offsetWeights[index];
    w.lo = offsetWeights[index + 1];
    return w;
}

Weight weightProduct(Weight a, Weight b)
{
    precise float p = a.hi * b.hi;
    precise float e = fma(a.hi, b.hi, -p);
    precise float lo = e + (a.hi * b.lo + a.lo * b.hi);
    precise float s = p + lo;
    Weight w;
    w.hi = s;
    w.lo = lo - (s - p);
    return w;
}

// v * w rounded to float, where the CPU rounds the double product. A product that is not finite,
// or zero, is the CPU's already, and its error term would be NaN or lose the zero's sign.
float weigh(float v, Weight w)
{
    precise float p = v * w.hi;
    if (isinf(p) || isnan(p) || p == 0.0)
        return p;
    precise float e = fma(v, w.hi, -p);
    precise float lo = e + v * w.lo;
    return p + lo;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void offset_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= offsetParams.width || tid.y >= offsetParams.height)
        return;
    const uint n = offsetParams.nComps;
    const uint dst = offsetParams.dstOffset + tid.y * offsetParams.dstStride + tid.x * n;
    const bool inside = tid.x >= offsetParams.validX1 && tid.x < offsetParams.validX2
                        && tid.y >= offsetParams.validY1 && tid.y < offsetParams.validY2;
    uint pixel[4] = { 0u, 0u, 0u, 0u };
    if (inside)
    {
        const uint src = offsetParams.srcOffset + (tid.y - offsetParams.validY1) * offsetParams.srcStride
                         + (tid.x - offsetParams.validX1) * n;
        for (uint c = 0; c < n && c < 4; ++c)
            pixel[c] = offsetSrc[src + c];
    }
    if ((offsetParams.flags & kOffsetFlagMatte) != 0)
    {
        const uint column = 3 * tid.x;
        const uint row = offsetParams.rowWeights + 3 * tid.y;
        const Weight w = weightProduct(weightAt(column), weightAt(row));
        // Written rather than multiplied, so that a NaN or infinite pixel becomes 0 as on the CPU.
        if (offsetWeights[column + 2] != 0.0 || offsetWeights[row + 2] != 0.0)
        {
            for (uint c = 0; c < n && c < 4; ++c)
                pixel[c] = 0u;
        }
        else if (!(w.hi > 1.0 || (w.hi == 1.0 && w.lo >= 0.0)))
        {
            for (uint c = 0; c < n && c < 4; ++c)
                pixel[c] = asuint(weigh(asfloat(pixel[c]), w));
        }
    }
    for (uint c = 0; c < n && c < 4; ++c)
        offsetDst[dst + c] = pixel[c];
}
