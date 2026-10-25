// 4D OpenSimplex2S ("SuperSimplex") noise, ported from K.jpg's csharp/OpenSimplex2S.cs (CC0,
// KdotJPG/OpenSimplex2 4cd120d35bfc27096698de90d1bcbf4f9d359a3b) in the subset os2s4d.slang mirrors
// line for line.
//
// Unlike upstream, the skew and the lattice offsets are computed in float rather than double, so
// the CPU and the GPU evaluate the same expressions. The 64-bit gradient hash is upstream's; the
// GPU emulates it in 32-bit halves because the device does not enable shaderInt64.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace noisespike::opensimplex {

inline constexpr float kGrad4[] = {
#include "os2s4d_grad.inc"
};

// Per 4x4x4x4 sub-cell: the range [lo 16 bits, hi 16 bits) of kLookup4DB to visit.
inline constexpr uint32_t kLookup4DA[] = {
#include "os2s4d_lookup_a.inc"
};

// Lattice vertex codes: two bits per axis, x in the low bits, each the offset plus one.
inline constexpr uint32_t kLookup4DB[] = {
#include "os2s4d_lookup_b.inc"
};

inline constexpr size_t kLookupAOffset = std::size(kGrad4);
inline constexpr size_t kLookupBOffset = kLookupAOffset + std::size(kLookup4DA);
static_assert(kLookupAOffset == 640 && kLookupBOffset == 896, "os2s4d.slang hardcodes these offsets");

// The GPU's `tables` buffer: kGrad4 as float bits, kLookup4DA, kLookup4DB.
inline std::vector<uint32_t>
packedTables()
{
    std::vector<uint32_t> out(kLookupBOffset + std::size(kLookup4DB));
    std::memcpy(out.data(), kGrad4, sizeof(kGrad4));
    std::memcpy(out.data() + kLookupAOffset, kLookup4DA, sizeof(kLookup4DA));
    std::memcpy(out.data() + kLookupBOffset, kLookup4DB, sizeof(kLookup4DB));
    return out;
}

inline constexpr uint64_t kPrimeX = 0x5205402B9270C86Full;
inline constexpr uint64_t kPrimeY = 0x598CD327003817B5ull;
inline constexpr uint64_t kPrimeZ = 0x5BCC226E9FA0BACBull;
inline constexpr uint64_t kPrimeW = 0x56CC5227E58F554Bull;
inline constexpr uint64_t kHashMultiplier = 0x53A3F72DEEC546F5ull;

inline constexpr float kSkew4D = 0.309016994374947f;
inline constexpr float kUnskew4D = -0.138196601125011f;
inline constexpr float kRSquared4D = 0.8f;

inline uint64_t
sext(int v)
{
    return (uint64_t)(int64_t)v;
}

inline float
grad(uint64_t seed, uint64_t xsvp, uint64_t ysvp, uint64_t zsvp, uint64_t wsvp, float dx, float dy, float dz, float dw)
{
    uint64_t hash = seed ^ (xsvp ^ ysvp) ^ (zsvp ^ wsvp);
    hash *= kHashMultiplier;
    hash ^= (uint64_t)((int64_t)hash >> 57);
    // Upstream's 512-entry table repeats the 160 gradients, so the index wraps at 640 floats.
    int gi = (int)((uint32_t)hash & 0x7FCu) % 640;
    return (kGrad4[gi] * dx + kGrad4[gi + 1] * dy) + (kGrad4[gi + 2] * dz + kGrad4[gi + 3] * dw);
}

inline float
noise4_unskewed_base(int seed, float xs, float ys, float zs, float ws)
{
    float xsf = std::floor(xs);
    float ysf = std::floor(ys);
    float zsf = std::floor(zs);
    float wsf = std::floor(ws);
    int xsb = (int)xsf;
    int ysb = (int)ysf;
    int zsb = (int)zsf;
    int wsb = (int)wsf;
    float xsi = xs - xsf;
    float ysi = ys - ysf;
    float zsi = zs - zsf;
    float wsi = ws - wsf;

    float ssi = (xsi + ysi + zsi + wsi) * kUnskew4D;
    float xi = xsi + ssi;
    float yi = ysi + ssi;
    float zi = zsi + ssi;
    float wi = wsi + ssi;

    int index = (((int)std::floor(xs * 4.0f) & 3) << 0) | (((int)std::floor(ys * 4.0f) & 3) << 2)
        | (((int)std::floor(zs * 4.0f) & 3) << 4) | (((int)std::floor(ws * 4.0f) & 3) << 6);

    uint64_t seed64 = sext(seed);
    float value = 0.0f;
    uint32_t range = kLookup4DA[index];
    for (uint32_t i = range & 0xFFFFu; i < (range >> 16); i++) {
        uint32_t code = kLookup4DB[i];
        int cx = (int)((code >> 0) & 3u) - 1;
        int cy = (int)((code >> 2) & 3u) - 1;
        int cz = (int)((code >> 4) & 3u) - 1;
        int cw = (int)((code >> 6) & 3u) - 1;
        float ssv = (float)(cx + cy + cz + cw) * kUnskew4D;
        float dx = xi + (-(float)cx - ssv);
        float dy = yi + (-(float)cy - ssv);
        float dz = zi + (-(float)cz - ssv);
        float dw = wi + (-(float)cw - ssv);
        float a = (dx * dx + dy * dy) + (dz * dz + dw * dw);
        if (a < kRSquared4D) {
            a -= kRSquared4D;
            a *= a;
            value += a * a
                * grad(seed64, sext(xsb + cx) * kPrimeX, sext(ysb + cy) * kPrimeY, sext(zsb + cz) * kPrimeZ,
                       sext(wsb + cw) * kPrimeW, dx, dy, dz, dw);
        }
    }
    return value;
}

// XYZ oriented like Noise3_ImproveXY, W free: upstream's recommendation for an animated (W = time)
// texture whose Z is vertical, here the image plane's normal. W repeats eventually.
inline float
noise4_improve_xyz_improve_xy(int seed, float x, float y, float z, float w)
{
    float xy = x + y;
    float s2 = xy * -0.211324865405187f;
    float zz = z * 0.288675134594812942f;
    float ww = w * 1.118033988749894f;
    float xr = x + (zz + ww + s2);
    float yr = y + (zz + ww + s2);
    float zr = xy * -0.577350269189626f + (zz + ww);
    float wr = z * -0.866025403784439f + ww;
    return noise4_unskewed_base(seed, xr, yr, zr, wr);
}

inline float
noise4_fallback(int seed, float x, float y, float z, float w)
{
    float s = kSkew4D * (x + y + z + w);
    return noise4_unskewed_base(seed, x + s, y + s, z + s, w + s);
}

} // namespace noisespike::opensimplex
