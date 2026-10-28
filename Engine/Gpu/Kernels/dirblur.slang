import pixelwrap;
import resamplecore;

// DirBlur::renderOp per pixel: the sum, or with Max the maximum, over the samples in the CPU's
// order of each sample's factors times the source resampled through its matrix, as
// Resampler::resampleRow() does for one affine matrix, each scaled by one minus the Holdout channel
// resampled through the same matrix. The wrap's push block carries the Source, which is both the
// image sampled and the image blended under the mask and mix; the Holdout view's pixels are in
// holdoutBuf, read as black outside its own image. The parameter block in wrapAux holds the
// filter, the flags (the clamp and black outside flags as the filter knobs), the sample count and the
// Holdout view (offset, stride, x1, y1, x2, y2, components, component read), then per sample the
// matrix's rows (h00 h01 h02) and (h10 h11 h12) as (hi, lo) float pairs, the four factors in
// channel-bit order and the weight; DirBlur::renderOpGpu() writes it.

[[vk::binding(4, 0)]] StructuredBuffer<float> holdoutBuf;

static const uint kDirBlurSourceValid = 1;
static const uint kDirBlurMax = 2;
static const uint kDirBlurHoldout = 4;
static const uint kDirBlurHoldoutValid = 8;
static const uint kDirBlurClamp = 16;
static const uint kDirBlurBlackOutside = 32;
static const uint kDirBlurHeaderWords = 12;
static const uint kDirBlurSampleWords = 20;

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
    src.blackOutside = (flags & kDirBlurBlackOutside) != 0;
    return src;
}

ResampleSource dirblurHoldout(uint flags)
{
    ResampleSource src;
    src.buf = holdoutBuf;
    src.offset = wrapAux[3];
    src.stride = wrapAux[4];
    src.x1 = int(wrapAux[5]);
    src.y1 = int(wrapAux[6]);
    src.x2 = int(wrapAux[7]);
    src.y2 = int(wrapAux[8]);
    src.nComps = wrapAux[9];
    src.valid = (flags & kDirBlurHoldoutValid) != 0 && src.x2 > src.x1 && src.y2 > src.y1;
    src.blackOutside = true;
    return src;
}

// Resampler's samplePixel for an affine matrix: z is 1, the Jacobian is the matrix's linear part,
// zeroed along each axis the sample falls outside the source on, and along both with blackOutside.
float4 dirblurSample(ResampleSource src, uint filter, bool clampOn, uint base, int2 p)
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
        interpolate(src, filter, clampOn, fx, fy, r);
        return r;
    }
    // x1 <= fx + 0.5 && fx - 0.5 < x2, and likewise for y.
    bool xinside = toPos(ffAddF(fx, 0.5)).i >= src.x1 && toPos(ffAddF(fx, -0.5)).i < src.x2;
    bool yinside = toPos(ffAddF(fy, 0.5)).i >= src.y1 && toPos(ffAddF(fy, -0.5)).i < src.y2;
    if (src.blackOutside && !(xinside && yinside))
    {
        xinside = false;
        yinside = false;
    }
    const FF zero = ffMake(0.0, 0.0);
    const FF Jxx = xinside ? h00 : zero;
    const FF Jxy = xinside ? h01 : zero;
    const FF Jyx = yinside ? h10 : zero;
    const FF Jyy = yinside ? h11 : zero;
    return interpolateSuper(src, filter, clampOn, fx, fy, Jxx, Jxy, Jyx, Jyy, jacobianStates(Jxx, Jxy, Jyx, Jyy), 0);
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
        const bool takeMax = (flags & kDirBlurMax) != 0;
        const bool holdoutOn = (flags & kDirBlurHoldout) != 0;
        const bool clampOn = (flags & kDirBlurClamp) != 0;
        const ResampleSource holdout = dirblurHoldout(flags);
        const uint component = min(wrapAux[10], 3u);
        float weightSum = 0.0;
        for (uint i = 0; i < count; ++i)
        {
            const uint base = kDirBlurHeaderWords + i * kDirBlurSampleWords;
            float keep = 1.0;
            if (holdoutOn)
            {
                float h = 0.0;
                if (holdout.valid)
                    h = dirblurSample(holdout, filter, clampOn, base, xy)[component];
                keep = 1.0 - min(1.0, max(0.0, h));
            }
            if (takeMax && keep <= 0.0)
                continue;
            float4 s = dirblurSample(src, filter, clampOn, base, xy);
            // The sample in channel-bit order, a one-channel pixel being alpha.
            if (wrap.nComps == 1)
                s = float4(0.0, 0.0, 0.0, s.x);
            const float4 factor = float4(asfloat(wrapAux[base + 12]), asfloat(wrapAux[base + 13]), asfloat(wrapAux[base + 14]),
                                         asfloat(wrapAux[base + 15]));
            const float4 term = keep * factor * s;
            if (takeMax)
            {
                sum = max(sum, term);
            }
            else
            {
                sum += term;
                weightSum += keep * asfloat(wrapAux[base + 16]);
            }
        }
        // Held-out copies leave the average: the rest are renormalised to their weights.
        if (holdoutOn && !takeMax)
            sum = (weightSum > 0.0) ? sum / weightSum : float4(0.0, 0.0, 0.0, 0.0);
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
