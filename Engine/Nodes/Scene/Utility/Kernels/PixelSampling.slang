module PixelSampling;

// KernelPixelFilter in UtilityKernels.h.
public static const uint kFilterBox = 0;
public static const uint kFilterTriangle = 1;
public static const uint kFilterGaussian = 2;
public static const uint kFilterBlackmanHarris = 3;

public static const float kSamplingPi = 3.14159265358979;

// Integer only, so the host and the device draw the same samples.
public uint pcgHash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

public float unitFloat(uint bits)
{
    return float(bits >> 8u) * (1.0 / 16777216.0);
}

public float filterWeight(float t, uint filter)
{
    const float x = 2.0 * t - 1.0;
    switch (filter) {
    case kFilterTriangle:
        return max(0.0, 1.0 - abs(x));
    case kFilterGaussian:
        return exp(-4.5 * x * x);
    case kFilterBlackmanHarris:
        return max(0.0, 0.35875 - 0.48829 * cos(2.0 * kSamplingPi * t) + 0.14128 * cos(4.0 * kSamplingPi * t) - 0.01168 * cos(6.0 * kSamplingPi * t));
    default:
        return 1.0;
    }
}

// Kensler's hashed permutation of [0, length), different for each pattern.
public uint permute(uint index, uint length, uint pattern)
{
    uint mask = length - 1u;
    mask |= mask >> 1u;
    mask |= mask >> 2u;
    mask |= mask >> 4u;
    mask |= mask >> 8u;
    mask |= mask >> 16u;
    uint i = index;
    do {
        i ^= pattern;
        i *= 0xe170893du;
        i ^= pattern >> 16u;
        i ^= (i & mask) >> 4u;
        i ^= pattern >> 8u;
        i *= 0x0929eb3fu;
        i ^= pattern >> 23u;
        i ^= (i & mask) >> 1u;
        i *= 1u | pattern >> 27u;
        i *= 0x6935fa69u;
        i ^= (i & mask) >> 11u;
        i *= 0x74dcb303u;
        i ^= (i & mask) >> 2u;
        i *= 0x9e501cc3u;
        i ^= (i & mask) >> 2u;
        i *= 0xc860a3dfu;
        i &= mask;
        i ^= i >> 5u;
    } while (i >= length);
    return (i + pattern) % length;
}

// Correlated multi-jitter: one sample per cell of the samplesPerSide grid, and one per column and row
// of the grid's samplesPerSide squared subdivisions, so that an edge along either axis is covered to
// within one sample. The subdivisions are shuffled: unshuffled, a sample's place within its cell
// follows the cell's row, which ties x to y and biases any filter that weights them both. Each pixel
// visits the cells in its own order, so a partial pass shows noise rather than a pattern.
public float2 samplePosition(int pixelX, int pixelY, uint samplesPerSide, uint sampleIndex, uint seed)
{
    const uint n = max(samplesPerSide, 1u);
    if (n == 1u) {
        return float2(0.5, 0.5);
    }
    const uint pixelHash = pcgHash(uint(pixelX) ^ pcgHash(uint(pixelY) ^ pcgHash(seed)));
    const uint cell = (sampleIndex + pixelHash) % (n * n);
    const uint i = cell % n;
    const uint j = cell / n;
    const uint jitterX = pcgHash(pixelHash ^ (sampleIndex * 2654435769u));
    const uint jitterY = pcgHash(jitterX);
    const uint subX = permute(j, n, pixelHash * 0xa511e9b3u);
    const uint subY = permute(i, n, pixelHash * 0x63d83595u);
    return float2((float(i) + (float(subX) + unitFloat(jitterX)) / float(n)) / float(n),
                  (float(j) + (float(subY) + unitFloat(jitterY)) / float(n)) / float(n));
}

// A sample's offset from its pixel's centre, in pixels, and its filter weight.
public float2 sampleOffset(float2 t, uint samplesPerSide, float filterWidth)
{
    const float support = max(samplesPerSide, 1u) == 1u ? 1.0 : filterWidth;
    return float2((t.x - 0.5) * support, (t.y - 0.5) * support);
}

public float sampleWeight(float2 t, uint samplesPerSide, uint filter)
{
    return max(samplesPerSide, 1u) == 1u ? 1.0 : filterWeight(t.x, filter) * filterWeight(t.y, filter);
}
