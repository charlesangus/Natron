import pixelwrap;
import resamplecore;
import edgesample;

// DirBlur::renderOp per pixel: the sum, or with Max the maximum, over the samples in the CPU's
// order of each sample's factors times the source resampled through its matrix, as
// Resampler::resampleRow() does for one affine matrix. The wrap's push block carries the Source,
// which is both the image sampled and the image blended under the mask and mix. The parameter
// block in wrapAux holds the filter, the flags (the clamp and black outside flags as the filter
// knobs), the sample count, the filter's lobes, B and C, then per sample the matrix's rows
// (h00 h01 h02) and (h10 h11 h12) as (hi, lo) float pairs, the four factors in channel-bit order;
// DirBlur::renderOpGpu() writes it. With the identity-nearest flag, a sample whose matrix is
// exactly the identity reads its nearest pixel whatever the filter, as GodRays::renderOp() does.

static const uint kDirBlurSourceValid = 1;
static const uint kDirBlurMax = 2;
static const uint kDirBlurClamp = 16;
static const uint kDirBlurEdgeBlack = 32;
static const uint kDirBlurIdentityNearest = 64;
static const uint kDirBlurHeaderWords = 6;
static const uint kDirBlurSampleWords = 16;

FF dirblurFF(uint index)
{
    return ffMake(asfloat(wrapAux[index]), asfloat(wrapAux[index + 1]));
}

bool dirblurIsIdentity(uint base)
{
    for (uint e = 0; e < 6; ++e)
    {
        const float expected = (e == 0 || e == 4) ? 1.0 : 0.0;
        if (asfloat(wrapAux[base + 2 * e]) != expected || asfloat(wrapAux[base + 2 * e + 1]) != 0.0)
            return false;
    }
    return true;
}

ResampleSource dirblurSource(uint flags)
{
    ResampleSource src;
    src.buf = wrapSrc;
    src.offset = wrap.srcOffset;
    src.stride = wrap.srcStride;
    src.x1 = wrap.srcX1;
    src.y1 = wrap.srcY1;
    src.x2 = wrap.srcX2;
    src.y2 = wrap.srcY2;
    src.nComps = wrap.nComps;
    src.valid = (flags & kDirBlurSourceValid) != 0 && wrap.srcX2 > wrap.srcX1 && wrap.srcY2 > wrap.srcY1;
    src.edgeMode = edgeModeFromFlags(flags, kDirBlurEdgeBlack);
    return src;
}

// Resampler's samplePixel for an affine matrix: z is 1, the Jacobian is the matrix's linear part,
// zeroed along each axis the sample falls outside the source on, and along both under Black.
float4 dirblurSample(ResampleSource src, FilterSpec spec, bool clampOn, uint base, int2 p)
{
    const FF h00 = dirblurFF(base);
    const FF h01 = dirblurFF(base + 2);
    const FF h02 = dirblurFF(base + 4);
    const FF h10 = dirblurFF(base + 6);
    const FF h11 = dirblurFF(base + 8);
    const FF h12 = dirblurFF(base + 10);
    const float px = float(p.x) + 0.5;
    const float py = float(p.y) + 0.5;
    const FF fx = ffAdd(ffAdd(ffMulF(h00, px), ffMulF(h01, py)), h02);
    const FF fy = ffAdd(ffAdd(ffMulF(h10, px), ffMulF(h11, py)), h12);

    const FF zero = ffMake(0.0, 0.0);
    if (spec.filter == kFilterImpulse)
        return interpolateSuper(src, spec, clampOn, fx, fy, zero, zero, zero, zero);
    // x1 <= fx + 0.5 && fx - 0.5 < x2, and likewise for y.
    bool xinside = toPos(ffAddF(fx, 0.5)).i >= src.x1 && toPos(ffAddF(fx, -0.5)).i < src.x2;
    bool yinside = toPos(ffAddF(fy, 0.5)).i >= src.y1 && toPos(ffAddF(fy, -0.5)).i < src.y2;
    if (src.edgeMode == kEdgeBlack && !(xinside && yinside))
    {
        xinside = false;
        yinside = false;
    }
    const FF Jxx = xinside ? h00 : zero;
    const FF Jxy = xinside ? h01 : zero;
    const FF Jyx = yinside ? h10 : zero;
    const FF Jyy = yinside ? h11 : zero;
    return interpolateSuper(src, spec, clampOn, fx, fy, Jxx, Jxy, Jyx, Jyy);
}

struct DirBlurOp : IPixelOp
{
    FilterSpec spec;
    uint flags;
    uint count;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        spec.filter = params[0];
        spec.lobes = int(params[3]);
        spec.B = asfloat(params[4]);
        spec.C = asfloat(params[5]);
        flags = params[1];
        count = params[2];
    }

    float4 apply(float4 v, int2 xy)
    {
        float4 sum = float4(0.0, 0.0, 0.0, 0.0);
        const ResampleSource src = dirblurSource(flags);
        if (!src.valid)
            return sum;
        const bool takeMax = (flags & kDirBlurMax) != 0;
        const bool clampOn = (flags & kDirBlurClamp) != 0;
        const bool identityNearest = (flags & kDirBlurIdentityNearest) != 0;
        for (uint i = 0; i < count; ++i)
        {
            const uint base = kDirBlurHeaderWords + i * kDirBlurSampleWords;
            FilterSpec sampleSpec = spec;
            if (identityNearest && dirblurIsIdentity(base))
                sampleSpec.filter = kFilterImpulse;
            float4 s = dirblurSample(src, sampleSpec, clampOn, base, xy);
            // The sample in channel-bit order, a one-channel pixel being alpha.
            if (wrap.nComps == 1)
                s = float4(0.0, 0.0, 0.0, s.x);
            const float4 factor = float4(asfloat(wrapAux[base + 12]), asfloat(wrapAux[base + 13]), asfloat(wrapAux[base + 14]),
                                         asfloat(wrapAux[base + 15]));
            const float4 term = factor * s;
            sum = takeMax ? max(sum, term) : sum + term;
        }
        return sum;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void dirblur_main(uint3 tid : SV_DispatchThreadID)
{
    DirBlurOp op;
    runWrapped(op, tid);
}
