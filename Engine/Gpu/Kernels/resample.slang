// Resampler::resampleRow for affine and projective inverse matrices over resamplecore.slang's
// filters, reading the source from a storage buffer. resample_main samples one matrix;
// resample_blur_main averages several per pixel as Resampler::motionBlurPixel does.
// GpuResampleParams in Engine/Gpu/GpuResample.h mirrors ResampleParams.

import resamplecore;

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;
// resample_blur_main's matrices, laid out as Engine/Gpu/GpuResample.cpp writes them: a header of
// kBlurHeaderWords words, then kBlurMatrixWords words per matrix.
StructuredBuffer<uint> blurBuf;

// The destination's offset addresses the output origin. The source's addresses (srcX1, srcY1).
// The matrix rows (h00 h01 h02) and (h10 h11 h12) are each a double split into hi + lo. `states`
// holds one byte per Jacobian state (bit 0: x inside, bit 1: y inside): levelX in bits 0-2,
// levelY in bits 3-5, bit 6 set when that Jacobian is zero. resample_blur_main reads the matrices
// and their states from blurBuf instead, and so does resample_main under kFlagMatrixInBuffer, which
// a projective matrix needs: its third row does not fit the push constants.
struct ResampleParams
{
    int outX;
    int outY;
    uint width;
    uint height;
    uint dstOffset;
    uint dstStride;
    uint srcOffset;
    uint srcStride;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint nComps;
    uint filter;
    uint flags;
    uint states;
    float h00Hi;
    float h01Hi;
    float h02Hi;
    float h10Hi;
    float h11Hi;
    float h12Hi;
    float h00Lo;
    float h01Lo;
    float h02Lo;
    float h10Lo;
    float h11Lo;
    float h12Lo;
};
[[vk::push_constant]] ConstantBuffer<ResampleParams> rp;

// The source described by `rp` and srcBuf.
ResampleSource paramsSource()
{
    ResampleSource s;
    s.buf = srcBuf;
    s.offset = rp.srcOffset;
    s.stride = rp.srcStride;
    s.x1 = rp.srcX1;
    s.y1 = rp.srcY1;
    s.x2 = rp.srcX2;
    s.y2 = rp.srcY2;
    s.nComps = rp.nComps;
    s.valid = (rp.flags & kFlagSrcValid) != 0;
    s.blackOutside = (rp.flags & kFlagBlackOutside) != 0;
    return s;
}

///////////////////////////////////////////////////////////////////////////////

// The matrix rows (h00 h01 h02) and (h10 h11 h12), with `states` as in ResampleParams. A projective
// matrix also has the row (h20 h21 h22), and its Jacobian and states are worked out per pixel.
struct Matrix
{
    FF h00;
    FF h01;
    FF h02;
    FF h10;
    FF h11;
    FF h12;
    uint states;
    bool projective;
    FF h20;
    FF h21;
    FF h22;
};

// One entry of the Jacobian of (X / Z, Y / Z), as Resampler's samplePixel computes it: (h Z - P g) / Z^2,
// with h the matrix entry of that row and column, P the row's X or Y, and g the third row's entry in
// that column.
FF projectiveJacobian(FF hrc, FF P, FF h2c, FF Z, FF Z2)
{
    return ffDiv(ffAdd(ffMul(hrc, Z), ffNeg(ffMul(P, h2c))), Z2);
}

float4 samplePixel(Matrix m, int x, int y)
{
    const ResampleSource src = paramsSource();
    if (!src.valid)
        return float4(0.0, 0.0, 0.0, 0.0);
    const uint filter = rp.filter;
    const bool clampOn = (rp.flags & kFlagClamp) != 0;
    const float px = float(x) + 0.5;
    const float py = float(y) + 0.5;
    const FF X = ffAdd(ffAdd(ffMulF(m.h00, px), ffMulF(m.h01, py)), m.h02);
    const FF Y = ffAdd(ffAdd(ffMulF(m.h10, px), ffMulF(m.h11, py)), m.h12);
    const FF zero = ffMake(0.0, 0.0);
    FF Z = ffMake(1.0, 0.0);
    FF fx = X;
    FF fy = Y;
    if (m.projective)
    {
        Z = ffAdd(ffAdd(ffMulF(m.h20, px), ffMulF(m.h21, py)), m.h22);
        // Behind the camera, or at infinity.
        if (!ffLess(zero, Z))
            return float4(0.0, 0.0, 0.0, 0.0);
        fx = ffDiv(X, Z);
        fy = ffDiv(Y, Z);
    }

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
    FF Jxx = xinside ? m.h00 : zero;
    FF Jxy = xinside ? m.h01 : zero;
    FF Jyx = yinside ? m.h10 : zero;
    FF Jyy = yinside ? m.h11 : zero;
    uint states = m.states;
    uint state = (xinside ? 1u : 0u) | (yinside ? 2u : 0u);
    if (m.projective)
    {
        const FF Z2 = ffMul(Z, Z);
        Jxx = xinside ? projectiveJacobian(m.h00, X, m.h20, Z, Z2) : zero;
        Jxy = xinside ? projectiveJacobian(m.h01, X, m.h21, Z, Z2) : zero;
        Jyx = yinside ? projectiveJacobian(m.h10, Y, m.h20, Z, Z2) : zero;
        Jyy = yinside ? projectiveJacobian(m.h11, Y, m.h21, Z, Z2) : zero;
        states = jacobianStates(Jxx, Jxy, Jyx, Jyy);
        state = 0;
    }
    return interpolateSuper(src, filter, clampOn, fx, fy, Jxx, Jxy, Jyx, Jyy, states, state);
}

