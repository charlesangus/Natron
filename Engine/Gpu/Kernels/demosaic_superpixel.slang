// Raw Demosaic below full resolution with Zoomed-Out Demosaic at Fast: RawSuperpixel::render() in
// Engine/Nodes/Raw/RawSuperpixel.cpp, one invocation an output pixel, summing in the same order.

import rawlayout;

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;

// RawSuperpixelGpuPush in Engine/Nodes/Raw/RawGpuParams.h mirrors it.
struct SuperpixelParams
{
    int period;
    int originX;
    int originY;
    uint mip;
    uint cells0;
    uint cells1;
    uint cells2;
    uint cells3;
    uint cells4;
    uint cells5;
    uint cells6;
    uint cells7;
    uint cells8;
    uint mode;
    uint srcOffset;
    uint srcStride;
    uint srcComps;
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
    int rodX1;
    int rodY1;
    int rodX2;
    int rodY2;
};
[[vk::push_constant]] ConstantBuffer<SuperpixelParams> pc;

static const uint kModeSuperpixel = 0;
static const uint kModeBoxMono = 1;
static const uint kModeBoxRgb = 2;
static const uint kModePhotositeColor = 3;

RawLayout layoutOf()
{
    RawLayout l;
    l.period = pc.period;
    l.originX = pc.originX;
    l.originY = pc.originY;
    l.cells[0] = pc.cells0;
    l.cells[1] = pc.cells1;
    l.cells[2] = pc.cells2;
    l.cells[3] = pc.cells3;
    l.cells[4] = pc.cells4;
    l.cells[5] = pc.cells5;
    l.cells[6] = pc.cells6;
    l.cells[7] = pc.cells7;
    l.cells[8] = pc.cells8;
    return l;
}

float sourceAt(int x, int y, uint c)
{
    return srcBuf[pc.srcOffset + uint(y - pc.srcY1) * pc.srcStride + uint(x - pc.srcX1) * pc.srcComps + c];
}

// Adds the photosites of box whose colour is in wanted (a bit a colour) to sum and count.
void addPhotosites(RawLayout layout, int4 box, uint wanted, inout float3 sum, inout uint3 count)
{
    for (int y = box.y; y < box.w; ++y)
    {
        for (int x = box.x; x < box.z; ++x)
        {
            const uint c = layout.colourAt(x, y);
            if (c < 3 && ((wanted >> c) & 1u) != 0)
            {
                sum[c] += sourceAt(x, y, 0);
                count[c] += 1u;
            }
        }
    }
}

int4 clipBox(int4 box, int4 clip)
{
    return int4(max(box.x, clip.x), max(box.y, clip.y), min(box.z, clip.z), min(box.w, clip.w));
}

bool emptyBox(int4 box)
{
    return box.x >= box.z || box.y >= box.w;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void demosaic_superpixel(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.outWidth || tid.y >= pc.outHeight)
        return;
    const int x = pc.outX + int(tid.x);
    const int y = pc.outY + int(tid.y);
    const int size = 1 << pc.mip;
    const int4 clip = clipBox(int4(pc.rodX1, pc.rodY1, pc.rodX2, pc.rodY2), int4(pc.srcX1, pc.srcY1, pc.srcX2, pc.srcY2));
    const int4 f = clipBox(int4(x * size, y * size, (x + 1) * size, (y + 1) * size), clip);
    float3 rgb = float3(0.0, 0.0, 0.0);
    if (!emptyBox(f))
    {
        if (pc.mode == kModeBoxMono || pc.mode == kModeBoxRgb)
        {
            const uint channels = (pc.mode == kModeBoxRgb) ? 3u : 1u;
            float3 sum = float3(0.0, 0.0, 0.0);
            for (int sy = f.y; sy < f.w; ++sy)
            {
                for (int sx = f.x; sx < f.z; ++sx)
                {
                    for (uint c = 0; c < channels; ++c)
                        sum[c] += sourceAt(sx, sy, c);
                }
            }
            const float n = float((f.z - f.x) * (f.w - f.y));
            [ForceUnroll]
            for (uint c = 0; c < 3; ++c)
                rgb[c] = sum[channels == 3u ? c : 0u] / n;
        }
        else
        {
            const RawLayout layout = layoutOf();
            float3 sum = float3(0.0, 0.0, 0.0);
            uint3 count = uint3(0, 0, 0);
            addPhotosites(layout, f, 7u, sum, count);
            if (pc.mode == kModePhotositeColor)
            {
                const float n = float((f.z - f.x) * (f.w - f.y));
                [ForceUnroll]
                for (uint c = 0; c < 3; ++c)
                    rgb[c] = sum[c] / n;
            }
            else
            {
                for (uint c = 0; c < 3; ++c)
                {
                    for (int g = 1; g <= pc.period && count[c] == 0; ++g)
                        addPhotosites(layout, clipBox(int4(f.x - g, f.y - g, f.z + g, f.w + g), clip), 1u << c, sum, count);
                }
                [ForceUnroll]
                for (uint c = 0; c < 3; ++c)
                    rgb[c] = count[c] > 0 ? sum[c] / float(count[c]) : 0.0;
            }
        }
    }
    const uint pixel = pc.dstOffset + tid.y * pc.dstStride + tid.x * 4;
    dstBuf[pixel] = rgb.x;
    dstBuf[pixel + 1] = rgb.y;
    dstBuf[pixel + 2] = rgb.z;
    dstBuf[pixel + 3] = 1.0;
}
