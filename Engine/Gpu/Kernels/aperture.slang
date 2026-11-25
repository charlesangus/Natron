// The aperture mask of Engine/Optics/Aperture.cpp, from the tables buildApertureTables() makes. Each
// statement rounds once and in the CPU's order, which is why the locals are precise; the tables hold
// every trigonometric value and every quotient but roundnessGradient's division by the radius. That
// division and sqrt are the only operations Vulkan does not round correctly, so a hard edge can move
// a sample where the two land within a few ulps of it. GpuAperturePush in
// Engine/Gpu/GpuAperture.h mirrors the parameters.

StructuredBuffer<uint> tables;
StructuredBuffer<float> imageBuf;
RWStructuredBuffer<float> dstBuf;

struct AperturePush
{
    uint dstOffset;
    uint dstStride;
};
[[vk::push_constant]] ConstantBuffer<AperturePush> pc;

// Optics::ApertureWord; GpuAperture.cpp asserts the two agree.
static const uint kWidth = 0;
static const uint kHeight = 1;
static const uint kNdcScaleX = 2;
static const uint kNdcScaleY = 3;
static const uint kInvSizeX = 4;
static const uint kInvSizeY = 5;
static const uint kBlades = 6;
static const uint kRoundness = 7;
static const uint kShapeCos = 8;
static const uint kShapeSin = 9;
static const uint kShapeEdge = 10;
static const uint kShapeInvSpan = 11;
static const uint kGratingStrength = 12;
static const uint kGratingCount = 13;
static const uint kGratingHalfW = 14;
static const uint kGratingHalfL = 15;
static const uint kGratingEdge = 16;
static const uint kGratingInvSpan = 17;
static const uint kGratingBound = 18;
static const uint kScratchesStrength = 19;
static const uint kScratchesCount = 20;
static const uint kScratchesHalfW = 21;
static const uint kScratchesHalfL = 22;
static const uint kScratchesEdge = 23;
static const uint kScratchesInvSpan = 24;
static const uint kScratchesBound = 25;
static const uint kScratchesCos = 26;
static const uint kScratchesSin = 27;
static const uint kDustStrength = 28;
static const uint kDustCount = 29;
static const uint kDustRadius = 30;
static const uint kDustRadiusSquared = 31;
static const uint kDustEdge = 32;
static const uint kDustInvSpan = 33;
static const uint kDustBound = 34;
static const uint kImageStrength = 35;
static const uint kImageWidth = 36;
static const uint kImageHeight = 37;
static const uint kImageScaleX = 38;
static const uint kImageScaleY = 39;
static const uint kImageBiasX = 40;
static const uint kImageBiasY = 41;
static const uint kImageBlack = 42;
static const uint kImageWhite = 43;
static const uint kBladeOffset = 44;
static const uint kGratingOffset = 45;
static const uint kScratchesOffset = 46;
static const uint kDustOffset = 47;

static const uint kBladeWords = 2;
static const uint kGratingWords = 4;
static const uint kScratchWords = 6;
static const uint kDustWords = 2;
static const uint kSamples = 8;
static const uint kGratingMaskWords = 12;
static const uint kScratchMaskWords = 32;
static const uint kDustMaskWords = 32;

float tf(uint index)
{
    return asfloat(tables[index]);
}

float coverage(float sdf, float edge, float invSpan)
{
    if (invSpan > 0.0) {
        precise float d = sdf - edge;
        precise float t = d * invSpan;
        t = min(max(t, 0.0), 1.0);
        precise float tt = t * t;
        precise float twice = 2.0 * t;
        precise float s = 3.0 - twice;
        precise float ts = tt * s;
        return 1.0 - ts;
    }
    return sdf < edge ? 1.0 : 0.0;
}

float rectangleSdf(float px, float py, float hx, float hy)
{
    precise float ex = abs(px) - hx;
    precise float ey = abs(py) - hy;
    const float ox = max(ex, 0.0);
    const float oy = max(ey, 0.0);
    precise float oxx = ox * ox;
    precise float oyy = oy * oy;
    precise float o2 = oxx + oyy;
    const float outside = sqrt(o2);
    const float inside = min(max(ex, ey), 0.0);
    precise float sdf = outside + inside;
    return sdf;
}

