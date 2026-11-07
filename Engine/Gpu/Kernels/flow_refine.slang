// Variational refinement, as refineFlow() in Engine/Flow/VariationalRefine.cpp. The terms hold iz,
// ix, iy, ixz, iyz, ixx, ixy and iyy per pixel and channel, the system a11, a12, a22, b1, b2 and the
// smoothness per pixel, and the increment du and dv per pixel. FlowRefinePush in
// Engine/Gpu/GpuFlow.cpp mirrors the push block.

StructuredBuffer<float> fromBuf;
StructuredBuffer<float> toBuf;
StructuredBuffer<float> weightBuf;
RWStructuredBuffer<float> flowBuf;
RWStructuredBuffer<float> termsBuf;
RWStructuredBuffer<float> incrementBuf;
RWStructuredBuffer<float> systemBuf;

struct FlowRefineParams
{
    uint fromOffset;
    uint toOffset;
    uint weightOffset;
    uint width;
    uint height;
    uint channels;
    uint hasWeight;
    uint colour;
    float halfDelta;
    float halfGamma;
    float halfAlpha;
    float omega;
};

[[vk::push_constant]] ConstantBuffer<FlowRefineParams> params;

static const float kCodeValueScale = 255.f;
static const float kEpsilonSquared = 1e-6f;
static const float kZetaSquared = 0.01f;
static const uint kTermCount = 8;

float flowU(uint x, uint y)
{
    return flowBuf[(y * params.width + x) * 3];
}

float flowV(uint x, uint y)
{
    return flowBuf[(y * params.width + x) * 3 + 1];
}

float incrementU(uint x, uint y)
{
    return incrementBuf[(y * params.width + x) * 2];
}

float incrementV(uint x, uint y)
{
    return incrementBuf[(y * params.width + x) * 2 + 1];
}

