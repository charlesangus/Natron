/*
 * Ported from darktable, DARKTABLE_MASTER_REF (commit fee19b421fdb), src/iop/contrastntexture.c,
 * Copyright (C) 2026 darktable developers, distributed under the GNU GPL version 3 or later, with
 * the arithmetic of the CPU port in Engine/Nodes/Filter/ContrastTextureKernels.cpp. darktable has
 * no OpenCL path; the blurs run on Kernels/eigf.slang.
 *
 * lowBuf, highBuf and corrBuf are tightly packed and top-down, width x height, the source's region
 * from (fullX1, fullY2 - 1) at the top-left. srcBuf and dstBuf are image views, rows bottom-up.
 */

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> lowBuf;
RWStructuredBuffer<float> highBuf;
RWStructuredBuffer<float> corrBuf;
// The result over the output rect, nComps floats a pixel, rows bottom-up.
RWStructuredBuffer<float> dstBuf;

// ContrastTextureGpuParams in Engine/Nodes/Filter/ContrastTexture.cpp mirrors it.
struct CtParams
{
    uint width;
    uint height;
    int fullX1;
    int fullY2;
    uint srcOffset;
    uint srcStride;
    uint srcComps;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    int srcIndex0;
    int srcIndex1;
    int srcIndex2;
    int srcIndex3;
    uint nComps;
    float noiseBias;
    float gain;
    float reduction;
    uint subtract;
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint display;
    float slopeShadows;
    float slopeHighlights;
    float poly0;
    float poly1;
    float poly2;
    float noiseBias0;
    uint padding0;
};
[[vk::push_constant]] ConstantBuffer<CtParams> pc;

static const float kNormMin = 1.52587890625e-05f;
static const float kPivot = 0.1845f;
static const float kMidtonesWidth = 0.3f;
static const uint kDisplayOff = 0;
static const uint kDisplayLowpass = 4;

// std::fmax / std::fmin for a b that is not NaN.
float fmaxCpu(float a, float b)
{
    if (isnan(a))
        return b;
    return a > b ? a : b;
}

float fminCpu(float a, float b)
{
    if (isnan(a))
        return b;
    return a < b ? a : b;
}

int srcIndex(uint c)
{
    if (c == 0)
        return pc.srcIndex0;
    if (c == 1)
        return pc.srcIndex1;
    if (c == 2)
        return pc.srcIndex2;
    return pc.srcIndex3;
}

// Channel c of the plane at pixel (x, y), zero outside the source view.
float srcPixel(int x, int y, uint c)
{
    if (x < pc.srcX1 || x >= pc.srcX2 || y < pc.srcY1 || y >= pc.srcY2)
        return 0.0f;
    const int idx = srcIndex(c);
    if (idx < 0)
        return 0.0f;
    return srcBuf[pc.srcOffset + uint(y - pc.srcY1) * pc.srcStride + uint(x - pc.srcX1) * pc.srcComps + uint(idx)];
}

int pixelX(uint col)
{
    return pc.fullX1 + int(col);
}

int pixelY(uint row)
{
    return pc.fullY2 - 1 - int(row);
}

float extractDetails(float highpass, float lowpass, float noiseBias)
{
    const float logPixel = log2(fmaxCpu(highpass, kNormMin));
    const float logSmoothed = log2(fmaxCpu(lowpass, kNormMin));
    const float noisePower = noiseBias * noiseBias;
    const float combinedPower = lowpass * lowpass;
    const float wienerGain = fmaxCpu(combinedPower - noisePower, 0.0f) / fmaxCpu(combinedPower, kNormMin);
    return wienerGain * fmaxCpu(fminCpu(logPixel - logSmoothed, 5.0f), -5.0f);
}

float shadowsHighlights(float lowpass)
{
    const float pivotOffset = log2(pc.noiseBias0 + kPivot);
    const float normalizedEv = log2(fmaxCpu(lowpass, kNormMin)) - pivotOffset;
    float correctionEv = 0.0f;
    if (normalizedEv <= -kMidtonesWidth)
        correctionEv += pc.slopeShadows * normalizedEv;
    else if (normalizedEv >= kMidtonesWidth)
        correctionEv += pc.slopeHighlights * normalizedEv;
    else
    {
        const float shiftedEv = normalizedEv + kMidtonesWidth;
        correctionEv += (pc.poly0 + pc.poly1 * shiftedEv + pc.poly2 * shiftedEv * shiftedEv);
    }
    return correctionEv - normalizedEv;
}

