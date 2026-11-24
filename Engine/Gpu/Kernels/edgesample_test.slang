import edgesample;
import resamplecore;

// A test entry for edgesample.slang: per read position, the tap edgePixel picks, resamplecore's
// cellPixel and the two axes' readOffset, for one source and edge mode. Positions are two ints each
// in posBuf; each writes kEdgeTestWords floats of dstBuf: the edgePixel tap and the cellPixel
// result as four channels each, the source's channels first and zero past nComps, then readOffset
// along x and y.

static const uint kEdgeTestWords = 10;

struct EdgeTestParams
{
    int x1;
    int y1;
    int x2;
    int y2;
    uint nComps;
    uint count;
    uint edgeMode;
    uint padding;
};

[[vk::binding(0, 0)]] StructuredBuffer<float> srcBuf;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> dstBuf;
[[vk::binding(2, 0)]] StructuredBuffer<int> posBuf;
[[vk::push_constant]] ConstantBuffer<EdgeTestParams> ep;

[shader("compute")]
[numthreads(64, 1, 1)]
void edgesample_test_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= ep.count)
        return;
    const int2 p = int2(posBuf[tid.x * 2], posBuf[tid.x * 2 + 1]);
    ResampleSource src;
    src.buf = srcBuf;
    src.offset = 0;
    src.stride = uint(max(ep.x2 - ep.x1, 0)) * ep.nComps;
    src.x1 = ep.x1;
    src.y1 = ep.y1;
    src.x2 = ep.x2;
    src.y2 = ep.y2;
    src.nComps = ep.nComps;
    src.valid = ep.x2 > ep.x1 && ep.y2 > ep.y1;
    src.edgeMode = ep.edgeMode;

    float4 tap = float4(0.0, 0.0, 0.0, 0.0);
    int2 sampled;
    if (edgePixel(p, int4(ep.x1, ep.y1, ep.x2, ep.y2), ep.edgeMode, sampled))
        tap = srcAt(src, sampled.x, sampled.y);
    float4 cell;
    cellPixel(src, p, cell);

    const uint base = tid.x * kEdgeTestWords;
    for (uint c = 0; c < 4; ++c)
    {
        dstBuf[base + c] = tap[c];
        dstBuf[base + 4 + c] = cell[c];
    }
    dstBuf[base + 8] = float(readOffset(p.x, ep.x1, ep.x2, ep.edgeMode));
    dstBuf[base + 9] = float(readOffset(p.y, ep.y1, ep.y2, ep.edgeMode));
}
