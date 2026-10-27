import mapremap;

// IErode's and IDilate's filter, the definition of IErodeDilateKernels::filterPlane() in
// Engine/Nodes/Filter/IErodeDilate.cpp, one component at a time: the row tables of the whole
// domain at once, which the GPU's radius cap of 255 keeps small, where the CPU bounds its memory
// with batches. The work buffer covers the CPU's scratch rect, the domain of domWidth x domHeight
// pixels from (domX1, domY1): level k of the row tables at k x domWidth x domHeight, level 0
// being the component itself.
//
// Minima and maxima are exact, and the radii and the blends are the CPU's float operations in the
// same order, precise so that none contracts, so the result matches the CPU's bit for bit. The
// Map's gamma divides, and mapremap.slang rounds that division itself, since the device need not.
//
// MorphParams and MorphConstants mirror IErodeDilateGpuParams and IErodeDilateGpuConstants in
// Engine/Nodes/Filter/IErodeDilate.cpp.

struct MorphConstants
{
    // kMap* flags.
    uint mapFlags;
    // The Map's channel, of mapChannels floats per pixel, its pixels (mapX1, mapY1)-(mapX2, mapY2)
    // from mapOffset, rows mapStride floats apart.
    uint mapChannel;
    uint mapChannels;
    uint mapOffset;
    uint mapStride;
    int mapX1;
    int mapY1;
    int mapX2;
    int mapY2;
    // MapInputParams::remap()'s exponent, 1 / Map Gamma; kMapSmooth in mapFlags is its smooth.
    float exponent;
    uint remapPad;
    // The Map value outside the Map image, or everywhere without a Map, as the CPU computes it.
    float outside;
    // IErodeDilateAxis of x and y.
    float blackRx;
    float whiteRx;
    float blackRy;
    float whiteRy;
};

// load: the source image.
StructuredBuffer<uint> srcBuf;
// filter: the Map image.
StructuredBuffer<uint> mapBuf;
StructuredBuffer<MorphConstants> constBuf;
RWStructuredBuffer<float> workBuf;
// filter: outWidth x outHeight pixels at (outX1, outY1), the first at dstOffset.
RWStructuredBuffer<float> outBuf;

struct MorphParams
{
    int domX1;
    int domY1;
    uint domWidth;
    uint domHeight;
    uint channels;
    // The component the table and the filter work on.
    uint comp;
    // level: the level it builds.
    uint level;
    // kDisc and kTakeMax.
    uint flags;
    // load: the source pixels that exist, the first at srcOffset; empty when there are none.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    int outX1;
    int outY1;
    uint outWidth;
    uint outHeight;
    uint dstOffset;
    uint dstStride;
};
[[vk::push_constant]] ConstantBuffer<MorphParams> params;

// Mirrored in Engine/Nodes/Filter/IErodeDilate.cpp.
static const uint kDisc = 1;
static const uint kTakeMax = 2;
static const uint kMapConnected = 1;
static const uint kMapRead = 2;
static const uint kMapInvert = 4;
static const uint kMapSmooth = 8;

// IErodeDilateKernels' reduce(), in the same argument order.
float reduce(float a, float b, bool takeMax)
{
    if (takeMax)
        return (b > a) ? b : a;
    return (b < a) ? b : a;
}

// blend() there.
float blend(float a, float b, float t)
{
    if (a == b || t == 0.0)
        return a;
    precise float d = b - a;
    precise float s = t * d;
    precise float r = a + s;
    return r;
}

// MapInputParams::remap(), by mapremap.slang.
float remapMap(MorphConstants k, float raw)
{
    return mapRemap(raw, (k.mapFlags & kMapInvert) != 0u, (k.mapFlags & kMapSmooth) != 0u, k.exponent);
}

// IErodeDilateAxis::at().
float axisAt(float black, float white, float m)
{
    if (m >= 1.0)
        return white;
    if (!(m > 0.0))
        return black;
    precise float d = white - black;
    precise float s = m * d;
    precise float r = black + s;
    return min(max(r, min(black, white)), max(black, white));
}

// IErodeDilateKernels::ellipseHalfWidth(), for radii of at most 255 so that 32 bits hold it.
int ellipseHalfWidth(int rx, int ry, int dy)
{
    if (dy < 0)
        dy = -dy;
    if (dy > ry)
        return -1;
    if (ry == 0 || rx == 0)
        return rx;
    const uint ry2 = uint(ry * ry);
    const uint rhs = uint(rx * rx) * uint(ry * ry - dy * dy);
    int d = min(rx, int(sqrt(float(rhs)) / float(ry)));
    while (d < rx && uint((d + 1) * (d + 1)) * ry2 <= rhs)
        ++d;
    while (d > 0 && uint(d * d) * ry2 > rhs)
        --d;
    return d;
}

uint levelIndex(uint level, uint x, uint y)
{
    return (level * params.domHeight + y) * params.domWidth + x;
}