uint termsPixel(uint x, uint y, uint c)
{
    return (y * params.width + x) * params.channels * kTermCount + c * kTermCount;
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
void flow_refine_linearise(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint x = tid.x;
    uint y = tid.y;
    precise float tx = float(x) + flowU(x, y);
    precise float ty = float(y) + flowV(x, y);
    for (uint c = 0; c < params.channels; ++c) {
        uint from = params.fromOffset + (y * params.width + x) * params.channels * 3 + c * 3;
        float value = fromBuf[from];
        float gradX = fromBuf[from + 1];
        float gradY = fromBuf[from + 2];
        float warped = sampleTo(tx, ty, c, 0);
        float warpedGradX = sampleTo(tx, ty, c, 1);
        float warpedGradY = sampleTo(tx, ty, c, 2);
        uint pixel = termsPixel(x, y, c);
        precise float iz = kCodeValueScale * (warped - value);
        precise float ix = kCodeValueScale * 0.5f * (warpedGradX + gradX);
        precise float iy = kCodeValueScale * 0.5f * (warpedGradY + gradY);
        precise float ixz = kCodeValueScale * (warpedGradX - gradX);
        precise float iyz = kCodeValueScale * (warpedGradY - gradY);
        termsBuf[pixel] = iz;
        termsBuf[pixel + 1] = ix;
        termsBuf[pixel + 2] = iy;
        termsBuf[pixel + 3] = ixz;
        termsBuf[pixel + 4] = iyz;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_refine_derivatives(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint x = tid.x;
    uint y = tid.y;
    uint above = (y > 0) ? y - 1 : 0;
    uint below = min(y + 1, params.height - 1);
    uint left = (x > 0) ? x - 1 : 0;
    uint right = min(x + 1, params.width - 1);
    float yScale = (below > above) ? 1.f / float(below - above) : 0.f;
    float xScale = (right > left) ? 1.f / float(right - left) : 0.f;
    for (uint c = 0; c < params.channels; ++c) {
        uint pixel = termsPixel(x, y, c);
        precise float ixx = termsBuf[termsPixel(right, y, c) + 1] - termsBuf[termsPixel(left, y, c) + 1];
        precise float ixy = termsBuf[termsPixel(x, below, c) + 1] - termsBuf[termsPixel(x, above, c) + 1];
        precise float iyy = termsBuf[termsPixel(x, below, c) + 2] - termsBuf[termsPixel(x, above, c) + 2];
        termsBuf[pixel + 5] = xScale * ixx;
        termsBuf[pixel + 6] = yScale * ixy;
        termsBuf[pixel + 7] = yScale * iyy;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_refine_assemble(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint x = tid.x;
    uint y = tid.y;
    uint below = min(y + 1, params.height - 1);
    uint right = min(x + 1, params.width - 1);
    float pixelU = flowU(x, y);
    float pixelV = flowV(x, y);
    float pixelDu = incrementU(x, y);
    float pixelDv = incrementV(x, y);
    precise float a11 = 0.f;
    precise float a12 = 0.f;
    precise float a22 = 0.f;
    precise float b1 = 0.f;
    precise float b2 = 0.f;
    precise float tx = float(x) + pixelU;
    precise float ty = float(y) + pixelV;
    bool inside = (tx >= -0.5f) && (ty >= -0.5f) && (tx <= float(params.width) - 0.5f) && (ty <= float(params.height) - 0.5f);
    float weight = (params.hasWeight != 0) ? weightBuf[params.weightOffset + y * params.width + x] : 1.f;
    float data = weight * (inside ? 1.f : 0.f);
    if (data > 0.f) {
        for (uint c = 0; c < params.channels; ++c) {
            uint pixel = termsPixel(x, y, c);
            float iz = termsBuf[pixel];
            float ix = termsBuf[pixel + 1];
            float iy = termsBuf[pixel + 2];
            float ixz = termsBuf[pixel + 3];
            float iyz = termsBuf[pixel + 4];
            float ixx = termsBuf[pixel + 5];
            float ixy = termsBuf[pixel + 6];
            float iyy = termsBuf[pixel + 7];

            precise float norm = ix * ix + iy * iy + kZetaSquared;
            precise float brightness = iz + ix * pixelDu + iy * pixelDv;
            precise float bw = data * params.halfDelta / sqrt(brightness * brightness / norm + kEpsilonSquared) / norm;
            a11 += bw * ix * ix;
            a12 += bw * ix * iy;
            a22 += bw * iy * iy;
            b1 += bw * iz * ix;
            b2 += bw * iz * iy;

            precise float normX = ixx * ixx + ixy * ixy + kZetaSquared;
            precise float normY = iyy * iyy + ixy * ixy + kZetaSquared;
            precise float gradientX = ixz + ixx * pixelDu + ixy * pixelDv;
            precise float gradientY = iyz + ixy * pixelDu + iyy * pixelDv;
            precise float gw = data * params.halfGamma / sqrt(gradientX * gradientX / normX + gradientY * gradientY / normY + kEpsilonSquared);
            a11 += gw * (ixx * ixx / normX + ixy * ixy / normY);
            a12 += gw * (ixx * ixy / normX + ixy * iyy / normY);
            a22 += gw * (ixy * ixy / normX + iyy * iyy / normY);
            b1 += gw * (ixx * ixz / normX + ixy * iyz / normY);
            b2 += gw * (ixy * ixz / normX + iyy * iyz / normY);
        }
    }
    precise float ux = flowU(right, y) + incrementU(right, y) - pixelU - pixelDu;
    precise float uy = flowU(x, below) + incrementU(x, below) - pixelU - pixelDu;
    precise float vx = flowV(right, y) + incrementV(right, y) - pixelV - pixelDv;
    precise float vy = flowV(x, below) + incrementV(x, below) - pixelV - pixelDv;
    uint system = (y * params.width + x) * 6;
    systemBuf[system] = a11;
    systemBuf[system + 1] = a12;
    systemBuf[system + 2] = a22;
    systemBuf[system + 3] = b1;
    systemBuf[system + 4] = b2;
    systemBuf[system + 5] = params.halfAlpha / sqrt(ux * ux + uy * uy + vx * vx + vy * vy + kEpsilonSquared);
}

float smoothnessAt(uint x, uint y)
{
    return systemBuf[(y * params.width + x) * 6 + 5];
}

// One colour of the red-black checkerboard: a pixel's neighbours are all of the other colour, so
// the pixels of a dispatch share nothing and the CPU's sweep is reproduced exactly.
[shader("compute")]
[numthreads(8, 8, 1)]
void flow_refine_sor(uint3 tid : SV_DispatchThreadID)
{
    uint y = tid.y;
    if (y >= params.height) {
        return;
    }
    uint x = 2 * tid.x + ((y + params.colour) & 1);
    if (x >= params.width) {
        return;
    }
    float u = flowU(x, y);
    float v = flowV(x, y);
    precise float sumWeight = 0.f;
    precise float sumU = 0.f;
    precise float sumV = 0.f;
    if (x > 0) {
        float s = smoothnessAt(x - 1, y);
        sumWeight += s;
        sumU += s * (flowU(x - 1, y) + incrementU(x - 1, y) - u);
        sumV += s * (flowV(x - 1, y) + incrementV(x - 1, y) - v);
    }
    if (x + 1 < params.width) {
        float s = smoothnessAt(x, y);
        sumWeight += s;
        sumU += s * (flowU(x + 1, y) + incrementU(x + 1, y) - u);
        sumV += s * (flowV(x + 1, y) + incrementV(x + 1, y) - v);
    }
    if (y > 0) {
        float s = smoothnessAt(x, y - 1);
        sumWeight += s;
        sumU += s * (flowU(x, y - 1) + incrementU(x, y - 1) - u);
        sumV += s * (flowV(x, y - 1) + incrementV(x, y - 1) - v);
    }
    if (y + 1 < params.height) {
        float s = smoothnessAt(x, y);
        sumWeight += s;
        sumU += s * (flowU(x, y + 1) + incrementU(x, y + 1) - u);
        sumV += s * (flowV(x, y + 1) + incrementV(x, y + 1) - v);
    }
    uint system = (y * params.width + x) * 6;
    uint increment = (y * params.width + x) * 2;
    precise float du = incrementBuf[increment];
    precise float dv = incrementBuf[increment + 1];
    precise float denominatorU = systemBuf[system] + sumWeight;
    if (denominatorU > 0.f) {
        precise float next = (sumU - systemBuf[system + 3] - systemBuf[system + 1] * dv) / denominatorU;
        du += params.omega * (next - du);
    }
    precise float denominatorV = systemBuf[system + 2] + sumWeight;
    if (denominatorV > 0.f) {
        precise float next = (sumV - systemBuf[system + 4] - systemBuf[system + 1] * du) / denominatorV;
        dv += params.omega * (next - dv);
    }
    incrementBuf[increment] = du;
    incrementBuf[increment + 1] = dv;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_refine_apply(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint flow = (tid.y * params.width + tid.x) * 3;
    uint increment = (tid.y * params.width + tid.x) * 2;
    flowBuf[flow] += incrementBuf[increment];
    flowBuf[flow + 1] += incrementBuf[increment + 1];
}
