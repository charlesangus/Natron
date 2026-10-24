import resample;

// Resampler::resampleRowFromCoords for one pixel: sampling at a per-pixel source coordinate, wrapped
// per axis, with resample.slang's interpolate, or its interpolateSuper when the caller has a
// Jacobian. The source, the filter's clamp and black-outside flags are resample.slang's `rp`.

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

float finiteOrZero(float v)
{
    return isFiniteF(v) ? v : 0.0;
}

// The interpolateSuper state byte of Jacobian (Jxx Jxy; Jyx Jyy), as Resampler::supersampleLevels
// decides it: level n along an axis once the squared norm of its column exceeds 3^(2n - 1), up to 4.
uint jacobianState(float Jxx, float Jxy, float Jyx, float Jyy)
{
    if (Jxx == 0.0 && Jxy == 0.0 && Jyx == 0.0 && Jyy == 0.0)
        return 0x40u;
    const float dx = Jxx * Jxx + Jyx * Jyx;
    const float dy = Jxy * Jxy + Jyy * Jyy;
    uint levelX = 0;
    uint levelY = 0;
    float threshold = 3.0;
    [ForceUnroll]
    for (int n = 0; n < 4; ++n)
    {
        if (dx > threshold)
            ++levelX;
        if (dy > threshold)
            ++levelY;
        threshold *= 9.0;
    }
    return levelX | (levelY << 3);
}

// The sample at source coordinate (sx, sy), wrapped in `domain` (x1, y1, x2, y2; the source's bounds
// when empty). A coordinate that is not finite after wrapping is outside and reads zero, whatever
// black-outside says. `J` is (Jxx, Jxy, Jyx, Jyy), the coordinate's derivatives along the
// destination's x and y; the impulse filter and hasJacobian false sample with the filter alone.
// Otherwise non-finite entries read 0, the row of an axis that clamping moved is zeroed, and, as
// resample.slang's samplePixel does, so is the row of an axis with no source pixel within half a
// pixel, both rows under black-outside when either axis is out.
float4 sampleAt(float sx, float sy, float4 J, bool hasJacobian, uint filter, bool clampOn, uint wrapU, uint wrapV,
                int4 domain)
{
    const float4 zero = float4(0.0, 0.0, 0.0, 0.0);
    if (!srcValid())
        return zero;
    if (domain.z <= domain.x || domain.w <= domain.y)
        domain = int4(rp.srcX1, rp.srcY1, rp.srcX2, rp.srcY2);
    FF fx;
    FF fy;
    bool movedX;
    bool movedY;
    if (!wrapCoord(sx, domain.x, domain.z, wrapU, fx, movedX) || !wrapCoord(sy, domain.y, domain.w, wrapV, fy, movedY))
        return zero;

    float4 r;
    if (filter == kFilterImpulse || !hasJacobian)
    {
        interpolate(filter, clampOn, fx, fy, r);
        return r;
    }
    float Jxx = finiteOrZero(J.x);
    float Jxy = finiteOrZero(J.y);
    float Jyx = finiteOrZero(J.z);
    float Jyy = finiteOrZero(J.w);
    bool xinside = toPos(ffAddF(fx, 0.5)).i >= rp.srcX1 && toPos(ffAddF(fx, -0.5)).i < rp.srcX2;
    bool yinside = toPos(ffAddF(fy, 0.5)).i >= rp.srcY1 && toPos(ffAddF(fy, -0.5)).i < rp.srcY2;
    if (blackOutside() && !(xinside && yinside))
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
    return interpolateSuper(filter, clampOn, fx, fy, ffMake(Jxx, 0.0), ffMake(Jxy, 0.0), ffMake(Jyx, 0.0),
                            ffMake(Jyy, 0.0), jacobianState(Jxx, Jxy, Jyx, Jyy), 0);
}

///////////////////////////////////////////////////////////////////////////////
// The test entry: samples a buffer of coordinates into width x height destination pixels, read and
// written row by row. Each pixel's coordinate is kCoordWords floats of coordBuf: sx, sy, then Jxx,
// Jxy, Jyx, Jyy. `rp` is resample.slang's, but for its `states`, which holds wrapU in bits 0-1, wrapV
// in bits 2-3 and, in bit 4, whether the Jacobians apply. blurBuf's first four words are the wrap
// domain, x1 y1 x2 y2 as ints.

static const uint kCoordWords = 6;

[[vk::binding(3, 0)]] StructuredBuffer<float> coordBuf;

[shader("compute")]
[numthreads(8, 8, 1)]
void coordmap_test_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= rp.width || tid.y >= rp.height)
        return;
    const uint base = (tid.y * rp.width + tid.x) * kCoordWords;
    const float4 J = float4(coordBuf[base + 2], coordBuf[base + 3], coordBuf[base + 4], coordBuf[base + 5]);
    const int4 domain = int4(int(blurBuf[0]), int(blurBuf[1]), int(blurBuf[2]), int(blurBuf[3]));
    const uint wrapU = rp.states & 3u;
    const uint wrapV = (rp.states >> 2) & 3u;
    const bool hasJacobian = (rp.states & 16u) != 0;
    const bool clampOn = (rp.flags & kFlagClamp) != 0;
    storePixel(tid, sampleAt(coordBuf[base], coordBuf[base + 1], J, hasJacobian, rp.filter, clampOn, wrapU, wrapV,
                             domain));
}
