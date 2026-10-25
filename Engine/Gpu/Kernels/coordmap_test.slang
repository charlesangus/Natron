import coordmap;
import resample;
import resamplecore;

// A test entry for coordmap.slang's sampleAt: samples a buffer of coordinates into width x height
// destination pixels, read and written row by row. Each pixel's coordinate is kCoordWords floats of
// coordBuf: sx, sy, then Jxx, Jxy, Jyx, Jyy. `rp` is resample.slang's, but for its `states`, which
// holds wrapU in bits 0-1, wrapV in bits 2-3 and, in bit 4, whether the Jacobians apply. blurBuf's
// first four words are the wrap domain, x1 y1 x2 y2 as ints.

static const uint kCoordWords = 6;

[[vk::binding(3, 0)]] StructuredBuffer<float> coordBuf;

[shader("compute")]
[numthreads(8, 8, 1)]
void coordmap_test_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= rp.width || tid.y >= rp.height)
        return;
    const uint base = (tid.y * rp.width + tid.x) * kCoordWords;
    const float4 J = float4(coordBuf[base + 2], coordBuf[base + 3], coordBuf[base + 4], coordBuf[base + 5]);
    const int4 domain = int4(int(blurBuf[0]), int(blurBuf[1]), int(blurBuf[2]), int(blurBuf[3]));
    const uint wrapU = rp.states & 3u;
    const uint wrapV = (rp.states >> 2) & 3u;
    const bool hasJacobian = (rp.states & 16u) != 0;
    const bool clampOn = (rp.flags & kFlagClamp) != 0;
    storePixel(tid, sampleAt(paramsSource(), coordBuf[base], coordBuf[base + 1], J, hasJacobian, rp.filter, clampOn, wrapU, wrapV,
                             domain));
}
