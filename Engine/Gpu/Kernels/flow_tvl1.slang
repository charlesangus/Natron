// TV-L1 flow, as computeTvL1Flow() in Engine/Flow/TvL1Flow.cpp. The terms hold gradX, gradY,
// gradSquared and rho per pixel and channel, the solution its two components per pixel, and the
// dual pxU, pyU, pxV and pyV per pixel. FlowTvL1Push in Engine/Gpu/GpuFlow.cpp mirrors the push
// block.

StructuredBuffer<float> fromBuf;
StructuredBuffer<float> toBuf;
StructuredBuffer<float> weightBuf;
RWStructuredBuffer<float> flowBuf;
RWStructuredBuffer<float> termsBuf;
RWStructuredBuffer<float> solutionBuf;
RWStructuredBuffer<float> dualBuf;
RWStructuredBuffer<float> dstBuf;

struct FlowTvL1Params
{
    uint fromOffset;
    uint toOffset;
    uint weightOffset;
    uint width;
    uint height;
    uint channels;
    uint hasWeight;
    float lambdaTheta;
    float theta;
    float dualStep;
    uint firstRow;
};

[[vk::push_constant]] ConstantBuffer<FlowTvL1Params> params;

static const float kCodeValueScale = 255.f;
static const float kMinGradientSquared = 1e-6f;
static const int kMedianRadius = 2;
static const uint kMedianSamples = 25;

float flowComponent(uint x, uint y, uint component)
{
    return flowBuf[(y * params.width + x) * 3 + component];
}

float toComponent(uint x, uint y, uint c, uint component)
{
    return toBuf[params.toOffset + (y * params.width + x) * params.channels * 3 + c * 3 + component];
}

