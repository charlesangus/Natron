StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;
StructuredBuffer<float> weights;

// Coordinates are image pixels. The pass writes the rect of width x height pixels at (outX, outY);
// along the blur axis a tap outside [lineLo, lineHi) reads zero, as does a sample outside the
// source rect [srcX1, srcX2) x [srcY1, srcY2). An output pixel whose position along the axis lies
// outside [lineLo, lineHi) is written as zero.
struct BlurParams
{
    uint width;
    uint height;
    uint channels;
    uint radius;
    uint vertical;
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

// kGroup, kNarrowColumns, kNarrowRows and kDirectMaxRadius are mirrored in Engine/Gpu/GpuBlur.cpp.
static const uint kGroup = 128;
static const uint kNarrowColumns = 8;
static const uint kNarrowRows = 64;
static const uint kNarrowThreadRows = kGroup / kNarrowColumns;
static const uint kDirectMaxRadius = 32;
static const uint kMaxRadius = 512;
static const uint kMaxTile = kGroup + 2 * kMaxRadius;

// Whole pixels, so a tap is one 16-byte read and a wave reads consecutive pixels: with the channels
// as separate floats, lanes four floats apart pile onto a quarter of the LDS banks.
groupshared float4 tile[kMaxTile];

bool insideLine(int a)
{
    return a >= params.lineLo && a < params.lineHi;
}

// Float index of channel 0 of the source sample at position a along the axis on the given line,
// or false when the sample reads as zero.
bool sourceIndex(int a, int line, out uint idx)
{
    idx = 0;
    if (!insideLine(a))
        return false;
    const int x = params.vertical != 0 ? line : a;
    const int y = params.vertical != 0 ? a : line;
    if (params.srcX1 >= params.srcX2 || params.srcY1 >= params.srcY2)
        return false;
    if (x < params.srcX1 || x >= params.srcX2 || y < params.srcY1 || y >= params.srcY2)
        return false;
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
// the blur axis and group.y the line it filters, except on the vertical paths: the direct path
// takes kNarrowColumns columns as group.y and kNarrowRows rows as group.x, and the shared-tile path swaps the two so that
// groups dispatched together filter neighbouring columns and share the cache lines of each row.
// weights holds radius + 1 floats, for offsets 0 to radius.
[shader("compute")]
[numthreads(128, 1, 1)]
void blurPass(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const int len = int(params.vertical != 0 ? params.height : params.width);
    const int lines = int(params.vertical != 0 ? params.width : params.height);
    const int R = int(params.radius);
    const uint nc = params.channels;
    const bool columnMajor = params.vertical != 0 && R > int(kDirectMaxRadius);
    const uint line = columnMajor ? gid.x : gid.y;
    const int base = int((columnMajor ? gid.y : gid.x) * kGroup);
    const int pos = base + int(tid.x);
    const bool active = pos < len && int(line) < lines;
    const int a0 = axisOrigin();
    const int l0 = lineOrigin();

    __target_switch
    {
    case cpp:
        // The C++ target runs a group's threads one after another, so a
        // barrier cannot publish a cooperatively loaded tile there. Its
        // callers dispatch every pass with group.x along the blur axis.
        {
            const int p = int(gid.x * kGroup + tid.x);
            if (p < len && int(gid.y) < lines)
            {
                const bool inside = insideLine(a0 + p);
                for (uint c = 0; c < nc; ++c)
                    dstBuf[outputIndex(gid.y, uint(p)) + c] = inside ? firAt(a0 + p, l0 + int(gid.y), c) : 0.0;
            }
        }
        break;
    default:
        if (params.vertical != 0 && R <= int(kDirectMaxRadius))
        {
            // Small radii: a group filters kNarrowColumns neighbouring columns over kNarrowRows rows.
            // A tile row is then one contiguous run of kNarrowColumns pixels (a whole 128-byte line),
            // each loaded once and reused by every tap, instead of every tap re-reading global memory
            // with a stride of one image row.
            const uint cx = tid.x % kNarrowColumns;
            const uint ty = tid.x / kNarrowColumns;
            const uint col = gid.y * kNarrowColumns + cx;
            const int rowBase = int(gid.x * kNarrowRows);
            const int tileRows = int(kNarrowRows) + 2 * R;
            for (int t = int(ty); t < tileRows; t += int(kNarrowThreadRows))
            {
                uint idx = 0;
                float4 v = float4(0.0);
                if (col < params.width && sourceIndex(a0 + rowBase - R + t, l0 + int(col), idx))
                {
                    [ForceUnroll]
                    for (uint c = 0; c < 4; ++c)
                    {
                        if (c < nc)
                            v[c] = srcBuf[idx + c];
                    }
                }
                tile[uint(t) * kNarrowColumns + cx] = v;
            }
            GroupMemoryBarrierWithGroupSync();
            if (col < params.width)
            {
                for (uint r = ty; r < kNarrowRows; r += kNarrowThreadRows)
                {
                    const int rowPos = rowBase + int(r);
                    if (rowPos >= len)
                        break;
                    const bool inside = insideLine(a0 + rowPos);
                    const uint centre = (r + uint(R)) * kNarrowColumns + cx;
                    float4 acc = weights[0] * tile[centre];
                    for (uint k = 1; k <= uint(R); ++k)
                        acc += weights[k] * (tile[centre - k * kNarrowColumns] + tile[centre + k * kNarrowColumns]);
                    const uint o = outputIndex(col, uint(rowPos));
                    [ForceUnroll]
                    for (uint c = 0; c < 4; ++c)
                    {
                        if (c < nc)
                            dstBuf[o + c] = inside ? acc[c] : 0.0;
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
                float4 v = float4(0.0);
                if (int(line) < lines && sourceIndex(a0 + base - R + t, l0 + int(line), idx))
                {
                    [ForceUnroll]
                    for (uint c = 0; c < 4; ++c)
                    {
                        if (c < nc)
                            v[c] = srcBuf[idx + c];
                    }
                }
                tile[t] = v;
            }
            GroupMemoryBarrierWithGroupSync();
            if (active)
            {
                const bool inside = insideLine(a0 + pos);
                const uint centre = tid.x + uint(R);
                // Componentwise, so every channel is summed in the same order as firAt.
                float4 acc = weights[0] * tile[centre];
                for (uint k = 1; k <= uint(R); ++k)
                    acc += weights[k] * (tile[centre - k] + tile[centre + k]);
                const uint o = outputIndex(line, uint(pos));
                [ForceUnroll]
                for (uint c = 0; c < 4; ++c)
                {
                    if (c < nc)
                        dstBuf[o + c] = inside ? acc[c] : 0.0;
                }
            }
        }
        break;
    }
}
