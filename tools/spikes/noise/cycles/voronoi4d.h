/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Scalar port of Cycles' 4D voronoi_f1 with the Euclidean metric (intern/cycles/kernel/svm/voronoi.h)
 * and the PCG 4D hash it uses (intern/cycles/util/hash.h, from "Hash Functions for GPU Rendering",
 * JCGT 2020, https://jcgt.org/published/0009/03/02/), at blender/blender
 * 052275913f65d6df3598588101345c621fd35ac8. Only the distance output is kept. */

#pragma once

#include <cfloat>
#include <cmath>
#include <cstdint>

namespace noisespike::cycles {

struct Int4 {
    int x, y, z, w;
};

// Signed overflow is undefined in C++, so the products wrap in uint32_t; >> stays arithmetic, as
// it is on int in Cycles and in Slang.
inline Int4
hash_pcg4d_i(Int4 v)
{
    uint32_t x = (uint32_t)v.x * 1664525u + 1013904223u;
    uint32_t y = (uint32_t)v.y * 1664525u + 1013904223u;
    uint32_t z = (uint32_t)v.z * 1664525u + 1013904223u;
    uint32_t w = (uint32_t)v.w * 1664525u + 1013904223u;
    x += y * w;
    y += z * x;
    z += x * y;
    w += y * z;
    x ^= (uint32_t)((int32_t)x >> 16);
    y ^= (uint32_t)((int32_t)y >> 16);
    z ^= (uint32_t)((int32_t)z >> 16);
    w ^= (uint32_t)((int32_t)w >> 16);
    x += y * w;
    y += z * x;
    z += x * y;
    w += y * z;
    return { (int)(x & 0x7FFFFFFFu), (int)(y & 0x7FFFFFFFu), (int)(z & 0x7FFFFFFFu), (int)(w & 0x7FFFFFFFu) };
}

inline float
hash_component_to_float(int h)
{
    return (float)h * (1.0f / (float)0x7FFFFFFFu);
}

inline float
voronoi_f1_4d(float x, float y, float z, float w, float randomness)
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
                    Int4 h = hash_pcg4d_i({ cx + i, cy + j, cz + k, cw + u });
                    float dx = ((float)i + hash_component_to_float(h.x) * randomness) - lx;
                    float dy = ((float)j + hash_component_to_float(h.y) * randomness) - ly;
                    float dz = ((float)k + hash_component_to_float(h.z) * randomness) - lz;
                    float dw = ((float)u + hash_component_to_float(h.w) * randomness) - lw;
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

} // namespace noisespike::cycles
