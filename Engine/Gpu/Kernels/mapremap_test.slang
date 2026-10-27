import mapremap;

// A test entry for mapremap.slang, one float per thread. Mode 0 remaps aBuf by mapRemap(); mode 1
// divides aBuf by bBuf with mapDivide(); mode 2 divides them with the device's own float division,
// which the test only reports, as Vulkan does not require it to round correctly.

struct MapRemapTestParams
{
    uint count;
    uint mode;
    uint invert;
    uint smooth;
    float exponent;
};

[[vk::push_constant]] ConstantBuffer<MapRemapTestParams> tp;

[[vk::binding(0, 0)]] StructuredBuffer<float> aBuf;
[[vk::binding(1, 0)]] StructuredBuffer<float> bBuf;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> outBuf;

[shader("compute")]
[numthreads(64, 1, 1)]
void mapremap_test_main(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= tp.count)
        return;
    if (tp.mode == 0u) {
        outBuf[i] = mapRemap(aBuf[i], tp.invert != 0u, tp.smooth != 0u, tp.exponent);
    } else if (tp.mode == 1u) {
        outBuf[i] = mapDivide(aBuf[i], bBuf[i]);
    } else {
        precise float q = aBuf[i] / bBuf[i];
        outBuf[i] = q;
    }
}
