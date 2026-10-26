import pixelwrap;
import noisecore;

// The Noise generator per pixel: NoiseOp::row and noisePixel in Engine/Nodes/Generator/Noise.cpp,
// with the same operations in the same order. The parameter block in wrapAux is NoisePixel;
// NoisePixelParams in Engine/Nodes/Generator/Noise.h mirrors its words.

static const uint kNoiseChannelSeed = 0x632BE5ABu;

// NoisePixelParams, loaded word by word from its 38 words (152 bytes).
struct NoisePixel
{
    NoiseParams noise;
    uint decorrelate;
    float4 color0;
    float4 color1;
    float amplitude;
    float offset;
    float mxx;
    float mxy;
    float myx;
    float myy;
    NoiseAxis bx;
    NoiseAxis by;
    NoiseAxis z;
    NoiseAxis w;
}

NoiseAxis noiseLoadAxis(StructuredBuffer<uint> params, uint index)
{
    NoiseAxis a;
    a.c = params[index];
    a.f = asfloat(params[index + 1]);
    return a;
}

NoiseCoord noiseCoordAt(NoisePixel p, int x, int y)
{
    uint ux = uint(x);
    uint uy = uint(y);
    NoiseAxis nx = axisAdd(axisAdd(axisScale(ux, 0.5f, p.mxx), axisScale(uy, 0.5f, p.mxy)), p.bx);
    NoiseAxis ny = axisAdd(axisAdd(axisScale(ux, 0.5f, p.myx), axisScale(uy, 0.5f, p.myy)), p.by);
    return coordFromAxes(nx, ny, p.z, p.w);
}

NoiseParams noiseChannelParams(NoisePixel p, uint channel)
{
    NoiseParams prm = p.noise;
    if (p.decorrelate != 0u) {
        prm.seed = prm.seed + kNoiseChannelSeed * channel;
    }
    return prm;
}

float noiseColor(NoisePixel p, float n, uint channel)
{
    float t = 0.5f * (1.0f + p.amplitude * n) + p.offset;
    return p.color0[channel] + (p.color1[channel] - p.color0[channel]) * t;
}

struct NoiseOp : IPixelOp
{
    NoisePixel p;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        p.noise.type = int(params[0]);
        p.noise.seed = params[1];
        p.noise.fractal = int(params[2]);
        p.noise.octaves = int(params[3]);
        p.noise.lacunarity = asfloat(params[4]);
        p.noise.gain = asfloat(params[5]);
        p.noise.weightedStrength = asfloat(params[6]);
        p.noise.cellularMetric = int(params[7]);
        p.noise.cellularOutput = int(params[8]);
        p.noise.jitter = asfloat(params[9]);
        p.noise.smoothness = asfloat(params[10]);
        p.noise.warpType = int(params[11]);
        p.noise.warpAmount = asfloat(params[12]);
        p.noise.warpFrequency = asfloat(params[13]);
        p.decorrelate = params[14];
        // Word 15 is reserved.
        p.color0 = wrapParamFloat4(params, 16);
        p.color1 = wrapParamFloat4(params, 20);
        p.amplitude = asfloat(params[24]);
        p.offset = asfloat(params[25]);
        p.mxx = asfloat(params[26]);
        p.mxy = asfloat(params[27]);
        p.myx = asfloat(params[28]);
        p.myy = asfloat(params[29]);
        p.bx = noiseLoadAxis(params, 30);
        p.by = noiseLoadAxis(params, 32);
        p.z = noiseLoadAxis(params, 34);
        p.w = noiseLoadAxis(params, 36);
    }

    // Only the channels the wrapper keeps are computed; it passes the others through.
    float4 apply(float4 v, int2 xy)
    {
        float4 rgba = float4(0.0, 0.0, 0.0, 0.0);
        const uint mask = wrap.channelMask & 15u;
        if (mask == 0u)
            return rgba;
        NoiseCoord q = noiseCoordAt(p, xy.x, xy.y);
        if (p.decorrelate != 0u)
        {
            for (uint c = 0; c < 4; ++c)
            {
                if ((mask & (1u << c)) != 0u)
                    rgba[c] = noiseColor(p, noise4(noiseChannelParams(p, c), q), c);
            }
            return rgba;
        }
        const float n = noise4(p.noise, q);
        for (uint c = 0; c < 4; ++c)
        {
            if ((mask & (1u << c)) != 0u)
                rgba[c] = noiseColor(p, n, c);
        }
        return rgba;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void noise_main(uint3 tid : SV_DispatchThreadID)
{
    NoiseOp op;
    runWrapped<NoiseOp>(op, tid);
}