float roundnessGradient(float px, float py, uint blades)
{
    precise float pxx = px * px;
    precise float pyy = py * py;
    precise float r2 = pxx + pyy;
    precise float c = 1.0;
    if (r2 > 0.0) {
        const float r = sqrt(r2);
        c = -py / r;
        c = min(max(c, -1.0), 1.0);
    }
    precise float onePlus = 1.0 + c;
    precise float oneMinus = 1.0 - c;
    precise float hp = onePlus * 0.5;
    precise float hm = oneMinus * 0.5;
    const float ca = sqrt(max(hp, 0.0));
    const float sa = sqrt(max(hm, 0.0));
    precise float re = 1.0;
    precise float im = 0.0;
    for (uint i = 0; i < blades; ++i) {
        precise float a = re * ca;
        precise float b = im * sa;
        precise float d = re * sa;
        precise float e = im * ca;
        re = a - b;
        im = d + e;
    }
    return (blades % 2) == 0 ? abs(re) : abs(im);
}

float shapeIntensity(float px, float py)
{
    const uint blades = tables[kBlades];
    const uint bladeOffset = tables[kBladeOffset];
    precise float sdf = 0.0;
    for (uint i = 0; i < blades; ++i) {
        const uint e = bladeOffset + i * kBladeWords;
        precise float ax = tf(e) * px;
        precise float ay = tf(e + 1) * py;
        precise float dp = ax + ay;
        sdf = max(sdf, dp);
    }
    const float roundness = tf(kRoundness);
    if (roundness != 0.0) {
        precise float g = roundnessGradient(px, py, blades) * roundness;
        sdf = sdf + g;
    }
    return coverage(sdf, tf(kShapeEdge), tf(kShapeInvSpan));
}

float imageTexel(int x, int y, int width, int height)
{
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return 0.0;
    }
    return imageBuf[uint(y) * uint(width) + uint(x)];
}

float imageIntensity(float nx, float ny)
{
    const int width = int(tables[kImageWidth]);
    const int height = int(tables[kImageHeight]);
    const float w = float(width);
    const float h = float(height);
    precise float sx = nx * tf(kImageScaleX);
    precise float sy = ny * tf(kImageScaleY);
    precise float bx = sx + tf(kImageBiasX);
    precise float by = sy + tf(kImageBiasY);
    precise float wHi = w + 1.0;
    precise float hHi = h + 1.0;
    const float tx = min(max(bx, -2.0), wHi);
    const float ty = min(max(by, -2.0), hHi);
    const float x0 = floor(tx);
    const float y0 = floor(ty);
    precise float fx = tx - x0;
    precise float fy = ty - y0;
    const int ix = int(x0);
    const int iy = int(y0);
    const float a = imageTexel(ix, iy, width, height);
    const float b = imageTexel(ix + 1, iy, width, height);
    const float c = imageTexel(ix, iy + 1, width, height);
    const float d = imageTexel(ix + 1, iy + 1, width, height);
    precise float ba = b - a;
    precise float dc = d - c;
    precise float bax = ba * fx;
    precise float dcx = dc * fx;
    precise float r0 = a + bax;
    precise float r1 = c + dcx;
    precise float rr = r1 - r0;
    precise float rry = rr * fy;
    precise float v = r0 + rry;
    if (v < tf(kImageBlack)) {
        v = 0.0;
    }
    if (v > tf(kImageWhite)) {
        v = 1.0;
    }
    precise float inv = 1.0 - v;
    precise float lost = tf(kImageStrength) * inv;
    precise float r = 1.0 - min(max(lost, 0.0), 1.0);
    return r;
}

float applyLayer(float value, float sum, float strength)
{
    precise float lost = sum * strength;
    precise float keep = 1.0 - min(max(lost, 0.0), 1.0);
    precise float r = value * keep;
    return r;
}

bool culled(float cx, float cy, float bx, float by, float bound)
{
    precise float dx = cx - bx;
    precise float dy = cy - by;
    precise float dxx = dx * dx;
    precise float dyy = dy * dy;
    precise float d2 = dxx + dyy;
    return d2 > bound;
}

float ndc(uint p, uint k, float scale)
{
    precise float ko = float(k) * 0.125;
    precise float o = float(p) + ko;
    precise float s = o * scale;
    precise float n = s - 1.0;
    return n;
}

