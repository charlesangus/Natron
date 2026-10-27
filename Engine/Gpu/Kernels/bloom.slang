// The bloom of Engine/Nodes/Filter/BloomKernels.cpp, after its CPU workers: a copy that fills a
// buffer from a source with the nearest pixel repeated or black outside it, the [1 3 3 1] / 8
// downsampling of a pyramid level along one axis, and the Catmull-Rom upsampling of a blurred level
// added into the output. The blur of each level between them is blur.slang's or blur_lines.slang's.
// BloomGpuParams in BloomKernels.cpp mirrors BloomParams.

StructuredBuffer<uint> inBuf;
RWStructuredBuffer<uint> outBuf;

struct BloomParams
{
    // The pixel at (inX1, inY1) of inBuf is at inOffset; consecutive rows are inStride floats apart.
    int inX1;
    int inY1;
    uint inOffset;
    uint inStride;
    // The pixels of inBuf that exist; empty when there are none.
    int validX1;
    int validY1;
    int validX2;
    int validY2;
    // copy, with nearest: the rect a pixel is clamped to before the valid one.
    int clampX1;
    int clampY1;
    int clampX2;
    int clampY2;
    // The rect written, whose first pixel is at outOffset of outBuf.
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint outOffset;
    uint outStride;
    // up_add: the CPU's buffer, outside which the output is zero.
    int domX1;
    int domY1;
    int domX2;
    int domY2;
    uint channels;
    // Whether reads past the valid pixels repeat the nearest one; else they are zero.
    uint nearest;
    // down: 0 halves x, 1 halves y.
    uint axis;
    // up_add: how many times smaller than the output the level is along each axis.
    uint factorX;
    uint factorY;
    // up_add: the output is cleared before the add; a positive divisor then divides the sum.
    uint first;
    float divisor;
    uint pad0;
};

[[vk::push_constant]] ConstantBuffer<BloomParams> params;

static const float kDown[4] = { 0.125, 0.375, 0.375, 0.125 };

bool validEmpty()
{
    return params.validX1 >= params.validX2 || params.validY1 >= params.validY2;
}

// The value of channel c of inBuf at (x, y), already within the valid pixels.
float readIn(int x, int y, uint c)
{
    return asfloat(inBuf[params.inOffset + uint(y - params.inY1) * params.inStride + uint(x - params.inX1) * params.channels + c]);
}

// Reads past the valid pixels: the nearest one, or zero. resolveIndex() in BloomKernels.cpp.
bool resolve(inout int x, inout int y)
{
    if (validEmpty())
        return false;
    if (params.nearest != 0)
    {
        x = clamp(x, params.validX1, params.validX2 - 1);
        y = clamp(y, params.validY1, params.validY2 - 1);
        return true;
    }
    return x >= params.validX1 && x < params.validX2 && y >= params.validY1 && y < params.validY2;
}

int floorDiv(int a, int b)
{
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// upTaps() in BloomKernels.cpp.
void upTaps(uint factorU, int x, out int i0, out float w[4], out int count)
{
    w[0] = 1.0;
    w[1] = 0.0;
    w[2] = 0.0;
    w[3] = 0.0;
    const int factor = int(factorU);
    if (factor == 1)
    {
        i0 = x;
        count = 1;
        return;
    }
    const int num = 2 * x + 1 - factor;
    const int den = 2 * factor;
    const int i = floorDiv(num, den);
    const float f = float(num - i * den) / float(den);
    const float f2 = f * f;
    const float f3 = f2 * f;
    i0 = i - 1;
    w[0] = 0.5 * (-f3 + 2.0 * f2 - f);
    w[1] = 0.5 * (3.0 * f3 - 5.0 * f2 + 2.0);
    w[2] = 0.5 * (-3.0 * f3 + 4.0 * f2 + f);
    w[3] = 0.5 * (f3 - f2);
    count = 4;
}

// fillBuffer() over the output rect: with nearest, the pixel is clamped to the clamp rect, then to
// the valid one; without, a pixel outside the valid rect is zero.
[shader("compute")]
[numthreads(8, 8, 1)]
void bloom_copy(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = min(params.channels, 4u);
    const uint dst = params.outOffset + tid.y * params.outStride + tid.x * params.channels;
    int x = params.outX1 + int(tid.x);
    int y = params.outY1 + int(tid.y);
    if (params.nearest != 0)
    {
        x = clamp(x, params.clampX1, params.clampX2 - 1);
        y = clamp(y, params.clampY1, params.clampY2 - 1);
    }
    const bool inside = resolve(x, y);
    for (uint c = 0; c < n; ++c)
        outBuf[dst + c] = asuint(inside ? readIn(x, y, c) : 0.0);
}

// Sample j of the next level along the axis: the [1 3 3 1] / 8 sum of samples 2j - 1 to 2j + 2.
[shader("compute")]
[numthreads(8, 8, 1)]
void bloom_down(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = min(params.channels, 4u);
    const uint dst = params.outOffset + tid.y * params.outStride + tid.x * params.channels;
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    for (uint c = 0; c < n; ++c)
    {
        float v = 0.0;
        for (int t = 0; t < 4; ++t)
        {
            int sx = (params.axis == 0) ? (2 * x - 1 + t) : x;
            int sy = (params.axis == 0) ? y : (2 * y - 1 + t);
            const bool inside = resolve(sx, sy);
            v += kDown[t] * (inside ? readIn(sx, sy, c) : 0.0);
        }
        outBuf[dst + c] = asuint(v);
    }
}

// The blurred level upsampled at each output pixel and added in; zero outside the CPU's buffer.
[shader("compute")]
[numthreads(8, 8, 1)]
void bloom_up_add(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint n = min(params.channels, 4u);
    const uint dst = params.outOffset + tid.y * params.outStride + tid.x * params.channels;
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    if (x < params.domX1 || x >= params.domX2 || y < params.domY1 || y >= params.domY2)
    {
        for (uint c = 0; c < n; ++c)
            outBuf[dst + c] = 0u;
        return;
    }
    int ix;
    int iy;
    int nx;
    int ny;
    float wx[4];
    float wy[4];
    upTaps(params.factorX, x, ix, wx, nx);
    upTaps(params.factorY, y, iy, wy, ny);
    for (uint c = 0; c < n; ++c)
    {
        float v = 0.0;
        for (int j = 0; j < ny; ++j)
        {
            float rowAcc = 0.0;
            for (int i = 0; i < nx; ++i)
            {
                int sx = ix + i;
                int sy = iy + j;
                if (resolve(sx, sy))
                    rowAcc += wx[i] * readIn(sx, sy, c);
            }
            v += wy[j] * rowAcc;
        }
        float acc = (params.first != 0) ? 0.0 : asfloat(outBuf[dst + c]);
        acc += v;
        if (params.divisor > 0.0)
            acc = acc / params.divisor;
        outBuf[dst + c] = asuint(acc);
    }
}