void storePixel(uint3 tid, float4 v)
{
    const uint base = rp.dstOffset + tid.y * rp.dstStride + tid.x * rp.nComps;
    for (uint c = 0; c < rp.nComps && c < 4; ++c)
        dstBuf[base + c] = v[c];
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resample_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= rp.width || tid.y >= rp.height)
        return;
    Matrix m;
    if ((rp.flags & kFlagMatrixInBuffer) != 0)
    {
        m = blurMatrix(0);
    }
    else
    {
        m.h00 = ffMake(rp.h00Hi, rp.h00Lo);
        m.h01 = ffMake(rp.h01Hi, rp.h01Lo);
        m.h02 = ffMake(rp.h02Hi, rp.h02Lo);
        m.h10 = ffMake(rp.h10Hi, rp.h10Lo);
        m.h11 = ffMake(rp.h11Hi, rp.h11Lo);
        m.h12 = ffMake(rp.h12Hi, rp.h12Lo);
        m.states = rp.states;
        m.projective = false;
        m.h20 = ffMake(0.0, 0.0);
        m.h21 = ffMake(0.0, 0.0);
        m.h22 = ffMake(1.0, 0.0);
    }
    storePixel(tid, samplePixel(m, rp.outX + int(tid.x), rp.outY + int(tid.y)));
}

///////////////////////////////////////////////////////////////////////////////
// Resampler's motionBlurPixelT. The CPU accumulates in doubles and stops on the variance, so the
// sums here are kept in two floats: a sample more or less moves the mean by far more than the
// tolerance.

// The header: matrix count, minimum samples, maximum samples, the seed's offset
// (unsigned(0x10000 * motionBlur)), the squared maximum error as hi and lo float bits, and 1 when the
// matrices are weighted. A matrix: hi of h00 h01 h02 h10 h11 h12, then their lo, its states, its
// weight as hi and lo float bits, 1 when it is projective, then hi of h20 h21 h22 and their lo.
static const uint kBlurHeaderWords = 8;
static const uint kBlurMatrixWords = 22;

uint motionBlurHash(uint a)
{
    a = (a ^ 61u) ^ (a >> 16);
    a = a + (a << 3);
    a = a ^ (a >> 4);
    a = a * 0x27d4eb2du;
    a = a ^ (a >> 15);
    return a;
}

// floor(vanDerCorput2(seed) * count) exactly: the sequence's element is reversebits(seed) / 2^32,
// and count is below 2^16.
uint vanDerCorputIndex(uint rev, uint count)
{
    return ((rev >> 16) * count + (((rev & 0xffffu) * count) >> 16)) >> 16;
}

Matrix blurMatrix(uint t)
{
    const uint base = kBlurHeaderWords + t * kBlurMatrixWords;
    Matrix m;
    m.h00 = ffMake(asfloat(blurBuf[base + 0]), asfloat(blurBuf[base + 6]));
    m.h01 = ffMake(asfloat(blurBuf[base + 1]), asfloat(blurBuf[base + 7]));
    m.h02 = ffMake(asfloat(blurBuf[base + 2]), asfloat(blurBuf[base + 8]));
    m.h10 = ffMake(asfloat(blurBuf[base + 3]), asfloat(blurBuf[base + 9]));
    m.h11 = ffMake(asfloat(blurBuf[base + 4]), asfloat(blurBuf[base + 10]));
    m.h12 = ffMake(asfloat(blurBuf[base + 5]), asfloat(blurBuf[base + 11]));
    m.states = blurBuf[base + 12];
    m.projective = blurBuf[base + 15] != 0;
    m.h20 = ffMake(asfloat(blurBuf[base + 16]), asfloat(blurBuf[base + 19]));
    m.h21 = ffMake(asfloat(blurBuf[base + 17]), asfloat(blurBuf[base + 20]));
    m.h22 = ffMake(asfloat(blurBuf[base + 18]), asfloat(blurBuf[base + 21]));
    return m;
}

