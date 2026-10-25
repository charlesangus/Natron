/* SPDX-FileCopyrightText: 2009-2010 Sony Pictures Imageworks Inc., et al. All Rights Reserved.
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: BSD-3-Clause AND Apache-2.0
 *
 * Slang mirror of noise4d.h (Cycles perlin_4d/snoise_4d, BSD-3-Clause, adapted from Open Shading
 * Language; hash_uint4 from Cycles util/hash.h, Apache-2.0) and voronoi4d.h (Cycles 4D voronoi_f1
 * and hash_pcg4d_i, Apache-2.0), at blender/blender 052275913f65d6df3598588101345c621fd35ac8.
 * Compiled with -fp-mode precise so no multiply-add is contracted, matching the CPU build. */

RWStructuredBuffer<float> dstBuf;

struct SpikeParams
{
    uint width;
    uint height;
    float originX;
    float originY;
    float scale;
    float z;
    float w;
    int seed;
    float randomness;
};
[[vk::push_constant]] ConstantBuffer<SpikeParams> params;

uint rot(uint x, int k)
{
    return (x << k) | (x >> (32 - k));
}

uint hash_uint4(uint kx, uint ky, uint kz, uint kw)
{
    uint a, b, c;
    a = b = c = 0xdeadbeefu + (4u << 2) + 13u;

    a += kx;
    b += ky;
    c += kz;

    a -= c; a ^= rot(c, 4); c += b;
    b -= a; b ^= rot(a, 6); a += c;
    c -= b; c ^= rot(b, 8); b += a;
    a -= c; a ^= rot(c, 16); c += b;
    b -= a; b ^= rot(a, 19); a += c;
    c -= b; c ^= rot(b, 4); b += a;

    a += kw;

    c ^= b; c -= rot(b, 14);
    a ^= c; a -= rot(c, 11);
    b ^= a; b -= rot(a, 25);
    c ^= b; c -= rot(b, 16);
    a ^= c; a -= rot(c, 4);
    b ^= a; b -= rot(a, 14);
    c ^= b; c -= rot(b, 24);

    return c;
}

float fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float negate_if(float val, int condition)
{
    return condition != 0 ? -val : val;
}

float grad4(int hash, float x, float y, float z, float w)
{
    int h = hash & 31;
    float u = h < 24 ? x : y;
    float v = h < 16 ? y : z;
    float s = h < 8 ? z : w;
    return negate_if(u, h & 1) + negate_if(v, h & 2) + negate_if(s, h & 4);
}

float mixf(float a, float b, float t)
{
    return a + t * (b - a);
}

float tri_mix(float v0, float v1, float v2, float v3, float v4, float v5, float v6, float v7, float x, float y, float z)
{
    float x1 = 1.0f - x;
    float y1 = 1.0f - y;
    float z1 = 1.0f - z;
    return z1 * (y1 * (v0 * x1 + v1 * x) + y * (v2 * x1 + v3 * x)) + z * (y1 * (v4 * x1 + v5 * x) + y * (v6 * x1 + v7 * x));
}

float quad_mix(float v0, float v1, float v2, float v3, float v4, float v5, float v6, float v7,
               float v8, float v9, float v10, float v11, float v12, float v13, float v14, float v15,
               float x, float y, float z, float w)
{
    return mixf(tri_mix(v0, v1, v2, v3, v4, v5, v6, v7, x, y, z), tri_mix(v8, v9, v10, v11, v12, v13, v14, v15, x, y, z), w);
}

float floorfrac(float x, out int i)
{
    float f = floor(x);
    i = int(f);
    return x - f;
}

int hashi(int x, int y, int z, int w)
{
    return int(hash_uint4(uint(x), uint(y), uint(z), uint(w)));
}

float perlin_4d(float x, float y, float z, float w)
{
    int X, Y, Z, W;
    float fx = floorfrac(x, X);
    float fy = floorfrac(y, Y);
    float fz = floorfrac(z, Z);
    float fw = floorfrac(w, W);

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

float noise_scale4(float result)
{
    return 0.8344f * result;
}

float precision_wrap(float p)
{
    float correction = abs(p) >= 1000000.0f ? 0.5f : 0.0f;
    return fmod(p, 100000.0f) + correction;
}

float snoise_4d(float x, float y, float z, float w)
{
    return noise_scale4(perlin_4d(precision_wrap(x), precision_wrap(y), precision_wrap(z), precision_wrap(w)));
}

int4 hash_pcg4d_i(int4 v)
{
    uint x = uint(v.x) * 1664525u + 1013904223u;
    uint y = uint(v.y) * 1664525u + 1013904223u;
    uint z = uint(v.z) * 1664525u + 1013904223u;
    uint w = uint(v.w) * 1664525u + 1013904223u;
    x += y * w;
    y += z * x;
    z += x * y;
    w += y * z;
    x ^= uint(int(x) >> 16);
    y ^= uint(int(y) >> 16);
    z ^= uint(int(z) >> 16);
    w ^= uint(int(w) >> 16);
    x += y * w;
    y += z * x;
    z += x * y;
    w += y * z;
    return int4(int(x & 0x7FFFFFFFu), int(y & 0x7FFFFFFFu), int(z & 0x7FFFFFFFu), int(w & 0x7FFFFFFFu));
}

float hash_component_to_float(int h)
{
    return float(h) * (1.0f / float(0x7FFFFFFFu));
}

float voronoi_f1_4d(float x, float y, float z, float w, float randomness)
{
    float cfx = floor(x);
    float cfy = floor(y);
    float cfz = floor(z);
    float cfw = floor(w);
    float lx = x - cfx;
    float ly = y - cfy;
    float lz = z - cfz;
    float lw = w - cfw;
    int cx = int(cfx);
    int cy = int(cfy);
    int cz = int(cfz);
    int cw = int(cfw);

    float minDistance = 3.402823466e+38f;
    for (int u = -1; u <= 1; u++)
    {
        for (int k = -1; k <= 1; k++)
        {
            for (int j = -1; j <= 1; j++)
            {
                for (int i = -1; i <= 1; i++)
                {
                    int4 h = hash_pcg4d_i(int4(cx + i, cy + j, cz + k, cw + u));
                    float dx = (float(i) + hash_component_to_float(h.x) * randomness) - lx;
                    float dy = (float(j) + hash_component_to_float(h.y) * randomness) - ly;
                    float dz = (float(k) + hash_component_to_float(h.z) * randomness) - lz;
                    float dw = (float(u) + hash_component_to_float(h.w) * randomness) - lw;
                    float d = dx * dx + dy * dy + dz * dz + dw * dw;
                    if (d < minDistance)
                        minDistance = d;
                }
            }
        }
    }
    return sqrt(minDistance);
}

float2 noisePosition(uint2 tid)
{
    return float2((params.originX + (float(tid.x) + 0.5f)) * params.scale,
                  (params.originY + (float(tid.y) + 0.5f)) * params.scale);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void perlin_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    float2 p = noisePosition(tid.xy);
    dstBuf[tid.y * params.width + tid.x] = snoise_4d(p.x, p.y, params.z, params.w);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void voronoi_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    float2 p = noisePosition(tid.xy);
    dstBuf[tid.y * params.width + tid.x] = voronoi_f1_4d(p.x, p.y, params.z, params.w, params.randomness);
}
