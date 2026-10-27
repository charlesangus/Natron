import mergeops;

// Test-only entry: Tests/Gpu builds it as C++ to hold the CPU operators to mergeops. It lives
// beside mergeops because slangc resolves imports in the importing file's directory, and outside
// mergeops because each global it declares would become a binding of every kernel importing that.
//
// Each case is 16 floats, A, a, B and b as float4s in channel-bit order, and yields one float4.
// oracleCtl holds the operator and the case count.

[[vk::binding(0, 0)]] StructuredBuffer<float> oracleIn;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> oracleOut;
[[vk::binding(2, 0)]] StructuredBuffer<uint> oracleCtl;

float4 oracleRead(uint base)
{
    return float4(oracleIn[base], oracleIn[base + 1], oracleIn[base + 2], oracleIn[base + 3]);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void mergeops_oracle(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= oracleCtl[1])
        return;
    const uint base = i * 16;
    const float4 r = mergePixel(oracleCtl[0], oracleRead(base), oracleRead(base + 4), oracleRead(base + 8), oracleRead(base + 12));
    for (uint c = 0; c < 4; ++c)
        oracleOut[i * 4 + c] = r[c];
}
