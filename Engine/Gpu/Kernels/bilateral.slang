// Bilateral and guided filtering on a log-luminance key, mirroring Engine/Nodes/Filter/Bilateral.cpp.
//
// The passes run over the domain, the CPU's scratch buffer: images are packed over it, channels
// floats per pixel, and the key one float per pixel. A pass writes its out rect; pixels of it
// outside the domain get 0. Every sum runs in the CPU's order, so the two differ only by the
// rounding of exp and log2.

RWStructuredBuffer<float> in0;
RWStructuredBuffer<float> in1;
RWStructuredBuffer<float> outBuf;

// Mirrored by BilateralGpuParams in Bilateral.cpp.
struct BilateralParams
{
    int domX1;
    int domY1;
    int domX2;
    int domY2;
    // The pixels of the image read that exist, the first at srcOffset; empty when there are none.
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    uint srcChannels;
    // The rect written, its first pixel at dstOffset.
    int outX1;
    int outY1;
    int outX2;
    int outY2;
    uint dstOffset;
    uint dstStride;
    uint channels;
    int radius;
    float invTwoSigmaS2;
    float invTwoSigmaR2;
    float eps;
    float weight0;
    float weight1;
    float weight2;
    float weight3;
    int comp0;
    int comp1;
    int comp2;
    int comp3;
    uint padding0;
    uint padding1;
};

[[vk::push_constant]] ConstantBuffer<BilateralParams> params;

static const float kKeyFloor = 1.0 / 65536.0;
static const float kFloatMax = 3.402823466e+38;
static const int kGroup = 16;
static const int kTiledMaxRadius = 8;
static const int kTileSide = kGroup + 2 * kTiledMaxRadius;

groupshared float tileKey[kTileSide * kTileSide];
groupshared float4 tileValue[kTileSide * kTileSide];

int domainWidth()
{
    return params.domX2 - params.domX1;
}

int domainHeight()
{
    return params.domY2 - params.domY1;
}

bool inDomain(int x, int y)
{
    return x >= params.domX1 && x < params.domX2 && y >= params.domY1 && y < params.domY2;
}

// v, or 0 for a NaN or an infinity, by bit test.
float finiteSample(float v)
{
    return (asuint(v) & 0x7f800000u) == 0x7f800000u ? 0.0 : v;
}

float luminanceKey(float l)
{
    if (!(l > kKeyFloor))
        l = kKeyFloor;
    if (l > kFloatMax)
        l = kFloatMax;
    return log2(l);
}

// The index in a domain-packed buffer of n floats per pixel of domain-relative pixel (x, y).
uint packedIndex(int x, int y, uint n)
{
    return (uint(y) * uint(domainWidth()) + uint(x)) * n;
}

uint outputIndex(int x, int y, uint c)
{
    return params.dstOffset + uint(y - params.outY1) * params.dstStride + uint(x - params.outX1) * params.channels + c;
}

void writeZero(int x, int y)
{
    for (uint c = 0; c < params.channels; ++c)
        outBuf[outputIndex(x, y, c)] = 0.0;
}

float channelWeight(uint c)
{
    return c == 0 ? params.weight0 : (c == 1 ? params.weight1 : (c == 2 ? params.weight2 : params.weight3));
}

int channelComp(uint c)
{
    return c == 0 ? params.comp0 : (c == 1 ? params.comp1 : (c == 2 ? params.comp2 : params.comp3));
}

// The source over the domain, 0 where it has no pixel.
[shader("compute")]
[numthreads(16, 16, 1)]
void bilateral_load(uint3 tid : SV_DispatchThreadID)
{
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    if (x >= params.outX2 || y >= params.outY2)
        return;
    const bool inside = x >= params.srcX1 && x < params.srcX2 && y >= params.srcY1 && y < params.srcY2;
    for (uint c = 0; c < params.channels; ++c)
    {
        float v = 0.0;
        if (inside)
            v = in0[params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.srcChannels + c];
        outBuf[outputIndex(x, y, c)] = v;
    }
}

