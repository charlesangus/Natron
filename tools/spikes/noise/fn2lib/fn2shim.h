// C interface to a FastNoise2 build. The library is built twice, strict and relaxed floating
// point, as two shared objects that export only these functions, so the two copies of FastNoise2
// and FastSIMD never see each other's symbols.

#pragma once

extern "C" {

#define FN2SPIKE_DECLARE(mode)                                                                              \
    /* type: 0 Perlin, 1 SuperSimplex, 2 CellularDistance (Euclidean, Index0). octaves <= 1 renders */      \
    /* the source alone, otherwise FractalFBm over it. maxSimd: max, scalar, sse2, sse41, avx2, */          \
    /* avx512. Returns null when that SIMD level is not compiled in or the CPU lacks it. */                 \
    void* fn2spike_##mode##_create(int type, float featureScale, int octaves, float lacunarity, float gain, \
                                   const char* maxSimd);                                                    \
    void fn2spike_##mode##_destroy(void* handle);                                                           \
    /* GenPositionArray4D over count positions, offsets zero. */                                            \
    void fn2spike_##mode##_row(void* handle, float* out, int count, const float* xs, const float* ys,       \
                               const float* zs, const float* ws, int seed);                                 \
    const char* fn2spike_##mode##_simd(void* handle);

FN2SPIKE_DECLARE(strict)
FN2SPIKE_DECLARE(relaxed)

#undef FN2SPIKE_DECLARE
}
