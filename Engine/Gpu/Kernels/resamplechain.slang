import resample;
import resamplecore;
import coordmap;
import lensmodels;
import warpchain;

// The resample of a source through the op's own inverse matrix and an inverse warp chain with a
// map entry, as runResampleCpu() resamples a chained plane: the pixel centre through the matrix,
// then through the chain, sampled as Resampler::resampleRowFromCoords() samples, with no wrap. The
// source, destination and filter are resample.slang's push block; its blurBuf holds the matrix as
// kResampleChainMatrixWords words of a warpchain.slang matrix block, then the chain as
// InverseWarpChain::packGpu() writes it. Bit 0 of `states` asks for the Jacobian.

static const uint kResampleChainMatrixWords = 18;

[shader("compute")]
[numthreads(8, 8, 1)]
void resample_chain_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= rp.width || tid.y >= rp.height)
        return;
    FF x = ffMake(float(rp.outX + int(tid.x)) + 0.5, 0.0);
    FF y = ffMake(float(rp.outY + int(tid.y)) + 0.5, 0.0);
    float4 J = float4(1.0, 0.0, 0.0, 1.0);
    const bool jacobian = (rp.states & 1u) != 0;
    const bool mapped = warpChainMatrix(blurBuf, 0, x, y, J, jacobian) && warpChainMap(blurBuf, kResampleChainMatrixWords, x, y, J, jacobian);
    const float nan = asfloat(0x7fc00000u);
    const float sx = mapped ? ffToFloat(x) : nan;
    const float sy = mapped ? ffToFloat(y) : nan;
    storePixel(tid, sampleAt(paramsSource(), sx, sy, J, jacobian, paramsFilter(), (rp.flags & kFlagClamp) != 0, kWrapNone, kWrapNone,
                             int4(0, 0, 0, 0)));
}