float sampleTo(float x, float y, uint c, uint component)
{
    x = clamp(x, 0.f, float(params.width - 1));
    y = clamp(y, 0.f, float(params.height - 1));
    uint x0 = min(uint(x), params.width - 1);
    uint y0 = min(uint(y), params.height - 1);
    uint x1 = min(x0 + 1, params.width - 1);
    uint y1 = min(y0 + 1, params.height - 1);
    precise float fx = x - float(x0);
    precise float fy = y - float(y0);
    float p = toComponent(x0, y0, c, component);
    float q = toComponent(x1, y0, c, component);
    float r = toComponent(x0, y1, c, component);
    float s = toComponent(x1, y1, c, component);
    precise float top = p + fx * (q - p);
    precise float bottom = r + fx * (s - r);

    return top + fy * (bottom - top);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_tvl1_linearise(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint x = tid.x;
    uint y = tid.y;
    float u = flowComponent(x, y, 0);
    float v = flowComponent(x, y, 1);
    precise float tx = float(x) + u;
    precise float ty = float(y) + v;
    for (uint c = 0; c < params.channels; ++c) {
        float value = fromBuf[params.fromOffset + (y * params.width + x) * params.channels * 3 + c * 3];
        precise float gx = kCodeValueScale * sampleTo(tx, ty, c, 1);
        precise float gy = kCodeValueScale * sampleTo(tx, ty, c, 2);
        precise float gradSquared = gx * gx + gy * gy;
        precise float rho = kCodeValueScale * (sampleTo(tx, ty, c, 0) - value) - gx * u - gy * v;
        uint pixel = (y * params.width + x) * params.channels * 4 + c * 4;
        termsBuf[pixel] = gx;
        termsBuf[pixel + 1] = gy;
        termsBuf[pixel + 2] = gradSquared;
        termsBuf[pixel + 3] = rho;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_tvl1_threshold(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint x = tid.x;
    uint y = tid.y;
    precise float vx = flowComponent(x, y, 0);
    precise float vy = flowComponent(x, y, 1);
    float weight = (params.hasWeight != 0) ? weightBuf[params.weightOffset + y * params.width + x] : 1.f;
    precise float threshold = params.lambdaTheta * weight;
    for (uint c = 0; c < params.channels; ++c) {
        uint pixel = (y * params.width + x) * params.channels * 4 + c * 4;
        float gradSquared = termsBuf[pixel + 2];
        if (gradSquared < kMinGradientSquared) {
            continue;
        }
        float gx = termsBuf[pixel];
        float gy = termsBuf[pixel + 1];
        precise float rho = termsBuf[pixel + 3] + gx * vx + gy * vy;
        precise float limit = threshold * gradSquared;
        float scale;
        if (rho < -limit) {
            scale = threshold;
        } else if (rho > limit) {
            scale = -threshold;
        } else {
            scale = -rho / gradSquared;
        }
        vx += scale * gx;
        vy += scale * gy;
    }
    uint solution = (y * params.width + x) * 2;
    solutionBuf[solution] = vx;
    solutionBuf[solution + 1] = vy;
}

float divergence(uint x, uint y, uint component)
{
    uint w = params.width;
    uint h = params.height;
    uint first = component * 2;
    float px = (x + 1 < w) ? dualBuf[(y * w + x) * 4 + first] : 0.f;
    float pxBefore = (x > 0) ? dualBuf[(y * w + x - 1) * 4 + first] : 0.f;
    float py = (y + 1 < h) ? dualBuf[(y * w + x) * 4 + first + 1] : 0.f;
    float pyBefore = (y > 0) ? dualBuf[((y - 1) * w + x) * 4 + first + 1] : 0.f;
    precise float dx = px - pxBefore;
    precise float dy = py - pyBefore;

    return dx + dy;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_tvl1_primal(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint x = tid.x;
    uint y = tid.y;
    uint solution = (y * params.width + x) * 2;
    uint flow = (y * params.width + x) * 3;
    precise float u = solutionBuf[solution] + params.theta * divergence(x, y, 0);
    precise float v = solutionBuf[solution + 1] + params.theta * divergence(x, y, 1);
    flowBuf[flow] = u;
    flowBuf[flow + 1] = v;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_tvl1_dual(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint x = tid.x;
    uint y = tid.y;
    uint dual = (y * params.width + x) * 4;
    for (uint component = 0; component < 2; ++component) {
        float here = flowComponent(x, y, component);
        precise float gx = (x + 1 < params.width) ? flowComponent(x + 1, y, component) - here : 0.f;
        precise float gy = (y + 1 < params.height) ? flowComponent(x, y + 1, component) - here : 0.f;
        precise float norm = 1.f + params.dualStep * sqrt(gx * gx + gy * gy);
        uint first = dual + component * 2;
        precise float px = (dualBuf[first] + params.dualStep * gx) / norm;
        precise float py = (dualBuf[first + 1] + params.dualStep * gy) / norm;
        dualBuf[first] = px;
        dualBuf[first + 1] = py;
    }
}

float medianOf(uint x, uint y, uint component)
{
    float window[kMedianSamples];
    uint count = 0;
    int x0 = max(int(x) - kMedianRadius, 0);
    int x1 = min(int(x) + kMedianRadius, int(params.width) - 1);
    int y0 = max(int(y) - kMedianRadius, 0);
    int y1 = min(int(y) + kMedianRadius, int(params.height) - 1);
    for (int ny = y0; ny <= y1; ++ny) {
        for (int nx = x0; nx <= x1; ++nx) {
            float value = flowComponent(uint(nx), uint(ny), component);
            uint slot = count;
            while ((slot > 0) && (window[slot - 1] > value)) {
                window[slot] = window[slot - 1];
                --slot;
            }
            window[slot] = value;
            ++count;
        }
    }

    return window[count / 2];
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_tvl1_median(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint pixel = (tid.y * params.width + tid.x) * 3;
    dstBuf[pixel] = medianOf(tid.x, tid.y, 0);
    dstBuf[pixel + 1] = medianOf(tid.x, tid.y, 1);
    dstBuf[pixel + 2] = 0.f;
}
