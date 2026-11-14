// Box-downscales a level-0 image by 2^levels in one pass, to what Image::downscaleMipmap() makes in levels halvings.
// GpuMipDownscalePush in Engine/Gpu/GpuDownscale.h mirrors the parameters.

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;

struct MipDownscaleParams
{
    uint levels;
    uint nComps;
    uint srcOffset;
    uint srcStride;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint dstOffset;
    uint dstStride;
    int outX;
    int outY;
    uint outWidth;
    uint outHeight;
};
[[vk::push_constant]] ConstantBuffer<MipDownscaleParams> pc;

// The weight of source coordinate p along one axis in Image::buildMipmapLevel's repeated halving over [lo, hi): each
// halving averages the children of a pixel that intersect [lo, hi), so a partial edge block weighs its pixels unevenly.
float axisWeight(int p, uint levels, int lo, int hi)
{
    float w = 1.0;
    for (uint k = 1; k <= levels; ++k) {
        const int span = 1 << (k - 1);
        const int first = (p >> k) << 1;
        int children = 0;
        for (int c = first; c < first + 2; ++c) {
            if ((c * span < hi) && ((c + 1) * span > lo)) {
                ++children;
            }
        }
        w /= float(children);
    }
    return w;
}

[numthreads(8, 8, 1)]
void mip_downscale(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= pc.outWidth) || (tid.y >= pc.outHeight)) {
        return;
    }
    const int block = 1 << pc.levels;
    const int bx1 = (pc.outX + int(tid.x)) * block;
    const int by1 = (pc.outY + int(tid.y)) * block;
    const int x1 = max(bx1, pc.srcX1);
    const int y1 = max(by1, pc.srcY1);
    const int x2 = min(bx1 + block, pc.srcX2);
    const int y2 = min(by1 + block, pc.srcY2);
    const bool fullX = (x1 == bx1) && (x2 == bx1 + block);
    const bool fullY = (y1 == by1) && (y2 == by1 + block);
    const float even = 1.0 / float(block);

    float sum[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (int y = y1; y < y2; ++y) {
        const uint row = pc.srcOffset + uint(y - pc.srcY1) * pc.srcStride;
        float rowSum[4] = { 0.0, 0.0, 0.0, 0.0 };
        for (int x = x1; x < x2; ++x) {
            const uint pixel = row + uint(x - pc.srcX1) * pc.nComps;
            const float wx = fullX ? even : axisWeight(x, pc.levels, pc.srcX1, pc.srcX2);
            for (uint c = 0; c < pc.nComps; ++c) {
                rowSum[c] += wx * srcBuf[pixel + c];
            }
        }
        const float wy = fullY ? even : axisWeight(y, pc.levels, pc.srcY1, pc.srcY2);
        for (uint c = 0; c < pc.nComps; ++c) {
            sum[c] += wy * rowSum[c];
        }
    }
    const uint out = pc.dstOffset + tid.y * pc.dstStride + tid.x * pc.nComps;
    for (uint c = 0; c < pc.nComps; ++c) {
        dstBuf[out + c] = sum[c];
    }
}
