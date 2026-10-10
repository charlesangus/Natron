StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;
StructuredBuffer<float> weights;

struct BlurParams
{
    uint width;
    uint height;
    uint channels;
    uint radius;
    uint vertical;
    uint neumann;
};
[[vk::push_constant]] ConstantBuffer<BlurParams> params;

static const uint kGroup = 128;
static const uint kMaxRadius = 512;
static const uint kMaxTile = kGroup + 2 * kMaxRadius;

groupshared float tile[kMaxTile * 4];

// Position p along the blur axis on the given line, resolved against the
// boundary mode; returns false when the sample is outside under Dirichlet.
bool resolve(int p, int len, bool neumann, out int q)
{
    q = p;
    if (p >= 0 && p < len)
        return true;
    if (!neumann)
        return false;
    q = clamp(p, 0, len - 1);
    return true;
}

uint pixelIndex(uint line, int p)
{
    return params.vertical != 0 ? uint(p) * params.width + line
                                : line * params.width + uint(p);
}

// Dispatch (ceil(axisLen / 128), lineCount, 1): group.x is the tile along the
// blur axis, group.y the row (horizontal) or column (vertical) it filters.
[shader("compute")]
[numthreads(128, 1, 1)]
void blurPass(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const int len = int(params.vertical != 0 ? params.height : params.width);
    const int lines = int(params.vertical != 0 ? params.width : params.height);
    const int R = int(params.radius);
    const uint nc = params.channels;
    const bool neumann = params.neumann != 0;
    const uint line = gid.y;
    const int base = int(gid.x * kGroup);
    const int pos = base + int(tid.x);
    const bool active = pos < len && int(line) < lines;

    __target_switch
    {
    case cpp:
        // The C++ target runs a group's threads one after another, so a
        // barrier cannot publish a cooperatively loaded tile there.
        if (active)
        {
            for (uint c = 0; c < nc; ++c)
            {
                float acc = 0.0;
                for (int k = -R; k <= R; ++k)
                {
                    int q;
                    if (resolve(pos + k, len, neumann, q))
                        acc += weights[k + R] * srcBuf[pixelIndex(line, q) * nc + c];
                }
                dstBuf[pixelIndex(line, pos) * nc + c] = acc;
            }
        }
        break;
    default:
        const int tileLen = int(kGroup) + 2 * R;
        for (int t = int(tid.x); t < tileLen; t += int(kGroup))
        {
            int q = 0;
            const bool ok = int(line) < lines && resolve(base - R + t, len, neumann, q);
            for (uint c = 0; c < nc; ++c)
                tile[uint(t) * nc + c] = ok ? srcBuf[pixelIndex(line, q) * nc + c] : 0.0;
        }
        GroupMemoryBarrierWithGroupSync();
        if (active)
        {
            for (uint c = 0; c < nc; ++c)
            {
                float acc = 0.0;
                for (int k = 0; k <= 2 * R; ++k)
                    acc += weights[k] * tile[(tid.x + uint(k)) * nc + c];
                dstBuf[pixelIndex(line, pos) * nc + c] = acc;
            }
        }
        break;
    }
}
