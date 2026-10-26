import lensmodels;
import resamplecore;

// A test entry for lensmodels.slang's lensMap: maps `count` points in one direction through the
// model packed at the start of modelBuf. Each point is kLensTestPointWords floats of pointBuf, x and
// y as two-float pairs; each result is kLensTestResultWords floats of resultBuf: 1 or 0 for ok, then
// x, y and Jxx, Jxy, Jyx, Jyy as pairs, all zero when not ok.

static const uint kLensTestPointWords = 4;
static const uint kLensTestResultWords = 13;

struct LensTestParams
{
    uint count;
    uint direction;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> modelBuf;
[[vk::binding(1, 0)]] StructuredBuffer<float> pointBuf;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> resultBuf;
[[vk::push_constant]] ConstantBuffer<LensTestParams> lensTest;

void storeFF(uint index, FF v)
{
    resultBuf[index] = v.hi;
    resultBuf[index + 1] = v.lo;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void lensmodels_test_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= lensTest.count)
        return;
    const uint p = tid.x * kLensTestPointWords;
    const FF x = ffMake(pointBuf[p], pointBuf[p + 1]);
    const FF y = ffMake(pointBuf[p + 2], pointBuf[p + 3]);
    const LensSample s = lensMap(lensModelAt(modelBuf, 0), lensTest.direction, x, y);
    const uint r = tid.x * kLensTestResultWords;
    resultBuf[r] = s.ok ? 1.0 : 0.0;
    storeFF(r + 1, s.x);
    storeFF(r + 3, s.y);
    storeFF(r + 5, s.J.j0);
    storeFF(r + 7, s.J.j1);
    storeFF(r + 9, s.J.j2);
    storeFF(r + 11, s.J.j3);
}
