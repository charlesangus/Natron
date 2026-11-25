import warpchain;
import resamplecore;

// A test entry for warpchain.slang's warpChainMap: maps `count` points through the chain packed at
// the start of chainBuf, with their Jacobians from the identity when `jacobian` is set. Each point
// is kWarpChainTestPointWords floats of pointBuf, x and y as two-float pairs; each result is
// kWarpChainTestResultWords floats of resultBuf: x and y as pairs, NaN without a source, then Jxx,
// Jxy, Jyx, Jyy.

static const uint kWarpChainTestPointWords = 4;
static const uint kWarpChainTestResultWords = 8;

struct WarpChainTestParams
{
    uint count;
    uint jacobian;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> chainBuf;
[[vk::binding(1, 0)]] StructuredBuffer<float> pointBuf;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> resultBuf;
[[vk::push_constant]] ConstantBuffer<WarpChainTestParams> warpChainTest;

[shader("compute")]
[numthreads(64, 1, 1)]
void warpchain_test_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= warpChainTest.count)
        return;
    const uint p = tid.x * kWarpChainTestPointWords;
    FF x = ffMake(pointBuf[p], pointBuf[p + 1]);
    FF y = ffMake(pointBuf[p + 2], pointBuf[p + 3]);
    float4 J = float4(1.0, 0.0, 0.0, 1.0);
    const bool ok = warpChainMap(chainBuf, 0, x, y, J, warpChainTest.jacobian != 0);
    const uint r = tid.x * kWarpChainTestResultWords;
    const float nan = asfloat(0x7fc00000u);
    resultBuf[r] = ok ? x.hi : nan;
    resultBuf[r + 1] = ok ? x.lo : nan;
    resultBuf[r + 2] = ok ? y.hi : nan;
    resultBuf[r + 3] = ok ? y.lo : nan;
    for (uint k = 0; k < 4; ++k)
        resultBuf[r + 4 + k] = J[k];
}