[numthreads(8, 8, 1)]
void aperture(uint3 tid : SV_DispatchThreadID)
{
    const uint width = tables[kWidth];
    const uint height = tables[kHeight];
    if ((tid.x >= width) || (tid.y >= height)) {
        return;
    }
    const float ndcScaleX = tf(kNdcScaleX);
    const float ndcScaleY = tf(kNdcScaleY);
    const float invSizeX = tf(kInvSizeX);
    const float invSizeY = tf(kInvSizeY);
    const float shapeCos = tf(kShapeCos);
    const float shapeSin = tf(kShapeSin);
    const float scratchesCos = tf(kScratchesCos);
    const float scratchesSin = tf(kScratchesSin);

    precise float pcx = float(tid.x) + 0.5;
    precise float pcy = float(tid.y) + 0.5;
    precise float pcxs = pcx * ndcScaleX;
    precise float pcys = pcy * ndcScaleY;
    precise float centreX = pcxs - 1.0;
    precise float centreY = pcys - 1.0;
    precise float cx = centreX * invSizeX;
    precise float cy = centreY * invSizeY;
    precise float qca = scratchesCos * cx;
    precise float qcb = scratchesSin * cy;
    precise float qcc = scratchesSin * cx;
    precise float qcd = scratchesCos * cy;
    precise float qcx = qca - qcb;
    precise float qcy = qcc + qcd;

    const float gratingStrength = tf(kGratingStrength);
    const float scratchesStrength = tf(kScratchesStrength);
    const float dustStrength = tf(kDustStrength);
    const bool hasImage = tf(kImageStrength) > 0.0 && tables[kImageWidth] > 0;

    // Whether an entry is culled depends only on the pixel, so each layer's survivors are found once
    // here, one bit per entry; walking the bits upwards keeps the CPU's order and so its sums.
    const uint gratingCount = gratingStrength > 0.0 ? tables[kGratingCount] : 0;
    const uint gratingOffset = tables[kGratingOffset];
    const uint gratingWords = (gratingCount + 31) / 32;
    uint gratingMask[kGratingMaskWords];
    for (uint w = 0; w < kGratingMaskWords; ++w) {
        gratingMask[w] = 0;
    }
    {
        const float bound = tf(kGratingBound);
        for (uint i = 0; i < gratingCount; ++i) {
            const uint e = gratingOffset + i * kGratingWords;
            if (!culled(cx, cy, tf(e + 2), tf(e + 3), bound)) {
                gratingMask[i >> 5] |= 1u << (i & 31);
            }
        }
    }

    const uint scratchesCount = scratchesStrength > 0.0 ? tables[kScratchesCount] : 0;
    const uint scratchesOffset = tables[kScratchesOffset];
    const uint scratchesWords = (scratchesCount + 31) / 32;
    uint scratchesMask[kScratchMaskWords];
    for (uint w = 0; w < kScratchMaskWords; ++w) {
        scratchesMask[w] = 0;
    }
    {
        const float bound = tf(kScratchesBound);
        for (uint i = 0; i < scratchesCount; ++i) {
            const uint e = scratchesOffset + i * kScratchWords;
            if (!culled(qcx, qcy, tf(e + 4), tf(e + 5), bound)) {
                scratchesMask[i >> 5] |= 1u << (i & 31);
            }
        }
    }

    const uint dustCount = dustStrength > 0.0 ? tables[kDustCount] : 0;
    const uint dustOffset = tables[kDustOffset];
    const uint dustWords = (dustCount + 31) / 32;
    uint dustMask[kDustMaskWords];
    for (uint w = 0; w < kDustMaskWords; ++w) {
        dustMask[w] = 0;
    }
    {
        const float bound = tf(kDustBound);
        for (uint i = 0; i < dustCount; ++i) {
            const uint e = dustOffset + i * kDustWords;
            if (!culled(cx, cy, tf(e), tf(e + 1), bound)) {
                dustMask[i >> 5] |= 1u << (i & 31);
            }
        }
    }

    precise float total = 0.0;
    for (uint sy = 0; sy < kSamples; ++sy) {
        const float ny = ndc(tid.y, sy, ndcScaleY);
        for (uint sx = 0; sx < kSamples; ++sx) {
            const float nx = ndc(tid.x, sx, ndcScaleX);
            precise float px = nx * invSizeX;
            precise float py = ny * invSizeY;
            precise float ra = shapeCos * px;
            precise float rb = shapeSin * py;
            precise float rc = shapeSin * px;
            precise float rd = shapeCos * py;
            precise float rx = ra - rb;
            precise float ry = rc + rd;
            precise float value = shapeIntensity(rx, ry);

            if (gratingStrength > 0.0) {
                const float halfW = tf(kGratingHalfW);
                const float halfL = tf(kGratingHalfL);
                const float edge = tf(kGratingEdge);
                const float invSpan = tf(kGratingInvSpan);
                precise float sum = 0.0;
                for (uint w = 0; w < gratingWords; ++w) {
                    uint bits = gratingMask[w];
                    while (bits != 0) {
                        const uint i = w * 32 + firstbitlow(bits);
                        bits &= bits - 1;
                        const uint e = gratingOffset + i * kGratingWords;
                        const float bc = tf(e);
                        const float bs = tf(e + 1);
                        precise float a = bc * px;
                        precise float b = bs * py;
                        precise float c = bs * px;
                        precise float d = bc * py;
                        precise float gx = a - b;
                        precise float gcd = c + d;
                        precise float gy = gcd - 1.5;
                        sum = sum + coverage(rectangleSdf(gx, gy, halfW, halfL), edge, invSpan);
                    }
                }
                value = applyLayer(value, sum, gratingStrength);
            }

            if (scratchesStrength > 0.0) {
                const float halfW = tf(kScratchesHalfW);
                const float halfL = tf(kScratchesHalfL);
                const float edge = tf(kScratchesEdge);
                const float invSpan = tf(kScratchesInvSpan);
                precise float qa = scratchesCos * px;
                precise float qb = scratchesSin * py;
                precise float qc = scratchesSin * px;
                precise float qd = scratchesCos * py;
                precise float qx = qa - qb;
                precise float qy = qc + qd;
                precise float sum = 0.0;
                for (uint w = 0; w < scratchesWords; ++w) {
                    uint bits = scratchesMask[w];
                    while (bits != 0) {
                        const uint i = w * 32 + firstbitlow(bits);
                        bits &= bits - 1;
                        const uint e = scratchesOffset + i * kScratchWords;
                        const float ox = tf(e);
                        const float oy = tf(e + 1);
                        const float sc = tf(e + 2);
                        const float ss = tf(e + 3);
                        precise float a = sc * qx;
                        precise float b = ss * qy;
                        precise float c = ss * qx;
                        precise float d = sc * qy;
                        precise float ab = a - b;
                        precise float cd = c + d;
                        precise float sx2 = ab - ox;
                        precise float sy2 = cd - oy;
                        sum = sum + coverage(rectangleSdf(sx2, sy2, halfW, halfL), edge, invSpan);
                    }
                }
                value = applyLayer(value, sum, scratchesStrength);
            }

            if (dustStrength > 0.0) {
                const float radius = tf(kDustRadius);
                const float radius2 = tf(kDustRadiusSquared);
                const float edge = tf(kDustEdge);
                const float invSpan = tf(kDustInvSpan);
                precise float sum = 0.0;
                for (uint w = 0; w < dustWords; ++w) {
                    uint bits = dustMask[w];
                    while (bits != 0) {
                        const uint i = w * 32 + firstbitlow(bits);
                        bits &= bits - 1;
                        const uint e = dustOffset + i * kDustWords;
                        const float ox = tf(e);
                        const float oy = tf(e + 1);
                        precise float dx = px - ox;
                        precise float dy = py - oy;
                        precise float dxx = dx * dx;
                        precise float dyy = dy * dy;
                        precise float d2 = dxx + dyy;
                        precise float cover;
                        if (invSpan > 0.0) {
                            precise float sdf = sqrt(d2) - radius;
                            cover = coverage(sdf, edge, invSpan);
                        } else {
                            cover = d2 < radius2 ? 1.0 : 0.0;
                        }
                        sum = sum + cover;
                    }
                }
                value = applyLayer(value, sum, dustStrength);
            }

            if (hasImage) {
                value = value * imageIntensity(nx, ny);
            }
            total = total + value;
        }
    }
    precise float mean = total * (1.0 / 64.0);
    dstBuf[pc.dstOffset + tid.y * pc.dstStride + tid.x] = mean;
}
