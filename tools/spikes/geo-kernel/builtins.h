// Builtins both backends share. The interpreter calls these inline functions directly; the JIT
// gets nk_noise3 as bitcode compiled from builtins_bc.cpp, linked into each snippet module so it
// inlines, and calls nk_nearpoint in the host process like any heavy native query.

#pragma once

#include <cstdint>

#include "Noise4D.h"

struct NearGrid {
    float origin[3];
    float invCell;
    std::int32_t dim;
    const std::int32_t* cells;
};

extern "C" std::int32_t nk_nearpoint(const NearGrid* grid, float x, float y, float z);
extern "C" float nk_noise3(float x, float y, float z);

inline float
gkNoise3(float x, float y, float z)
{
    const float n = noise4d::perlin4(1337u, noise4d::coordFromFloats(x, y, z, 0.f));

    return 0.5f + 0.5f * n;
}

inline std::int32_t
gkNearPoint(const NearGrid* g, float x, float y, float z)
{
    // A grid-cell lookup standing in for a tree query: a few flops and one random gather. NaN
    // and out-of-range coordinates clamp before the float-to-int conversion.
    const float hi = float(g->dim - 1);
    float c[3] = { (x - g->origin[0]) * g->invCell, (y - g->origin[1]) * g->invCell, (z - g->origin[2]) * g->invCell };
    std::int32_t ic[3];
    for (int k = 0; k < 3; ++k) {
        float v = c[k] > 0.f ? c[k] : 0.f;
        v = v < hi ? v : hi;
        ic[k] = std::int32_t(v);
    }

    return g->cells[(ic[2] * g->dim + ic[1]) * g->dim + ic[0]];
}
