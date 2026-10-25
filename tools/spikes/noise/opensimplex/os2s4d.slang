// Slang mirror of os2s4d.h (4D OpenSimplex2S, ported from K.jpg's csharp/OpenSimplex2S.cs, CC0,
// KdotJPG/OpenSimplex2 4cd120d35bfc27096698de90d1bcbf4f9d359a3b) and voronoi4d.h. Compiled with
// -fp-mode precise so no multiply-add is contracted, matching the CPU build.

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

// kGrad4 (as float bits), then kLookup4DA, then kLookup4DB, packed as os2s4d.h's
// packedTables(). A buffer rather than static const arrays: those become function-local
// copies that every invocation re-initialises.
StructuredBuffer<uint> tables;

static const uint kLookupAOffset = 640u;
static const uint kLookupBOffset = 896u;

static const float kSkew4D = 0.309016994374947f;
static const float kUnskew4D = -0.138196601125011f;
static const float kRSquared4D = 0.8f;

// uint64 arithmetic in 32-bit halves: the device does not enable shaderInt64.
struct U64
{
    uint lo;
    uint hi;
};

U64 u64(uint lo, uint hi)
{
    U64 r;
    r.lo = lo;
    r.hi = hi;
    return r;
}

U64 sext(int v)
{
    return u64(uint(v), v < 0 ? 0xFFFFFFFFu : 0u);
}

U64 xor64(U64 a, U64 b)
{
    return u64(a.lo ^ b.lo, a.hi ^ b.hi);
}

// The full 64-bit product of two 32-bit values, from 16-bit limbs.
U64 mulWide(uint x, uint y)
{
    uint x0 = x & 0xFFFFu;
    uint x1 = x >> 16;
    uint y0 = y & 0xFFFFu;
    uint y1 = y >> 16;
    uint p00 = x0 * y0;
    uint p01 = x0 * y1;
    uint p10 = x1 * y0;
    uint p11 = x1 * y1;
    uint mid = (p00 >> 16) + (p01 & 0xFFFFu) + (p10 & 0xFFFFu);
    return u64((p00 & 0xFFFFu) | (mid << 16), p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16));
}

U64 mul64(U64 a, U64 b)
{
    U64 p = mulWide(a.lo, b.lo);
    return u64(p.lo, p.hi + a.lo * b.hi + a.hi * b.lo);
}

static const U64 kPrimeX = { 0x9270C86Fu, 0x5205402Bu };
static const U64 kPrimeY = { 0x003817B5u, 0x598CD327u };
static const U64 kPrimeZ = { 0x9FA0BACBu, 0x5BCC226Eu };
static const U64 kPrimeW = { 0xE58F554Bu, 0x56CC5227u };
static const U64 kHashMultiplier = { 0xEEC546F5u, 0x53A3F72Du };

float grad(U64 seed, U64 xsvp, U64 ysvp, U64 zsvp, U64 wsvp, float dx, float dy, float dz, float dw)
{
    U64 hash = xor64(xor64(seed, xor64(xsvp, ysvp)), xor64(zsvp, wsvp));
    hash = mul64(hash, kHashMultiplier);
    // Only the low word survives the index mask; it takes bits 57..63 of the arithmetic shift.
    uint low = hash.lo ^ uint(int(hash.hi) >> 25);
    int gi = int(low & 0x7FCu) % 640;
    uint g = uint(gi);
    return (asfloat(tables[g]) * dx + asfloat(tables[g + 1u]) * dy)
        + (asfloat(tables[g + 2u]) * dz + asfloat(tables[g + 3u]) * dw);
}

float noise4_unskewed_base(int seed, float xs, float ys, float zs, float ws)
{
    float xsf = floor(xs);
    float ysf = floor(ys);
    float zsf = floor(zs);
    float wsf = floor(ws);
    int xsb = int(xsf);
    int ysb = int(ysf);
    int zsb = int(zsf);
    int wsb = int(wsf);
    float xsi = xs - xsf;
    float ysi = ys - ysf;
    float zsi = zs - zsf;
    float wsi = ws - wsf;

    float ssi = (xsi + ysi + zsi + wsi) * kUnskew4D;
    float xi = xsi + ssi;
    float yi = ysi + ssi;
    float zi = zsi + ssi;
    float wi = wsi + ssi;

    int index = ((int(floor(xs * 4.0f)) & 3) << 0) | ((int(floor(ys * 4.0f)) & 3) << 2)
        | ((int(floor(zs * 4.0f)) & 3) << 4) | ((int(floor(ws * 4.0f)) & 3) << 6);

    U64 seed64 = sext(seed);
    float value = 0.0f;
    uint range = tables[kLookupAOffset + uint(index)];
    for (uint i = range & 0xFFFFu; i < (range >> 16); i++)
    {
        uint code = tables[kLookupBOffset + i];
        int cx = int((code >> 0) & 3u) - 1;
        int cy = int((code >> 2) & 3u) - 1;
        int cz = int((code >> 4) & 3u) - 1;
        int cw = int((code >> 6) & 3u) - 1;
        float ssv = float(cx + cy + cz + cw) * kUnskew4D;
        float dx = xi + (-float(cx) - ssv);
        float dy = yi + (-float(cy) - ssv);
        float dz = zi + (-float(cz) - ssv);
        float dw = wi + (-float(cw) - ssv);
        float a = (dx * dx + dy * dy) + (dz * dz + dw * dw);
        if (a < kRSquared4D)
        {
            a -= kRSquared4D;
            a *= a;
            value += a * a
                * grad(seed64, mul64(sext(xsb + cx), kPrimeX), mul64(sext(ysb + cy), kPrimeY),
                       mul64(sext(zsb + cz), kPrimeZ), mul64(sext(wsb + cw), kPrimeW), dx, dy, dz, dw);
        }
    }
    return value;
}

float noise4_improve_xyz_improve_xy(int seed, float x, float y, float z, float w)
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

float noise4_fallback(int seed, float x, float y, float z, float w)
{
    float s = kSkew4D * (x + y + z + w);
    return noise4_unskewed_base(seed, x + s, y + s, z + s, w + s);
}

uint lowbias32(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

uint cell_hash(int x, int y, int z, int w, uint seed)
{
    uint h = lowbias32(seed ^ uint(w));
    h = lowbias32(h ^ uint(z));
    h = lowbias32(h ^ uint(y));
    return lowbias32(h ^ uint(x));
}

float hash_to_unit(uint h)
{
    return float(h >> 8) * (1.0f / 16777216.0f);
}

float jitter(uint cellHash, uint axis)
{
    return hash_to_unit(lowbias32(cellHash + axis * 0x9E3779B9u));
}

float voronoi_f1_4d(int seed, float x, float y, float z, float w, float randomness)
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
                    uint h = cell_hash(cx + i, cy + j, cz + k, cw + u, uint(seed));
                    float dx = (float(i) + jitter(h, 0u) * randomness) - lx;
                    float dy = (float(j) + jitter(h, 1u) * randomness) - ly;
                    float dz = (float(k) + jitter(h, 2u) * randomness) - lz;
                    float dw = (float(u) + jitter(h, 3u) * randomness) - lw;
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
void os2s_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    float2 p = noisePosition(tid.xy);
    dstBuf[tid.y * params.width + tid.x] = noise4_improve_xyz_improve_xy(params.seed, p.x, p.y, params.z, params.w);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void voronoi_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    float2 p = noisePosition(tid.xy);
    dstBuf[tid.y * params.width + tid.x] = voronoi_f1_4d(params.seed, p.x, p.y, params.z, params.w, params.randomness);
}