// The key of every domain pixel from the image read: the luminance of the components comp0..3
// weighted by weight0..3, 0 where the image has no pixel.
[shader("compute")]
[numthreads(16, 16, 1)]
void bilateral_key(uint3 tid : SV_DispatchThreadID)
{
    const int x = params.domX1 + int(tid.x);
    const int y = params.domY1 + int(tid.y);
    if (x >= params.domX2 || y >= params.domY2)
        return;
    precise float l = 0.0;
    if (x >= params.srcX1 && x < params.srcX2 && y >= params.srcY1 && y < params.srcY2)
    {
        const uint base = params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.srcChannels;
        for (uint c = 0; c < params.channels; ++c)
        {
            const float w = channelWeight(c);
            const int comp = channelComp(c);
            if (w != 0.0 && comp >= 0)
                l += w * finiteSample(in0[base + uint(comp)]);
        }
    }
    outBuf[packedIndex(x - params.domX1, y - params.domY1, 1)] = luminanceKey(l);
}

// The half-width of the disc of the radius on row dy: the largest s with s^2 + dy^2 <= r^2.
int discSpan(int dy, int r)
{
    int s = int(floor(sqrt(float(r * r - dy * dy))));
    while (s > 0 && s * s + dy * dy > r * r)
        --s;
    while ((s + 1) * (s + 1) + dy * dy <= r * r)
        ++s;
    return s;
}

// One Bilateral iteration reading the image from in0 and the key from in1, both domain-packed.
[shader("compute")]
[numthreads(16, 16, 1)]
void bilateral_direct(uint3 tid : SV_DispatchThreadID)
{
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    if (x >= params.outX2 || y >= params.outY2)
        return;
    if (!inDomain(x, y))
    {
        writeZero(x, y);
        return;
    }
    const uint nc = params.channels;
    const int w = domainWidth();
    const int h = domainHeight();
    const int px = x - params.domX1;
    const int py = y - params.domY1;
    const int r = params.radius;
    const float centre = in1[packedIndex(px, py, 1)];
    precise float weightSum = 0.0;
    precise float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    for (int dy = -r; dy <= r; ++dy)
    {
        const int yy = py + dy;
        if (yy < 0 || yy >= h)
            continue;
        const int s = discSpan(dy, r);
        const int lo = max(-s, -px);
        const int hi = min(s, w - 1 - px);
        for (int dx = lo; dx <= hi; ++dx)
        {
            const uint i = uint(yy) * uint(w) + uint(px + dx);
            precise const float dk = in1[i] - centre;
            precise const float e = float(dx * dx + dy * dy) * params.invTwoSigmaS2 + dk * dk * params.invTwoSigmaR2;
            const float wt = exp(-e);
            weightSum += wt;
            for (uint c = 0; c < nc; ++c)
                acc[c] += wt * finiteSample(in0[i * nc + c]);
        }
    }
    for (uint c = 0; c < nc; ++c)
        outBuf[outputIndex(x, y, c)] = acc[c] / weightSum;
}

