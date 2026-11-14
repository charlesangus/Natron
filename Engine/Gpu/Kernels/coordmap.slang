import resamplecore;
import edgesample;

// Resampler::resampleRowFromCoords for one pixel: sampling at a per-pixel source coordinate, wrapped
// per axis, with resamplecore.slang's interpolateSuper, on the Jacobian when the caller has one.

// Resampler::WrapMode.
static const uint kWrapNone = 0;
static const uint kWrapClamp = 1;
static const uint kWrapRepeat = 2;
static const uint kWrapMirror = 3;

// Resampler's kCoordLimit: wrapped coordinates are clamped to +-2^30, so the int conversions of the
// samplers stay defined.
static const float kCoordLimit = 1073741824.0;

bool isFiniteF(float v)
{
    return !isnan(v) && !isinf(v);
}

// d - k * period for k = floor(d / period), d >= 0 apart from rounding.
FF reduceToPeriod(FF d, float period)
{
    const FF q = ffDivF(d, period);
    float k = floor(q.hi);
    if (q.hi == k && q.lo < 0.0)
        k -= 1.0;
    return ffAdd(d, ffNeg(ffMulF(ffMake(k, 0.0), period)));
}

// One coordinate wrapped into [lo, hi) as Resampler's wrapCoord does: clamp to the centres of the
// edge pixels, [lo + 0.5, hi - 0.5], setting `moved` when that changed it; repeat
// s - w floor((s - lo) / w); mirror r = (s - lo) - 2w floor((s - lo) / 2w), then lo + r for r <= w
// and lo + 2w - r otherwise. An empty domain wraps nothing. False when the result is not finite.
bool wrapCoord(float s, int lo, int hi, uint mode, out FF result, out bool moved)
{
    moved = false;
    result = ffMake(s, 0.0);
    const float w = float(hi - lo);
    if (hi > lo)
    {
        switch (mode)
        {
        case kWrapClamp:
            if (s < float(lo) + 0.5)
            {
                result = ffMake(float(lo) + 0.5, 0.0);
                moved = true;
            }
            else if (s > float(hi) - 0.5)
            {
                result = ffMake(float(hi) - 0.5, 0.0);
                moved = true;
            }
            break;
        case kWrapRepeat:
            if (isFiniteF(s))
                result = ffAddF(reduceToPeriod(ffAddF(result, -float(lo)), w), float(lo));
            break;
        case kWrapMirror:
            if (isFiniteF(s))
            {
                const FF r = reduceToPeriod(ffAddF(result, -float(lo)), 2.0 * w);
                if (ffLess(ffMake(w, 0.0), r))
                    result = ffAddF(ffNeg(r), float(lo) + 2.0 * w);
                else
                    result = ffAddF(r, float(lo));
            }
            break;
        default:
            break;
        }
    }
    if (!isFiniteF(result.hi) || !isFiniteF(result.lo))
        return false;
    if (result.hi > kCoordLimit)
        result = ffMake(kCoordLimit, 0.0);
    else if (result.hi < -kCoordLimit)
        result = ffMake(-kCoordLimit, 0.0);
    return true;
}

// A parameter block's Resampler::FilterSpec: the filter's word, and lobes, B and C from word `lobesWord`.
FilterSpec loadFilterSpec(StructuredBuffer<uint> params, uint filterWord, uint lobesWord)
{
    FilterSpec s;
    s.filter = params[filterWord];
    s.lobes = int(params[lobesWord]);
    s.B = asfloat(params[lobesWord + 1]);
    s.C = asfloat(params[lobesWord + 2]);
    return s;
}

float finiteOrZero(float v)
{
    return isFiniteF(v) ? v : 0.0;
}

// The sample of `src` at source coordinate (sx, sy), wrapped in `domain` (x1, y1, x2, y2; the source's bounds
// when empty). A coordinate that is not finite after wrapping is outside and reads zero, whatever
// black-outside says. `J` is (Jxx, Jxy, Jyx, Jyy), the coordinate's derivatives along the
// destination's x and y; the impulse filter and hasJacobian false sample with the filter alone.
// Otherwise non-finite entries read 0, the row of an axis that clamping moved is zeroed, and, as
// resample.slang's samplePixel does, so is the row of an axis with no source pixel within half a
// pixel, both rows under black-outside when either axis is out.
float4 sampleAt(ResampleSource src, float sx, float sy, float4 J, bool hasJacobian, FilterSpec spec, bool clampOn, uint wrapU, uint wrapV,
                int4 domain)
{
    const float4 zero = float4(0.0, 0.0, 0.0, 0.0);
    if (!src.valid)
        return zero;
    if (domain.z <= domain.x || domain.w <= domain.y)
        domain = int4(src.x1, src.y1, src.x2, src.y2);
    FF fx;
    FF fy;
    bool movedX;
    bool movedY;
    if (!wrapCoord(sx, domain.x, domain.z, wrapU, fx, movedX) || !wrapCoord(sy, domain.y, domain.w, wrapV, fy, movedY))
        return zero;

    const bool useJacobian = hasJacobian && spec.filter != kFilterImpulse;
    float Jxx = useJacobian ? finiteOrZero(J.x) : 0.0;
    float Jxy = useJacobian ? finiteOrZero(J.y) : 0.0;
    float Jyx = useJacobian ? finiteOrZero(J.z) : 0.0;
    float Jyy = useJacobian ? finiteOrZero(J.w) : 0.0;
    bool xinside = toPos(ffAddF(fx, 0.5)).i >= src.x1 && toPos(ffAddF(fx, -0.5)).i < src.x2;
    bool yinside = toPos(ffAddF(fy, 0.5)).i >= src.y1 && toPos(ffAddF(fy, -0.5)).i < src.y2;
    if (src.edgeMode == kEdgeBlack && !(xinside && yinside))
    {
        xinside = false;
        yinside = false;
    }
    if (!xinside || movedX)
    {
        Jxx = 0.0;
        Jxy = 0.0;
    }
    if (!yinside || movedY)
    {
        Jyx = 0.0;
        Jyy = 0.0;
    }
    return interpolateSuper(src, spec, clampOn, fx, fy, ffMake(Jxx, 0.0), ffMake(Jxy, 0.0), ffMake(Jyx, 0.0), ffMake(Jyy, 0.0));
}