// The normalised luminance plus the coarsest level's noise bias, into the low-pass and the
// high-pass; the corrections start at 0.
[shader("compute")]
[numthreads(16, 16, 1)]
void contrastntexture_luminance(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.width || tid.y >= pc.height)
        return;
    const int x = pixelX(tid.x);
    const int y = pixelY(tid.y);
    const uint n = min(pc.nComps, 3u);
    float sum = 0.0f;
    for (uint c = 0; c < n; ++c)
    {
        const float v = srcPixel(x, y, c);
        sum += v * v;
    }
    if (n < 3)
        sum *= 3.0f / float(n);
    const float s = sqrt(sum);
    const float norm = s > kNormMin ? s : kNormMin;
    const float lum = (1.0f / sqrt(3.0f)) * norm + pc.noiseBias;
    const uint k = tid.y * pc.width + tid.x;
    lowBuf[k] = lum;
    highBuf[k] = lum;
    corrBuf[k] = 0.0f;
}

// One level after its blur: the detail into the corrections, the noise bias lowered to the next
// level's, and the high-pass the next level starts from.
[shader("compute")]
[numthreads(16, 16, 1)]
void contrastntexture_details(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.width || tid.y >= pc.height)
        return;
    const uint k = tid.y * pc.width + tid.x;
    float low = lowBuf[k];
    corrBuf[k] = corrBuf[k] + pc.gain * extractDetails(highBuf[k], low, pc.noiseBias);
    if (pc.subtract != 0)
    {
        low -= pc.reduction;
        lowBuf[k] = low;
    }
    highBuf[k] = low;
}

// The result over the output rect: the source times the multiplier, or a preview. Outside the
// source's region the source passes through.
[shader("compute")]
[numthreads(16, 16, 1)]
void contrastntexture_apply(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.outWidth || tid.y >= pc.outHeight)
        return;
    const int x = pc.outX1 + int(tid.x);
    const int y = pc.outY1 + int(tid.y);
    const uint nComps = pc.nComps;
    const uint colours = nComps == 4 ? 3u : nComps;
    const uint base = (tid.y * pc.outWidth + tid.x) * nComps;
    float inPix[4];
    for (uint c = 0; c < 4; ++c)
        inPix[c] = c < nComps ? srcPixel(x, y, c) : 0.0f;

    const int col = x - pc.fullX1;
    const int row = pc.fullY2 - 1 - y;
    if (col < 0 || row < 0 || col >= int(pc.width) || row >= int(pc.height))
    {
        for (uint c = 0; c < nComps; ++c)
            dstBuf[base + c] = inPix[c];
        return;
    }
    const uint k = uint(row) * pc.width + uint(col);
    float outPix[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (pc.display == kDisplayOff)
    {
        const float m = exp2(corrBuf[k] + shadowsHighlights(lowBuf[k]));
        for (uint c = 0; c < colours; ++c)
            outPix[c] = inPix[c] * m;
        outPix[3] = inPix[3];
    }
    else if (pc.display == kDisplayLowpass)
    {
        const float low = lowBuf[k];
        const float intensity = low / ((pc.noiseBias0 + kPivot) + low);
        float saturation = (intensity - 0.5f) * 2.0f;
        saturation *= saturation;
        const float3 tint = intensity < 0.5f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 1.0f, 0.0f);
        for (uint c = 0; c < colours; ++c)
            outPix[c] = colours >= 3 ? intensity * ((1.0f - saturation) + saturation * tint[c]) : intensity;
        outPix[3] = 1.0f;
    }
    else
    {
        const float localEv = corrBuf[k];
        const float intensity = localEv / sqrt(localEv * localEv + 1.0f) * 0.5f + 0.5f;
        for (uint c = 0; c < colours; ++c)
            outPix[c] = intensity;
        outPix[3] = 1.0f;
    }
    for (uint c = 0; c < nComps; ++c)
        dstBuf[base + c] = outPix[c];
}
