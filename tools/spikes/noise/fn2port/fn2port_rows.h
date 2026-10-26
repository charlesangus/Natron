// Row renderers for the plain C++ FastNoise2 port. fn2port_rows.cpp is compiled once per variant
// (instruction set baseline x fused/unfused multiply-add) and exports one Fn2PortVariant each.

#pragma once

#include <cstdint>

struct Fn2PortParams {
    int type; // 0 Perlin, 1 SuperSimplex, 2 Cellular F1
    float frequency;
    int octaves;
    float lacunarity;
    float gain;
    float weightedStrength;
    float gridJitter;
    uint32_t seed;
    float originX;
    float originY;
    float z;
    float w;
    int width;
};

struct Fn2PortVariant {
    const char* name;
    // True when the variant is compiled for x86-64-v3; the caller checks the CPU before calling it,
    // because nothing in that translation unit may run on an older CPU.
    bool needsV3;
    // Structure-of-arrays row loop, the one that is meant to vectorise.
    void (*row)(const Fn2PortParams& p, int y, float* out);
    // One fbm() call per pixel, the line-for-line reference the row loop must match bit for bit.
    void (*rowScalar)(const Fn2PortParams& p, int y, float* out);
};

extern const Fn2PortVariant fn2port_base; // x86-64 baseline, unfused
extern const Fn2PortVariant fn2port_base_fma; // x86-64 baseline, std::fma (a libm call without FMA hardware)
extern const Fn2PortVariant fn2port_v3; // x86-64-v3 (AVX2 + FMA available), unfused
extern const Fn2PortVariant fn2port_v3_fma; // x86-64-v3, fused
