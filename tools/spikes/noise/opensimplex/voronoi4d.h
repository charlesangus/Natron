// 4D Voronoi F1 (Euclidean) over a 3x3x3x3 cell scan, with one feature point per cell placed by an
// integer hash. lowbias32 is Chris Wellons' (hash-prospector, public domain). The same hash and the
// same expression order are in os2s4d.slang, so the CPU and GPU agree bit for bit up to sqrt.

#pragma once

#include <cfloat>
#include <cmath>
#include <cstdint>

namespace noisespike::opensimplex {

inline uint32_t
lowbias32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

inline uint32_t
cell_hash(int x, int y, int z, int w, uint32_t seed)
{
    uint32_t h = lowbias32(seed ^ (uint32_t)w);
    h = lowbias32(h ^ (uint32_t)z);
    h = lowbias32(h ^ (uint32_t)y);
    return lowbias32(h ^ (uint32_t)x);
}

// The top 24 bits, so the conversion is exact and the result lies in [0, 1).
inline float
hash_to_unit(uint32_t h)
{
    return (float)(h >> 8) * (1.0f / 16777216.0f);
}

inline float
jitter(uint32_t cellHash, uint32_t axis)
{
    return hash_to_unit(lowbias32(cellHash + axis * 0x9E3779B9u));
}

inline float
voronoi_f1_4d(int seed, float x, float y, float z, float w, float randomness)
{
    float cfx = std::floor(x);
    float cfy = std::floor(y);
    float cfz = std::floor(z);
    float cfw = std::floor(w);
    float lx = x - cfx;
    float ly = y - cfy;
    float lz = z - cfz;
    float lw = w - cfw;
    int cx = (int)cfx;
    int cy = (int)cfy;
    int cz = (int)cfz;
    int cw = (int)cfw;

    float minDistance = FLT_MAX;
    for (int u = -1; u <= 1; u++) {
        for (int k = -1; k <= 1; k++) {
            for (int j = -1; j <= 1; j++) {
                for (int i = -1; i <= 1; i++) {
                    uint32_t h = cell_hash(cx + i, cy + j, cz + k, cw + u, (uint32_t)seed);
                    float dx = ((float)i + jitter(h, 0u) * randomness) - lx;
                    float dy = ((float)j + jitter(h, 1u) * randomness) - ly;
                    float dz = ((float)k + jitter(h, 2u) * randomness) - lz;
                    float dw = ((float)u + jitter(h, 3u) * randomness) - lw;
                    float d = dx * dx + dy * dy + dz * dz + dw * dw;
                    if (d < minDistance) {
                        minDistance = d;
                    }
                }
            }
        }
    }
    return std::sqrt(minDistance);
}

} // namespace noisespike::opensimplex
