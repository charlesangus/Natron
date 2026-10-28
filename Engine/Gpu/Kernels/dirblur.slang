import pixelwrap;
import resamplecore;

// DirBlur::renderOp per pixel: the sum over the samples, in the CPU's order, of each sample's
// factors times the source resampled through its matrix, as Resampler::resampleRow() does for one
// affine matrix. The wrap's push block carries the Source, which is both the image sampled and the
// image blended under the mask and mix. The parameter block in wrapAux holds the filter, the flags
// and the sample count, then per sample the matrix's rows (h00 h01 h02) and (h10 h11 h12) as
// (hi, lo) float pairs and the four factors in channel-bit order; DirBlur::renderOpGpu() writes it.

static const uint kDirBlurSourceValid = 1;
static const uint kDirBlurHeaderWords = 4;
static const uint kDirBlurSampleWords = 16;

FF dirblurFF(uint index)
{
    return ffMake(asfloat(wrapAux[index]), asfloat(wrapAux[index + 1]));
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
    src.blackOutside = true;
    return src;
}

// Resampler's samplePixel for an affine matrix: z is 1, the Jacobian is the matrix's linear part,
// zeroed along both axes when the sample falls outside the source.
float4 dirblurSample(ResampleSource src, uint filter, uint base, int2 p)
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

    float4 r;
    if (filter == kFilterImpulse)
    {
        interpolate(src, filter, false, fx, fy, r);
        return r;
    }
    // x1 <= fx + 0.5 && fx - 0.5 < x2, and likewise for y.
    const bool xinside = toPos(ffAddF(fx, 0.5)).i >= src.x1 && toPos(ffAddF(fx, -0.5)).i < src.x2;
    const bool yinside = toPos(ffAddF(fy, 0.5)).i >= src.y1 && toPos(ffAddF(fy, -0.5)).i < src.y2;
    const bool inside = xinside && yinside;
    const FF zero = ffMake(0.0, 0.0);
    const FF Jxx = inside ? h00 : zero;
    const FF Jxy = inside ? h01 : zero;
    const FF Jyx = inside ? h10 : zero;
    const FF Jyy = inside ? h11 : zero;
    return interpolateSuper(src, filter, false, fx, fy, Jxx, Jxy, Jyx, Jyy, jacobianStates(Jxx, Jxy, Jyx, Jyy), 0);
}

struct DirBlurOp : IPixelOp
{
    uint filter;
    uint flags;
    uint count;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        filter = params[0];
        flags = params[1];
        count = params[2];
    }

    float4 apply(float4 v, int2 xy)
    {
        float4 sum = float4(0.0, 0.0, 0.0, 0.0);
        const ResampleSource src = dirblurSource(flags);
        if (!src.valid)
            return sum;
        for (uint i = 0; i < count; ++i)
        {
            const uint base = kDirBlurHeaderWords + i * kDirBlurSampleWords;
            float4 s = dirblurSample(src, filter, base, xy);
            // The sample in channel-bit order, a one-channel pixel being alpha.
            if (wrap.nComps == 1)
                s = float4(0.0, 0.0, 0.0, s.x);
            const float4 factor = float4(asfloat(wrapAux[base + 12]), asfloat(wrapAux[base + 13]), asfloat(wrapAux[base + 14]),
                                         asfloat(wrapAux[base + 15]));
            sum += factor * s;
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