// bilateral_direct for a radius up to kTiledMaxRadius, the key and the image of the group's
// window read once into group memory.
[shader("compute")]
[numthreads(16, 16, 1)]
void bilateral_tiled(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint nc = params.channels;
    const int w = domainWidth();
    const int h = domainHeight();
    const int r = params.radius;
    const int side = kGroup + 2 * r;
    // The group's window, domain-relative.
    const int tileX = params.outX1 + int(gid.x) * kGroup - r - params.domX1;
    const int tileY = params.outY1 + int(gid.y) * kGroup - r - params.domY1;
    for (int t = int(gi); t < side * side; t += kGroup * kGroup)
    {
        const int tx = tileX + t % side;
        const int ty = tileY + t / side;
        float k = 0.0;
        float4 v = float4(0.0, 0.0, 0.0, 0.0);
        if (tx >= 0 && tx < w && ty >= 0 && ty < h)
        {
            const uint i = uint(ty) * uint(w) + uint(tx);
            k = in1[i];
            for (uint c = 0; c < nc; ++c)
                v[c] = finiteSample(in0[i * nc + c]);
        }
        tileKey[t] = k;
        tileValue[t] = v;
    }
    GroupMemoryBarrierWithGroupSync();

    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    if (x >= params.outX2 || y >= params.outY2)
        return;
    if (!inDomain(x, y))
    {
        writeZero(x, y);
        return;
    }
    const int px = x - params.domX1;
    const int py = y - params.domY1;
    const int lx = px - tileX;
    const int ly = py - tileY;
    const float centre = tileKey[ly * side + lx];
    precise float weightSum = 0.0;
    precise float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    for (int dy = -r; dy <= r; ++dy)
    {
        const int yy = py + dy;
        if (yy < 0 || yy >= h)
            continue;
        const int s = discSpan(dy, r);
        const int lo = max(-s, -px);
        const int hi = min(s, w - 1 - px);
        for (int dx = lo; dx <= hi; ++dx)
        {
            const int t = (ly + dy) * side + lx + dx;
            precise const float dk = tileKey[t] - centre;
            precise const float e = float(dx * dx + dy * dy) * params.invTwoSigmaS2 + dk * dk * params.invTwoSigmaR2;
            const float wt = exp(-e);
            weightSum += wt;
            const float4 v = tileValue[t];
            for (uint c = 0; c < nc; ++c)
                acc[c] += wt * v[c];
        }
    }
    for (uint c = 0; c < nc; ++c)
        outBuf[outputIndex(x, y, c)] = acc[c] / weightSum;
}

// Guided mode, horizontal box of the key (in1), its square, the image (in0) and key * image:
// out holds, per pixel, the means of the key, the key squared, the channels, then the products.
[shader("compute")]
[numthreads(16, 16, 1)]
void bilateral_guided_rows(uint3 tid : SV_DispatchThreadID)
{
    const int w = domainWidth();
    const int px = int(tid.x);
    const int py = int(tid.y);
    if (px >= w || py >= domainHeight())
        return;
    const uint nc = params.channels;
    const int r = params.radius;
    const int lo = max(0, px - r);
    const int hi = min(w - 1, px + r);
    const float n = float(hi - lo + 1);
    precise float sKey = 0.0;
    precise float sKey2 = 0.0;
    precise float4 sImage = float4(0.0, 0.0, 0.0, 0.0);
    precise float4 sProduct = float4(0.0, 0.0, 0.0, 0.0);
    for (int i = lo; i <= hi; ++i)
    {
        const uint p = packedIndex(i, py, 1);
        const float k = in1[p];
        sKey += k;
        sKey2 += k * k;
        for (uint c = 0; c < nc; ++c)
        {
            const float v = finiteSample(in0[p * nc + c]);
            sImage[c] += v;
            sProduct[c] += k * v;
        }
    }
    const uint o = packedIndex(px, py, 2 + 2 * nc);
    outBuf[o] = sKey / n;
    outBuf[o + 1] = sKey2 / n;
    for (uint c = 0; c < nc; ++c)
    {
        outBuf[o + 2 + c] = sImage[c] / n;
        outBuf[o + 2 + nc + c] = sProduct[c] / n;
    }
}

