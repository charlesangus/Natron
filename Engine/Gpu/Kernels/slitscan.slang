// One frame of SlitScan's window per dispatch: each pixel's time comes from the slit ramp or the
// retime map, and the frame is added to the destination with the weight the filter gives it at
// that time. The first pass writes the destination, so a dispatch per frame, oldest first, sums
// the frames in the CPU's order. SlitScanGpuParams in Engine/Nodes/Time/SlitScan.cpp mirrors the
// push block.

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;
StructuredBuffer<float> mapBuf;

struct SlitScanParams
{
    int outX;
    int outY;
    uint width;
    uint height;
    uint dstOffset;
    uint dstStride;
    uint nComps;
    uint srcOffset;
    uint srcStride;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint srcComps;
    uint mapOffset;
    uint mapStride;
    int mapX1;
    int mapY1;
    int mapX2;
    int mapY2;
    uint mapComps;
    uint mapChannel;
    uint function;
    uint filter;
    uint firstPass;
    float frame;
    float rampScale;
    int rampOrigin;
    float gain;
    float base;
    float first;
    float last;
}

[[vk::push_constant]] ConstantBuffer<SlitScanParams> params;

static const uint kVerticalSlit = 1;
static const uint kRetimeMap = 2;
static const uint kLinear = 1;

[shader("compute")]
[numthreads(8, 8, 1)]
void slitscan(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
    {
        return;
    }
    const int x = params.outX + int(tid.x);
    const int y = params.outY + int(tid.y);

    float v = 0.0;
    if (params.function == kRetimeMap)
    {
        if (x >= params.mapX1 && x < params.mapX2 && y >= params.mapY1 && y < params.mapY2)
        {
            v = mapBuf[params.mapOffset + uint(y - params.mapY1) * params.mapStride + uint(x - params.mapX1) * params.mapComps + params.mapChannel];
        }
    }
    else
    {
        const int coord = params.function == kVerticalSlit ? x : y;
        precise float ramp = float(coord - params.rampOrigin) * params.rampScale;
        v = ramp;
    }
    // `precise` keeps the multiply and add apart, as the CPU's unfused arithmetic has them.
    precise float scaled = v * params.gain;
    precise float t = scaled + params.base;
    t = t < params.first ? params.first : (t > params.last ? params.last : t);

    float weight = 0.0;
    if (params.filter == kLinear)
    {
        const float f0 = floor(t);
        precise float w = t - f0;
        if (f0 == params.frame)
        {
            precise float keep = 1.0 - w;
            weight = keep;
        }
        else if (f0 + 1.0 == params.frame)
        {
            weight = w;
        }
    }
    else
    {
        precise float rounded = t + 0.5;
        weight = floor(rounded) == params.frame ? 1.0 : 0.0;
    }
    if (params.firstPass == 0 && weight == 0.0)
    {
        return;
    }

    const bool inside = x >= params.srcX1 && x < params.srcX2 && y >= params.srcY1 && y < params.srcY2;
    const uint dst = params.dstOffset + tid.y * params.dstStride + tid.x * params.nComps;
    const uint src = inside ? params.srcOffset + uint(y - params.srcY1) * params.srcStride + uint(x - params.srcX1) * params.srcComps : 0;
    for (uint c = 0; c < params.nComps; ++c)
    {
        const float p = (inside && c < params.srcComps) ? srcBuf[src + c] : 0.0;
        precise float term = weight * p;
        if (params.firstPass != 0)
        {
            dstBuf[dst + c] = weight == 0.0 ? 0.0 : term;
        }
        else
        {
            precise float sum = dstBuf[dst + c] + term;
            dstBuf[dst + c] = sum;
        }
    }
}
