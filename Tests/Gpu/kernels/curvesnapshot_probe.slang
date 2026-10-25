import "../../../Engine/Gpu/Kernels/pixelwrap";
import "../../../Engine/Gpu/Kernels/curvesnapshot";

// Operand pairs (a, b) are read from wrapSrc, one per thread at a four-float stride, and four
// floats per pair are written to wrapDst: hi, lo, the product error, and the product. The pair
// count is a multiple of the group size; the bindings are the ones pixelwrap claims.
[shader("compute")]
[numthreads(64, 1, 1)]
void probe_mul(uint3 tid : SV_DispatchThreadID)
{
    const float a = wrapSrc[4 * tid.x];
    const float b = wrapSrc[4 * tid.x + 1];
    const CurveFF m = curveMul(curveFF(a, 0.0), curveFF(b, 0.0));
    precise float p = a * b;
    wrapDst[4 * tid.x] = m.hi;
    wrapDst[4 * tid.x + 1] = m.lo;
    wrapDst[4 * tid.x + 2] = curveProductError(a, b, p);
    wrapDst[4 * tid.x + 3] = p;
}

// The fused form curveMul replaced, kept to show a device where it loses the error.
[shader("compute")]
[numthreads(64, 1, 1)]
void probe_fused(uint3 tid : SV_DispatchThreadID)
{
    const float a = wrapSrc[4 * tid.x];
    const float b = wrapSrc[4 * tid.x + 1];
    precise float p = a * b;
    wrapDst[4 * tid.x] = p;
    wrapDst[4 * tid.x + 1] = fma(a, b, -p);
    wrapDst[4 * tid.x + 2] = 0.0;
    wrapDst[4 * tid.x + 3] = p;
}
