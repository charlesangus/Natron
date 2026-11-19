// One-dimensional FFTs of `lines` lines of n samples, one workgroup per line, written transposed: sample k of line l
// lands at complex index k * lines + l, so running the kernel again over the result transforms the other axis and
// restores the layout. A Stockham autosort FFT in shared memory: radix-4 passes, then one radix-2 pass when log2 n is
// odd.
StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;
// exp(-2 pi i j / tableSize) for j in [0, tableSize), interleaved: sines and cosines from the host in double, since
// the device's own may be accurate to only 2^-11.
StructuredBuffer<float> twiddles;

struct FftParams
{
    uint n;
    uint log2n;
    uint lines;
    uint inverse;
    uint realInput;
    uint tableSize;
    float scale;
};
[[vk::push_constant]] ConstantBuffer<FftParams> params;

// kGroup and kMaxN are mirrored in Engine/Gpu/GpuFft.h.
static const uint kGroup = 256;
static const uint kMaxN = 4096;
static const uint kRadix4PerThread = kMaxN / 4 / kGroup;
static const uint kRadix2PerThread = kMaxN / 2 / kGroup;

groupshared float2 samples[kMaxN];

float2 cmul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// exp(sign 2 pi i num / den) for num < den, den a power of two dividing tableSize.
float2 twiddle(uint num, uint den, float sign)
{
    const uint j = num * (params.tableSize / den);
    return float2(twiddles[2 * j], sign * -twiddles[2 * j + 1]);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void fftLines(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const uint n = params.n;
    const uint l = gid.x;
    if (l >= params.lines)
        return;
    const uint t = tid.x;

    for (uint i = t; i < n; i += kGroup)
    {
        const uint s = l * n + i;
        samples[i] = params.realInput != 0 ? float2(srcBuf[s], 0.0) : float2(srcBuf[2 * s], srcBuf[2 * s + 1]);
    }
    GroupMemoryBarrierWithGroupSync();

    const float sign = params.inverse != 0 ? 1.0 : -1.0;
    uint p = 1;
    const uint quarter = n / 4;
    for (uint stage = 0; stage < params.log2n / 2; ++stage)
    {
        float2 v[kRadix4PerThread * 4];
        [ForceUnroll]
        for (uint b = 0; b < kRadix4PerThread; ++b)
        {
            const uint i = t + b * kGroup;
            if (i < quarter)
            {
                const uint k = i & (p - 1);
                const float2 u0 = samples[i];
                const float2 u1 = cmul(samples[i + quarter], twiddle(k, 4 * p, sign));
                const float2 u2 = cmul(samples[i + 2 * quarter], twiddle(2 * k, 4 * p, sign));
                const float2 u3 = cmul(samples[i + 3 * quarter], twiddle(3 * k, 4 * p, sign));
                const float2 a = u0 + u2;
                const float2 c = u1 + u3;
                const float2 bd = u0 - u2;
                const float2 e = u1 - u3;
                // (u1 - u3) times sign * i.
                const float2 d = sign * float2(-e.y, e.x);
                v[4 * b] = a + c;
                v[4 * b + 1] = bd + d;
                v[4 * b + 2] = a - c;
                v[4 * b + 3] = bd - d;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        [ForceUnroll]
        for (uint b = 0; b < kRadix4PerThread; ++b)
        {
            const uint i = t + b * kGroup;
            if (i < quarter)
            {
                const uint k = i & (p - 1);
                const uint j = (i - k) * 4 + k;
                samples[j] = v[4 * b];
                samples[j + p] = v[4 * b + 1];
                samples[j + 2 * p] = v[4 * b + 2];
                samples[j + 3 * p] = v[4 * b + 3];
            }
        }
        GroupMemoryBarrierWithGroupSync();
        p *= 4;
    }

    if ((params.log2n & 1) != 0)
    {
        const uint halfN = n / 2;
        float2 v[kRadix2PerThread * 2];
        [ForceUnroll]
        for (uint b = 0; b < kRadix2PerThread; ++b)
        {
            const uint i = t + b * kGroup;
            if (i < halfN)
            {
                const uint k = i & (p - 1);
                const float2 u0 = samples[i];
                const float2 u1 = cmul(samples[i + halfN], twiddle(k, 2 * p, sign));
                v[2 * b] = u0 + u1;
                v[2 * b + 1] = u0 - u1;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        [ForceUnroll]
        for (uint b = 0; b < kRadix2PerThread; ++b)
        {
            const uint i = t + b * kGroup;
            if (i < halfN)
            {
                const uint k = i & (p - 1);
                const uint j = (i - k) * 2 + k;
                samples[j] = v[2 * b];
                samples[j + p] = v[2 * b + 1];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    for (uint i = t; i < n; i += kGroup)
    {
        const uint o = i * params.lines + l;
        const float2 value = samples[i] * params.scale;
        dstBuf[2 * o] = value.x;
        dstBuf[2 * o + 1] = value.y;
    }
}
