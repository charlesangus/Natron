import shapefalloff;

// shapefalloff.slang's test entry, for the CPU/GPU parity test (Tests/Native/ShapeFalloff_Test.cpp).
// Each case reads four floats and writes four. Modes:
//   0 shapeEase(in.x, falloff)
//   1 rampT(in.xy, p0, p1)
//   2 ellipseEdge(in.xy, box): distance, nx, ny
//   3 roundedBoxEdge(in.xy, box, corner): distance, nx, ny
//   4 pixelCoverage and 5 bandCoverage of the edge (in.x, in.y, in.z)
//   6 ellipseT and 7 roundedBoxT at in.xy
//   8 shapeWeight of the edge (in.x, in.y, in.z)

struct ShapeFalloffTestParams
{
    uint count;
    uint mode;
    int falloff;
    float softness;
    ShapeBox box;
    float cornerX;
    float cornerY;
    float pixelW;
    float pixelH;
    float p0x;
    float p0y;
    float p1x;
    float p1y;
};

[[vk::push_constant]] ConstantBuffer<ShapeFalloffTestParams> tp;

[[vk::binding(0, 0)]] StructuredBuffer<float> inBuf;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> outBuf;

[shader("compute")]
[numthreads(64, 1, 1)]
void shapefalloff_test_main(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= tp.count)
        return;
    const float x = inBuf[i * 4u + 0u];
    const float y = inBuf[i * 4u + 1u];
    const float z = inBuf[i * 4u + 2u];
    float4 r = float4(0.f, 0.f, 0.f, 0.f);
    ShapeEdge e;
    e.distance = x;
    e.nx = y;
    e.ny = z;
    if (tp.mode == 0u) {
        r.x = shapeEase(x, tp.falloff);
    } else if (tp.mode == 1u) {
        r.x = rampT(x, y, tp.p0x, tp.p0y, tp.p1x, tp.p1y);
    } else if (tp.mode == 2u || tp.mode == 3u) {
        const ShapeEdge s = tp.mode == 2u ? ellipseEdge(x, y, tp.box) : roundedBoxEdge(x, y, tp.box, tp.cornerX, tp.cornerY);
        r = float4(s.distance, s.nx, s.ny, 0.f);
    } else if (tp.mode == 4u) {
        r.x = pixelCoverage(e, tp.pixelW, tp.pixelH);
    } else if (tp.mode == 5u) {
        r.x = bandCoverage(e, tp.softness, tp.pixelW, tp.pixelH);
    } else if (tp.mode == 6u) {
        r.x = ellipseT(x, y, tp.box, tp.softness, tp.pixelW, tp.pixelH);
    } else if (tp.mode == 7u) {
        r.x = roundedBoxT(x, y, tp.box, tp.cornerX, tp.cornerY, tp.softness, tp.pixelW, tp.pixelH);
    } else {
        r.x = shapeWeight(e, tp.softness, tp.falloff, tp.pixelW, tp.pixelH);
    }
    outBuf[i * 4u + 0u] = r.x;
    outBuf[i * 4u + 1u] = r.y;
    outBuf[i * 4u + 2u] = r.z;
    outBuf[i * 4u + 3u] = r.w;
}
