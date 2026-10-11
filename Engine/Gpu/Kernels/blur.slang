StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;
StructuredBuffer<float> weights;

// Coordinates are image pixels. The pass writes the rect of width x height pixels at (outX, outY);
// along the blur axis a tap is first resolved against [lineLo, lineHi), then the sample against
// the source rect [srcX1, srcX2) x [srcY1, srcY2), each by the boundary mode. An output pixel
// whose position along the axis lies outside [lineLo, lineHi) is written as zero.
struct BlurParams
{
    uint width;
    uint height;
    uint channels;
    uint radius;
    uint vertical;
    uint neumann;
    int outX;
    int outY;
    uint dstOffset;
    uint dstStride;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcOffset;
    uint srcStride;
    int lineLo;
    int lineHi;
};
[[vk::push_constant]] ConstantBuffer<BlurParams> params;

// kGroup, kColumns and kDirectMaxRadius are mirrored in Engine/Gpu/GpuBlur.cpp.
static const uint kGroup = 128;
static const uint kColumns = 32;
static const uint kDirectMaxRadius = 32;
static const uint kMaxRadius = 512;
static const uint kMaxTile = kGroup + 2 * kMaxRadius;

groupshared float tile[kMaxTile * 4];

bool insideLine(int a)
{
    return a >= params.lineLo && a < params.lineHi;
}

// Float index of channel 0 of the source sample at position a along the axis on the given line,
// or false when the sample reads as zero.
bool sourceIndex(int a, int line, out uint idx)
{
    idx = 0;
    const bool neumann = params.neumann != 0;
    if (params.lineLo >= params.lineHi)
        return false;
    if (!insideLine(a))
    {
        if (!neumann)
            return false;
        a = clamp(a, params.lineLo, params.lineHi - 1);
    }
    int x = params.vertical != 0 ? line : a;
    int y = params.vertical != 0 ? a : line;
    if (params.srcX1 >= params.srcX2 || params.srcY1 >= params.srcY2)
        return false;
    if (x < params.srcX1 || x >= params.srcX2 || y < params.srcY1 || y >= params.srcY2)
    {
        if (!neumann)
            return false;
        x = clamp(x, params.srcX1, params.srcX2 - 1);
        y = clamp(y, params.srcY1, params.srcY2 - 1);
    }
    idx = params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.channels;
    return true;
}

float sampleAt(int a, int line, uint c)
{
    uint idx;
    return sourceIndex(a, line, idx) ? srcBuf[idx + c] : 0.0;
}

// pos is the output position along the axis and line the output line, both relative to the rect.
uint outputIndex(uint line, uint pos)
{
    return params.vertical != 0 ? params.dstOffset + pos * params.dstStride + line * params.channels
                                : params.dstOffset + line * params.dstStride + pos * params.channels;
}

int axisOrigin()
{
    return params.vertical != 0 ? params.outY : params.outX;
}

int lineOrigin()
{
    return params.vertical != 0 ? params.outX : params.outY;
}

// The symmetric taps are summed in pairs, w[0] * centre first, in the order of the CPU FIR, so
// both round alike.
float firAt(int a, int line, uint c)
{
    const int R = int(params.radius);
    float acc = weights[0] * sampleAt(a, line, c);
    for (int k = 1; k <= R; ++k)
        acc += weights[k] * (sampleAt(a - k, line, c) + sampleAt(a + k, line, c));
    return acc;
}

// Dispatch shape comes from blurGroupCount in Engine/Gpu/GpuBlur.cpp, which also refuses
// radius > kMaxRadius and more than 4 channels, the limits of `tile`. group.x is the tile along
// the blur axis; group.y is the line it filters, or a block of kColumns columns on the vertical
// direct path. weights holds radius + 1 floats, for offsets 0 to radius.
[shader("compute")]
[numthreads(128, 1, 1)]
void blurPass(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const int len = int(params.vertical != 0 ? params.height : params.width);
    const int lines = int(params.vertical != 0 ? params.width : params.height);
    const int R = int(params.radius);
    const uint nc = params.channels;
    const uint line = gid.y;
    const int base = int(gid.x * kGroup);
    const int pos = base + int(tid.x);
    const bool active = pos < len && int(line) < lines;
    const int a0 = axisOrigin();
    const int l0 = lineOrigin();

    __target_switch
    {
    case cpp:
        // The C++ target runs a group's threads one after another, so a
        // barrier cannot publish a cooperatively loaded tile there.
        if (active)
        {
            const bool inside = insideLine(a0 + pos);
            for (uint c = 0; c < nc; ++c)
                dstBuf[outputIndex(line, uint(pos)) + c] = inside ? firAt(a0 + pos, l0 + int(line), c) : 0.0;
        }
        break;
    default:
        if (params.vertical != 0 && R <= int(kDirectMaxRadius))
        {
            // Small radii: threads of a wave take adjacent columns so every load is a contiguous
            // row segment. Wide kernels reuse each sample so often that the shared tile wins
            // despite its strided loads. group.y selects a block of kColumns columns.
            const uint col = gid.y * kColumns + (tid.x % kColumns);
            if (col < params.width)
            {
                for (uint r = tid.x / kColumns; r < kGroup; r += kGroup / kColumns)
                {
                    const int rowPos = base + int(r);
                    if (rowPos < len)
                    {
                        const bool inside = insideLine(a0 + rowPos);
                        for (uint c = 0; c < nc; ++c)
                            dstBuf[outputIndex(col, uint(rowPos)) + c] = inside ? firAt(a0 + rowPos, l0 + int(col), c) : 0.0;
                    }
                }
            }
        }
        else
        {
            const int tileLen = int(kGroup) + 2 * R;
            for (int t = int(tid.x); t < tileLen; t += int(kGroup))
            {
                uint idx = 0;
                const bool ok = int(line) < lines && sourceIndex(a0 + base - R + t, l0 + int(line), idx);
                for (uint c = 0; c < nc; ++c)
                    tile[uint(t) * nc + c] = ok ? srcBuf[idx + c] : 0.0;
            }
            GroupMemoryBarrierWithGroupSync();
            if (active)
            {
                const bool inside = insideLine(a0 + pos);
                const uint centre = tid.x + uint(R);
                for (uint c = 0; c < nc; ++c)
                {
                    float acc = weights[0] * tile[centre * nc + c];
                    for (uint k = 1; k <= uint(R); ++k)
                        acc += weights[k] * (tile[(centre - k) * nc + c] + tile[(centre + k) * nc + c]);
                    dstBuf[outputIndex(line, uint(pos)) + c] = inside ? acc : 0.0;
                }
            }
        }
        break;
    }
}