// rowQuery(): the extremum of row y from x1 to x2, both included.
float rowQuery(int y, int x1, int x2, bool takeMax)
{
    const uint k = firstbithigh(uint(x2 - x1 + 1));
    if (k == 0u)
        return workBuf[levelIndex(0u, uint(x1), uint(y))];
    return reduce(workBuf[levelIndex(k, uint(x1), uint(y))], workBuf[levelIndex(k, uint(x2 - (1 << k) + 1), uint(y))], takeMax);
}

// elementExtremum(): the element of whole radii (rx, ry) at (x, y), clipped to the domain.
float elementExtremum(int x, int y, int rx, int ry, bool disc, bool takeMax)
{
    const int y1 = max(0, y - ry);
    const int y2 = min(int(params.domHeight) - 1, y + ry);
    float acc = 0.0;
    for (int yy = y1; yy <= y2; ++yy) {
        const int hw = disc ? ellipseHalfWidth(rx, ry, yy - y) : rx;
        const int xa = max(0, x - hw);
        const int xb = min(int(params.domWidth) - 1, x + hw);
        const float q = rowQuery(yy, xa, xb, takeMax);
        acc = (yy == y1) ? q : reduce(acc, q, takeMax);
    }
    return acc;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ierodedilate_load(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.domWidth || tid.y >= params.domHeight)
        return;
    const int x = params.domX1 + int(tid.x);
    const int y = params.domY1 + int(tid.y);
    float v = 0.0;
    if (x >= params.srcX1 && x < params.srcX2 && y >= params.srcY1 && y < params.srcY2)
        v = asfloat(srcBuf[params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.channels + params.comp]);
    workBuf[levelIndex(0u, tid.x, tid.y)] = v;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ierodedilate_level(uint3 tid : SV_DispatchThreadID)
{
    const uint span = 1u << params.level;
    if (tid.y >= params.domHeight || tid.x + span > params.domWidth)
        return;
    const uint half = span >> 1;
    const bool takeMax = (params.flags & kTakeMax) != 0u;
    workBuf[levelIndex(params.level, tid.x, tid.y)] = reduce(workBuf[levelIndex(params.level - 1u, tid.x, tid.y)], workBuf[levelIndex(params.level - 1u, tid.x + half, tid.y)], takeMax);
}

// filterRow(): output pixel tid of component comp, from the Map's radii, the
// four elements of their floors and ceilings blended in x then in y; zero outside the domain.
[shader("compute")]
[numthreads(8, 8, 1)]
void ierodedilate_filter(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.outWidth || tid.y >= params.outHeight)
        return;
    const uint dst = params.dstOffset + tid.y * params.dstStride + tid.x * params.channels + params.comp;
    const int ax = params.outX1 + int(tid.x);
    const int ay = params.outY1 + int(tid.y);
    const int x = ax - params.domX1;
    const int y = ay - params.domY1;
    if (x < 0 || y < 0 || x >= int(params.domWidth) || y >= int(params.domHeight)) {
        outBuf[dst] = 0.0;
        return;
    }
    const MorphConstants k = constBuf[0];
    float m = k.outside;
    if ((k.mapFlags & kMapRead) != 0u && ax >= k.mapX1 && ax < k.mapX2 && ay >= k.mapY1 && ay < k.mapY2)
        m = remapMap(k, asfloat(mapBuf[k.mapOffset + uint(ay - k.mapY1) * k.mapStride + uint(ax - k.mapX1) * k.mapChannels + k.mapChannel]));
    const float rxf = axisAt(k.blackRx, k.whiteRx, m);
    const float ryf = axisAt(k.blackRy, k.whiteRy, m);
    const float fx = floor(rxf);
    const float fy = floor(ryf);
    precise float tx = rxf - fx;
    precise float ty = ryf - fy;
    const int r0x = int(fx);
    const int r0y = int(fy);
    const int r1x = (tx > 0.0) ? r0x + 1 : r0x;
    const int r1y = (ty > 0.0) ? r0y + 1 : r0y;
    const bool disc = (params.flags & kDisc) != 0u;
    const bool takeMax = (params.flags & kTakeMax) != 0u;

    const float v00 = elementExtremum(x, y, r0x, r0y, disc, takeMax);
    const float v10 = (r1x != r0x) ? elementExtremum(x, y, r1x, r0y, disc, takeMax) : v00;
    const float v01 = (r1y != r0y) ? elementExtremum(x, y, r0x, r1y, disc, takeMax) : v00;
    float v11;
    if (r1x != r0x && r1y != r0y)
        v11 = elementExtremum(x, y, r1x, r1y, disc, takeMax);
    else
        v11 = (r1x != r0x) ? v10 : v01;
    const float a = blend(v00, v10, tx);
    const float b = blend(v01, v11, tx);
    outBuf[dst] = blend(a, b, ty);
}