FF blurWeight(uint t)
{
    const uint base = kBlurHeaderWords + t * kBlurMatrixWords;
    return ffMake(asfloat(blurBuf[base + 13]), asfloat(blurBuf[base + 14]));
}

// The samples the variance asks for, as the CPU's (int)(var / maxErr2); 0 asks for none more. On
// x86 the CPU's conversion of a quotient at or past 2^31, or of a NaN, gives INT_MIN, which asks for
// none more either.
int samplesForVariance(FF var, FF maxErr2)
{
    const FF q = ffDiv(var, maxErr2);
    if (!(q.hi >= 0.0) || !(q.hi < 2147483648.0))
        return 0;
    return max(toPos(q).i, 0);
}

float4 motionBlurPixel(int x, int y)
{
    const uint count = blurBuf[0];
    const int minSamples = int(blurBuf[1]);
    const int maxIt = int(blurBuf[2]);
    const FF maxErr2 = ffMake(asfloat(blurBuf[4]), asfloat(blurBuf[5]));
    const bool weighted = blurBuf[6] != 0;
    const uint nComps = min(rp.nComps, 4u);
    const FF zero = ffMake(0.0, 0.0);

    FF acc = zero;
    FF accPix[4] = { zero, zero, zero, zero };
    FF accPix2[4] = { zero, zero, zero, zero };
    FF mean[4] = { zero, zero, zero, zero };

    uint seed = motionBlurHash(motionBlurHash(uint(x) + blurBuf[3]) + uint(y));
    int sample = 0;
    int maxSamples = minSamples;
    while (sample < maxSamples)
    {
        for (; sample < maxSamples; ++sample, ++seed)
        {
            const uint index = vanDerCorputIndex(reversebits(seed), count);
            // (sample + vanDerCorput2(seed)) * count / minSamples, floored: the fraction left over
            // from the index cannot carry the integer quotient past the next integer.
            const uint t = (sample < minSamples) ? (uint(sample) * count + index) / uint(minSamples) : index;
            const float4 v = samplePixel(blurMatrix(t), x, y);
            if (!weighted)
            {
                for (uint c = 0; c < nComps; ++c)
                {
                    precise float sq = v[c] * v[c];
                    accPix[c] = ffAddF(accPix[c], v[c]);
                    accPix2[c] = ffAddF(accPix2[c], sq);
                }
            }
            else
            {
                const FF w = blurWeight(t);
                acc = ffAdd(acc, w);
                for (uint c = 0; c < nComps; ++c)
                {
                    precise float sq = v[c] * v[c];
                    accPix[c] = ffAdd(accPix[c], ffMulF(w, v[c]));
                    accPix2[c] = ffAdd(accPix2[c], ffMulF(w, sq));
                }
            }
        }
        if (!weighted)
        {
            for (uint c = 0; c < nComps; ++c)
            {
                mean[c] = ffDivF(accPix[c], float(sample));
                if (sample > 1)
                {
                    const FF spread = ffAdd(accPix2[c], ffNeg(ffMulF(ffMul(mean[c], mean[c]), float(sample))));
                    const FF var = ffDivF(spread, float(sample - 1));
                    if (maxSamples < maxIt)
                        maxSamples = max(maxSamples, min(samplesForVariance(var, maxErr2), maxIt));
                }
            }
        }
        else if (ffLess(zero, acc))
        {
            for (uint c = 0; c < nComps; ++c)
            {
                mean[c] = ffDiv(accPix[c], acc);
                if (sample > 1)
                {
                    const FF var = ffAdd(ffDiv(accPix2[c], acc), ffNeg(ffMul(mean[c], mean[c])));
                    if (maxSamples < maxIt)
                        maxSamples = max(maxSamples, min(samplesForVariance(var, maxErr2), maxIt));
                }
            }
        }
    }
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    for (uint c = 0; c < nComps; ++c)
        r[c] = mean[c].hi + mean[c].lo;
    return r;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resample_blur_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= rp.width || tid.y >= rp.height)
        return;
    // Every sample of a source that is not there is zero, and so is their mean.
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    if ((rp.flags & kFlagSrcValid) != 0)
        v = motionBlurPixel(rp.outX + int(tid.x), rp.outY + int(tid.y));
    storePixel(tid, v);
}