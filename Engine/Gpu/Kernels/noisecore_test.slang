import noisecore;

// noisecore.slang's test entry: noise4 at a list of points, for the CPU/GPU parity test
// (Tests/Native/NoiseCoreGpu_Test.cpp).

// NoiseParams followed by the point count (60 bytes).
struct NoiseSampleParams {
    NoiseParams noise;
    uint count;
};

// Point i is eight words at 8 i: the cells cx, cy, cz, cw, then the fractions' bits.
[[vk::binding(0, 0)]] StructuredBuffer<uint> noiseSamplePoints;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> noiseSampleOut;
[[vk::push_constant]] ConstantBuffer<NoiseSampleParams> noiseSample;

[shader("compute")]
[numthreads(64, 1, 1)]
void noisecore_sample(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= noiseSample.count) {
        return;
    }
    uint base = i * 8u;
    NoiseCoord p;
    p.cx = noiseSamplePoints[base + 0u];
    p.cy = noiseSamplePoints[base + 1u];
    p.cz = noiseSamplePoints[base + 2u];
    p.cw = noiseSamplePoints[base + 3u];
    p.fx = asfloat(noiseSamplePoints[base + 4u]);
    p.fy = asfloat(noiseSamplePoints[base + 5u]);
    p.fz = asfloat(noiseSamplePoints[base + 6u]);
    p.fw = asfloat(noiseSamplePoints[base + 7u]);
    noiseSampleOut[i] = noise4(noiseSample.noise, p);
}
