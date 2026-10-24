// A whole-pixel translation of the source, as runResampleCpu's offset form reads it: every
// channel of each output pixel is the source pixel it shifts from, or zero outside the source.
// Pixels travel as their bits, so the copy is exact whatever they hold. GpuOffsetParams in
// Engine/Gpu/GpuResample.h mirrors OffsetParams.

// The destination's offset addresses the output origin. Threads inside [validX1, validX2) x
// [validY1, validY2) read the source, whose offset addresses the pixel thread (validX1, validY1)
// reads; the others write zero.
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
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> offsetSrc;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> offsetDst;
[[vk::push_constant]] ConstantBuffer<OffsetParams> offsetParams;

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
    if (!inside)
    {
        for (uint c = 0; c < n && c < 4; ++c)
            offsetDst[dst + c] = 0u;
        return;
    }
    const uint src = offsetParams.srcOffset + (tid.y - offsetParams.validY1) * offsetParams.srcStride
                     + (tid.x - offsetParams.validX1) * n;
    for (uint c = 0; c < n && c < 4; ++c)
        offsetDst[dst + c] = offsetSrc[src + c];
}