// Guided mode, vertical box of the row means (in0) and the coefficients from them: out holds,
// per pixel, a of every channel then b of every channel.
[shader("compute")]
[numthreads(16, 16, 1)]
void bilateral_guided_coefficients(uint3 tid : SV_DispatchThreadID)
{
    const int h = domainHeight();
    const int px = int(tid.x);
    const int py = int(tid.y);
    if (px >= domainWidth() || py >= h)
        return;
    const uint nc = params.channels;
    const uint stride = 2 + 2 * nc;
    const int r = params.radius;
    const int lo = max(0, py - r);
    const int hi = min(h - 1, py + r);
    const float n = float(hi - lo + 1);
    precise float sKey = 0.0;
    precise float sKey2 = 0.0;
    precise float4 sImage = float4(0.0, 0.0, 0.0, 0.0);
    precise float4 sProduct = float4(0.0, 0.0, 0.0, 0.0);
    for (int j = lo; j <= hi; ++j)
    {
        const uint i = packedIndex(px, j, stride);
        sKey += in0[i];
        sKey2 += in0[i + 1];
        for (uint c = 0; c < nc; ++c)
        {
            sImage[c] += in0[i + 2 + c];
            sProduct[c] += in0[i + 2 + nc + c];
        }
    }
    precise const float mKey = sKey / n;
    precise const float mKey2 = sKey2 / n;
    precise const float d = max(mKey2 - mKey * mKey, 0.0) + params.eps;
    const uint o = packedIndex(px, py, 2 * nc);
    for (uint c = 0; c < nc; ++c)
    {
        precise const float mImage = sImage[c] / n;
        precise const float mProduct = sProduct[c] / n;
        precise const float a = (mProduct - mKey * mImage) / d;
        outBuf[o + c] = a;
        outBuf[o + nc + c] = mImage - a * mKey;
    }
}

// Guided mode, horizontal box of the coefficients (in0) into out, in the same layout.
[shader("compute")]
[numthreads(16, 16, 1)]
void bilateral_guided_box_rows(uint3 tid : SV_DispatchThreadID)
{
    const int w = domainWidth();
    const int px = int(tid.x);
    const int py = int(tid.y);
    if (px >= w || py >= domainHeight())
        return;
    const uint n2 = 2 * params.channels;
    const int r = params.radius;
    const int lo = max(0, px - r);
    const int hi = min(w - 1, px + r);
    const float n = float(hi - lo + 1);
    const uint o = packedIndex(px, py, n2);
    for (uint q = 0; q < n2; ++q)
    {
        precise float s = 0.0;
        for (int i = lo; i <= hi; ++i)
            s += in0[packedIndex(i, py, n2) + q];
        outBuf[o + q] = s / n;
    }
}

// Guided mode, vertical box of the row-boxed coefficients (in0), applied to the key (in1):
// mean(a) * key + mean(b), written over the out rect.
[shader("compute")]
[numthreads(16, 16, 1)]
void bilateral_guided_apply(uint3 tid : SV_DispatchThreadID)
{
    const int x = params.outX1 + int(tid.x);
    const int y = params.outY1 + int(tid.y);
    if (x >= params.outX2 || y >= params.outY2)
        return;
    if (!inDomain(x, y))
    {
        writeZero(x, y);
        return;
    }
    const uint nc = params.channels;
    const uint n2 = 2 * nc;
    const int h = domainHeight();
    const int px = x - params.domX1;
    const int py = y - params.domY1;
    const int r = params.radius;
    const int lo = max(0, py - r);
    const int hi = min(h - 1, py + r);
    const float n = float(hi - lo + 1);
    precise float4 sa = float4(0.0, 0.0, 0.0, 0.0);
    precise float4 sb = float4(0.0, 0.0, 0.0, 0.0);
    for (int j = lo; j <= hi; ++j)
    {
        const uint i = packedIndex(px, j, n2);
        for (uint c = 0; c < nc; ++c)
        {
            sa[c] += in0[i + c];
            sb[c] += in0[i + nc + c];
        }
    }
    const float k = in1[packedIndex(px, py, 1)];
    for (uint c = 0; c < nc; ++c)
    {
        precise const float ma = sa[c] / n;
        precise const float mb = sb[c] / n;
        outBuf[outputIndex(x, y, c)] = ma * k + mb;
    }
}
