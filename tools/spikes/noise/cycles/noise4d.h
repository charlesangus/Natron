/* SPDX-FileCopyrightText: 2009-2010 Sony Pictures Imageworks Inc., et al. All Rights Reserved.
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Adapted code from Open Shading Language.
 *
 * hash_uint4 is from Cycles util/hash.h:
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 * SPDX-License-Identifier: Apache-2.0
 * Source: http://burtleburtle.net/bob/c/lookup3.c
 *
 * Scalar port of Cycles' non-SSE perlin_4d and snoise_4d (intern/cycles/kernel/svm/noise.h at
 * blender/blender 052275913f65d6df3598588101345c621fd35ac8), written in the subset that
 * noise4d.slang mirrors line for line. */

#pragma once

#include <cmath>
#include <cstdint>

namespace noisespike::cycles {

inline uint32_t
rot(uint32_t x, int k)
{
    return (x << k) | (x >> (32 - k));
}

inline uint32_t
hash_uint4(uint32_t kx, uint32_t ky, uint32_t kz, uint32_t kw)
{
    uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + (4u << 2) + 13u;

    a += kx;
    b += ky;
    c += kz;

    a -= c;
    a ^= rot(c, 4);
    c += b;
    b -= a;
    b ^= rot(a, 6);
    a += c;
    c -= b;
    c ^= rot(b, 8);
    b += a;
    a -= c;
    a ^= rot(c, 16);
    c += b;
    b -= a;
    b ^= rot(a, 19);
    a += c;
    c -= b;
    c ^= rot(b, 4);
    b += a;

    a += kw;

    c ^= b;
    c -= rot(b, 14);
    a ^= c;
    a -= rot(c, 11);
    b ^= a;
    b -= rot(a, 25);
    c ^= b;
    c -= rot(b, 16);
    a ^= c;
    a -= rot(c, 4);
    b ^= a;
    b -= rot(a, 14);
    c ^= b;
    c -= rot(b, 24);

    return c;
}

inline float
fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float
negate_if(float val, int condition)
{
    return condition ? -val : val;
}

inline float
grad4(int hash, float x, float y, float z, float w)
{
    int h = hash & 31;
    float u = h < 24 ? x : y;
    float v = h < 16 ? y : z;
    float s = h < 8 ? z : w;
    return negate_if(u, h & 1) + negate_if(v, h & 2) + negate_if(s, h & 4);
}

inline float
mixf(float a, float b, float t)
{
    return a + t * (b - a);
}

inline float
tri_mix(float v0, float v1, float v2, float v3, float v4, float v5, float v6, float v7, float x, float y, float z)
{
    float x1 = 1.0f - x;
    float y1 = 1.0f - y;
    float z1 = 1.0f - z;
    return z1 * (y1 * (v0 * x1 + v1 * x) + y * (v2 * x1 + v3 * x)) + z * (y1 * (v4 * x1 + v5 * x) + y * (v6 * x1 + v7 * x));
}

inline float
quad_mix(float v0, float v1, float v2, float v3, float v4, float v5, float v6, float v7,
         float v8, float v9, float v10, float v11, float v12, float v13, float v14, float v15,
         float x, float y, float z, float w)
{
    return mixf(tri_mix(v0, v1, v2, v3, v4, v5, v6, v7, x, y, z), tri_mix(v8, v9, v10, v11, v12, v13, v14, v15, x, y, z), w);
}

inline float
floorfrac(float x, int* i)
{
    float f = std::floor(x);
    *i = (int)f;
    return x - f;
}

inline int
hashi(int x, int y, int z, int w)
{
    return (int)hash_uint4((uint32_t)x, (uint32_t)y, (uint32_t)z, (uint32_t)w);
}

inline float
perlin_4d(float x, float y, float z, float w)
{
    int X, Y, Z, W;
    float fx = floorfrac(x, &X);
    float fy = floorfrac(y, &Y);
    float fz = floorfrac(z, &Z);
    float fw = floorfrac(w, &W);

    float u = fade(fx);
    float v = fade(fy);
    float t = fade(fz);
    float s = fade(fw);

    return quad_mix(grad4(hashi(X, Y, Z, W), fx, fy, fz, fw),
                    grad4(hashi(X + 1, Y, Z, W), fx - 1.0f, fy, fz, fw),
                    grad4(hashi(X, Y + 1, Z, W), fx, fy - 1.0f, fz, fw),
                    grad4(hashi(X + 1, Y + 1, Z, W), fx - 1.0f, fy - 1.0f, fz, fw),
                    grad4(hashi(X, Y, Z + 1, W), fx, fy, fz - 1.0f, fw),
                    grad4(hashi(X + 1, Y, Z + 1, W), fx - 1.0f, fy, fz - 1.0f, fw),
                    grad4(hashi(X, Y + 1, Z + 1, W), fx, fy - 1.0f, fz - 1.0f, fw),
                    grad4(hashi(X + 1, Y + 1, Z + 1, W), fx - 1.0f, fy - 1.0f, fz - 1.0f, fw),
                    grad4(hashi(X, Y, Z, W + 1), fx, fy, fz, fw - 1.0f),
                    grad4(hashi(X + 1, Y, Z, W + 1), fx - 1.0f, fy, fz, fw - 1.0f),
                    grad4(hashi(X, Y + 1, Z, W + 1), fx, fy - 1.0f, fz, fw - 1.0f),
                    grad4(hashi(X + 1, Y + 1, Z, W + 1), fx - 1.0f, fy - 1.0f, fz, fw - 1.0f),
                    grad4(hashi(X, Y, Z + 1, W + 1), fx, fy, fz - 1.0f, fw - 1.0f),
                    grad4(hashi(X + 1, Y, Z + 1, W + 1), fx - 1.0f, fy, fz - 1.0f, fw - 1.0f),
                    grad4(hashi(X, Y + 1, Z + 1, W + 1), fx, fy - 1.0f, fz - 1.0f, fw - 1.0f),
                    grad4(hashi(X + 1, Y + 1, Z + 1, W + 1), fx - 1.0f, fy - 1.0f, fz - 1.0f, fw - 1.0f),
                    u, v, t, s);
}

inline float
noise_scale4(float result)
{
    return 0.8344f * result;
}

// Cycles wraps every axis at 100000 to keep the fraction precise; the wrap is a visible seam.
// Slang's fmod lowers to OpFRem, which drivers need not compute exactly, so the CPU and GPU can
// disagree past the wrap; the bench's large-origin rows show by how much.
inline float
precision_wrap(float p)
{
    float correction = std::fabs(p) >= 1000000.0f ? 0.5f : 0.0f;
    return std::fmod(p, 100000.0f) + correction;
}

inline float
snoise_4d(float x, float y, float z, float w)
{
    return noise_scale4(perlin_4d(precision_wrap(x), precision_wrap(y), precision_wrap(z), precision_wrap(w)));
}

} // namespace noisespike::cycles
