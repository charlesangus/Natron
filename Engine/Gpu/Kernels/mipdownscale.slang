// Box-downscales a level-0 image by 2^levels in one pass. GpuMipDownscalePush in Engine/Gpu/GpuDownscale.h mirrors
// the parameters.

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

    float sum[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (int y = y1; y < y2; ++y) {
        const uint row = pc.srcOffset + uint(y - pc.srcY1) * pc.srcStride;
        for (int x = x1; x < x2; ++x) {
            const uint pixel = row + uint(x - pc.srcX1) * pc.nComps;
            for (uint c = 0; c < pc.nComps; ++c) {
                sum[c] += srcBuf[pixel + c];
            }
        }
    }
    const int count = max(x2 - x1, 0) * max(y2 - y1, 0);
    const float scale = (count > 0) ? (1.0 / float(count)) : 0.0;
    const uint out = pc.dstOffset + tid.y * pc.dstStride + tid.x * pc.nComps;
    for (uint c = 0; c < pc.nComps; ++c) {
        dstBuf[out + c] = sum[c] * scale;
    }
}
