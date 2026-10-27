// Halves a pyramid level: level-(j+1) sample (x, y) is the separable [1 3 3 1] / 8 (taps from
// 2x - 1) or [1 1] / 2 (taps from 2x) filter of level j, read as zero outside the valid rect.
// Same sum order as downsample() in pyramid.cpp.

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;

struct DownParams
{
    int srcX1;
    int srcY1;
    uint srcStride;
    int validX1;
    int validY1;
    int validX2;
    int validY2;
    int dstX1;
    int dstY1;
    uint dstWidth;
    uint dstHeight;
    uint dstStride;
    uint channels;
    uint binomial;
};
[[vk::push_constant]] ConstantBuffer<DownParams> p;

float sampleAt(int x, int y, uint c)
{
    if (x < p.validX1 || x >= p.validX2 || y < p.validY1 || y >= p.validY2) {
        return 0.0;
    }
    return srcBuf[uint(y - p.srcY1) * p.srcStride + uint(x - p.srcX1) * p.channels + c];
}

[shader("compute")]
[numthreads(16, 16, 1)]
void down_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= p.dstWidth || tid.y >= p.dstHeight) {
        return;
    }
    const int x = p.dstX1 + int(tid.x);
    const int y = p.dstY1 + int(tid.y);
    float w[4];
    int taps;
    int first;
    if (p.binomial != 0) {
        w[0] = 0.125;
        w[1] = 0.375;
        w[2] = 0.375;
        w[3] = 0.125;
        taps = 4;
        first = -1;
    } else {
        w[0] = 0.5;
        w[1] = 0.5;
        w[2] = 0.0;
        w[3] = 0.0;
        taps = 2;
        first = 0;
    }
    const int sx = 2 * x + first;
    const int sy = 2 * y + first;
    const uint out = tid.y * p.dstStride + tid.x * p.channels;
    for (uint c = 0; c < p.channels; ++c) {
        float acc = 0.0;
        for (int ty = 0; ty < taps; ++ty) {
            float rowAcc = 0.0;
            for (int tx = 0; tx < taps; ++tx) {
                rowAcc += w[tx] * sampleAt(sx + tx, sy + ty, c);
            }
            acc += w[ty] * rowAcc;
        }
        dstBuf[out + c] = acc;
    }
}
