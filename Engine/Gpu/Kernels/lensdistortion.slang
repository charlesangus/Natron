import pixelwrap;
import coordmap;
import resamplecore;
import lensmodels;

// LensDistortion::renderOp per pixel. The wrap's push block carries the Source, which is both the
// image sampled and the image blended under the mask and mix, the destination and the mask; the
// parameter block in wrapAux carries the settings and, from word kLensDistortionModelBase, the
// packed lens model. LensDistortionGpuParams in Engine/Nodes/Transform/LensDistortion.cpp mirrors
// the block's words.

static const uint kLensDistortionClamp = 1;
static const uint kLensDistortionBlackOutside = 2;
static const uint kLensDistortionJacobian = 4;
// Image output with a Source to sample and a valid model.
static const uint kLensDistortionSample = 8;
// STMap output with a valid model and a Source format.
static const uint kLensDistortionSTMap = 16;

static const uint kLensDistortionModelBase = 20;

struct LensDistortion
{
    uint filter;
    uint flags;
    // The direction each output pixel is mapped in to reach the Source: the opposite of the knob's.
    uint direction;
    FF scaleX;
    FF scaleY;
    // Lens pixels to Source pixels along x.
    FF toSourceX;
    // The Source format in Source pixels at scale 1.
    FF sourceX1;
    FF sourceY1;
    FF sourceWidth;
    FF sourceHeight;
}

FF lensDistortionFF(StructuredBuffer<uint> params, uint index)
{
    return ffMake(asfloat(params[index]), asfloat(params[index + 1]));
}

LensDistortion lensDistortionLoad(StructuredBuffer<uint> params)
{
    LensDistortion d;
    d.filter = params[0];
    d.flags = params[1];
    d.direction = params[2];
    d.scaleX = lensDistortionFF(params, 4);
    d.scaleY = lensDistortionFF(params, 6);
    d.toSourceX = lensDistortionFF(params, 8);
    d.sourceX1 = lensDistortionFF(params, 10);
    d.sourceY1 = lensDistortionFF(params, 12);
    d.sourceWidth = lensDistortionFF(params, 14);
    d.sourceHeight = lensDistortionFF(params, 16);
    return d;
}

ResampleSource lensDistortionSource(LensDistortion d)
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
    src.valid = wrap.srcX2 > wrap.srcX1 && wrap.srcY2 > wrap.srcY1;
    src.blackOutside = (d.flags & kLensDistortionBlackOutside) != 0;
    return src;
}

// The output in channel-bit order: (u, v, 0, valid) in STMap mode, whose alpha is a one-channel
// pixel's, or the Source sampled where the model sends the pixel centre.
float4 lensDistortionPixel(LensDistortion d, int2 p)
{
    const float4 zero = float4(0.0, 0.0, 0.0, 0.0);
    const bool stmap = (d.flags & kLensDistortionSTMap) != 0;
    if (!stmap && (d.flags & kLensDistortionSample) == 0)
        return zero;
    // The pixel centre in lens pixels at scale 1.
    const FF x = ffDiv(lensF(float(p.x) + 0.5), d.scaleX);
    const FF y = ffDiv(lensF(float(p.y) + 0.5), d.scaleY);
    const LensSample s = lensMap(lensModelAt(wrapAux, kLensDistortionModelBase), d.direction, x, y);
    const bool ok = s.ok && ffFinite(s.x) && ffFinite(s.y);
    if (stmap)
    {
        if (!ok)
            return zero;
        const FF u = ffDiv(ffSub(ffMul(s.x, d.toSourceX), d.sourceX1), d.sourceWidth);
        const FF v = ffDiv(ffSub(s.y, d.sourceY1), d.sourceHeight);
        const FF zeroFF = ffMake(0.0, 0.0);
        const FF oneFF = ffMake(1.0, 0.0);
        const bool inside = !ffLess(u, zeroFF) && ffLess(u, oneFF) && !ffLess(v, zeroFF) && ffLess(v, oneFF);
        return float4(ffToFloat(u), ffToFloat(v), 0.0, inside ? 1.0 : 0.0);
    }
    // Source pixel coordinates at the render scale, NaN where the model has no solution, which the
    // sampler reads as black.
    float sx = asfloat(0x7fc00000u);
    float sy = sx;
    float4 J = zero;
    const bool jacobian = (d.flags & kLensDistortionJacobian) != 0;
    if (ok)
    {
        sx = ffToFloat(ffMul(ffMul(s.x, d.toSourceX), d.scaleX));
        sy = ffToFloat(ffMul(s.y, d.scaleY));
        if (jacobian)
        {
            // d(source at scale) / d(output at scale), from the model's full-resolution one.
            J = float4(ffToFloat(ffMul(s.J.j0, d.toSourceX)), ffToFloat(ffDiv(ffMul(ffMul(s.J.j1, d.toSourceX), d.scaleX), d.scaleY)),
                       ffToFloat(ffDiv(ffMul(s.J.j2, d.scaleY), d.scaleX)), ffToFloat(s.J.j3));
        }
    }
    const float4 v = sampleAt(lensDistortionSource(d), sx, sy, J, jacobian, d.filter, (d.flags & kLensDistortionClamp) != 0, kWrapNone, kWrapNone,
                              int4(0, 0, 0, 0));
    if (wrap.nComps == 1)
        return float4(0.0, 0.0, 0.0, v.x);
    return v;
}

struct LensDistortionOp : IPixelOp
{
    LensDistortion d;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        d = lensDistortionLoad(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        return lensDistortionPixel(d, xy);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void lensdistortion_main(uint3 tid : SV_DispatchThreadID)
{
    LensDistortionOp op;
    runWrapped(op, tid);
}
