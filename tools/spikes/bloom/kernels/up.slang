// Upsamples a blurred pyramid level by `factor` (bilinear or Catmull-Rom; factor 1 copies) and adds
// it to the output, which `first` clears beforehand; a positive divisor then divides the sum.
// Same taps and sum order as upTaps() and upsampleAdd() in pyramid.cpp.

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;

struct UpParams
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
    uint factor;
    uint bicubic;
    uint first;
    float divisor;
};
[[vk::push_constant]] ConstantBuffer<UpParams> p;

float sampleAt(int x, int y, uint c)
{
    if (x < p.validX1 || x >= p.validX2 || y < p.validY1 || y >= p.validY2) {
        return 0.0;
    }
    return srcBuf[uint(y - p.srcY1) * p.srcStride + uint(x - p.srcX1) * p.channels + c];
}

int floorDiv(int a, int b)
{
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Level sample i is centred on absolute position factor * i + factor / 2, so pixel x sits at level
// position (2x + 1 - factor) / (2 factor); the fraction is exact in float.
void taps(int x, out int i0, out float w[4], out int count)
{
    w[0] = 0.0;
    w[1] = 0.0;
    w[2] = 0.0;
    w[3] = 0.0;
    const int factor = int(p.factor);
    if (factor == 1) {
        i0 = x;
        w[0] = 1.0;
        count = 1;
        return;
    }
    const int num = 2 * x + 1 - factor;
    const int den = 2 * factor;
    const int i = floorDiv(num, den);
    const float f = float(num - i * den) / float(den);
    if (p.bicubic == 0) {
        i0 = i;
        w[0] = 1.0 - f;
        w[1] = f;
        count = 2;
    } else {
        const float f2 = f * f;
        const float f3 = f2 * f;
        i0 = i - 1;
        w[0] = 0.5 * (-f3 + 2.0 * f2 - f);
        w[1] = 0.5 * (3.0 * f3 - 5.0 * f2 + 2.0);
        w[2] = 0.5 * (-3.0 * f3 + 4.0 * f2 + f);
        w[3] = 0.5 * (f3 - f2);
        count = 4;
    }
}

[shader("compute")]
[numthreads(16, 16, 1)]
void up_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= p.dstWidth || tid.y >= p.dstHeight) {
        return;
    }
    const int x = p.dstX1 + int(tid.x);
    const int y = p.dstY1 + int(tid.y);
    int ix;
    int iy;
    int nx;
    int ny;
    float wx[4];
    float wy[4];
    taps(x, ix, wx, nx);
    taps(y, iy, wy, ny);
    const uint out = tid.y * p.dstStride + tid.x * p.channels;
    for (uint c = 0; c < p.channels; ++c) {
        float v = 0.0;
        for (int j = 0; j < ny; ++j) {
            float rowAcc = 0.0;
            for (int i = 0; i < nx; ++i) {
                rowAcc += wx[i] * sampleAt(ix + i, iy + j, c);
            }
            v += wy[j] * rowAcc;
        }
        float acc = p.first != 0 ? 0.0 : dstBuf[out + c];
        acc += v;
        if (p.divisor > 0.0) {
            acc = acc / p.divisor;
        }
        dstBuf[out + c] = acc;
    }
}
