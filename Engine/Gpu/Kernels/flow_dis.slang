// Dense inverse search, as computeDisFlow() in Engine/Flow/DisFlow.cpp. A pyramid level holds
// value, gradX and gradY per channel, a flow field u, v and confidence per pixel, and the patch grid
// u and v per patch. FlowDisPush in Engine/Gpu/GpuFlow.cpp mirrors the push block.

StructuredBuffer<float> fromBuf;
StructuredBuffer<float> toBuf;
StructuredBuffer<float> weightBuf;
RWStructuredBuffer<float> flowBuf;
RWStructuredBuffer<float> patchBuf;
StructuredBuffer<float> coarseBuf;

struct FlowDisParams
{
    uint fromOffset;
    uint toOffset;
    uint weightOffset;
    uint flowOffset;
    uint patchOffset;
    uint coarseOffset;
    uint width;
    uint height;
    uint coarseWidth;
    uint coarseHeight;
    uint channels;
    uint hasWeight;
    uint patchSize;
    uint patchStride;
    uint iterations;
    uint patchesX;
    uint patchesY;
    uint lastX;
    uint lastY;
    uint firstRow;
};

[[vk::push_constant]] ConstantBuffer<FlowDisParams> params;

// The paper's error floor of 1 is on 8-bit code values, and these images are about 0..1.
static const float kPhotometricFloorSquared = (1.f / 255.f) * (1.f / 255.f);
static const float kHessianDamping = 1e-3f;
static const uint kMaxChannels = 3;

float weightAt(uint x, uint y)
{
    return (params.hasWeight != 0) ? weightBuf[params.weightOffset + y * params.width + x] : 1.f;
}

uint fromPixel(uint x, uint y, uint c)
{
    return params.fromOffset + (y * params.width + x) * params.channels * 3 + c * 3;
}

float toValue(int x, int y, uint c)
{
    return toBuf[params.toOffset + (uint(y) * params.width + uint(x)) * params.channels * 3 + c * 3];
}

float flowComponent(uint x, uint y, uint component)
{
    return flowBuf[params.flowOffset + (y * params.width + x) * 3 + component];
}

float sampleFlow(float x, float y, uint component)
{
    float maxX = float(params.width - 1);
    float maxY = float(params.height - 1);
    x = clamp(x, 0.f, maxX);
    y = clamp(y, 0.f, maxY);
    uint x0 = min(uint(x), params.width - 1);
    uint y0 = min(uint(y), params.height - 1);
    uint x1 = min(x0 + 1, params.width - 1);
    uint y1 = min(y0 + 1, params.height - 1);
    precise float fx = x - float(x0);
    precise float fy = y - float(y0);
    float a = flowComponent(x0, y0, component);
    float b = flowComponent(x1, y0, component);
    float c = flowComponent(x0, y1, component);
    float d = flowComponent(x1, y1, component);
    precise float top = a + fx * (b - a);
    precise float bottom = c + fx * (d - c);

    return top + fy * (bottom - top);
}

float sampleTo(float x, float y, uint c)
{
    float maxX = float(params.width - 1);
    float maxY = float(params.height - 1);
    x = clamp(x, 0.f, maxX);
    y = clamp(y, 0.f, maxY);
    int x0 = clamp(int(x), 0, int(params.width) - 1);
    int y0 = clamp(int(y), 0, int(params.height) - 1);
    int x1 = min(x0 + 1, int(params.width) - 1);
    int y1 = min(y0 + 1, int(params.height) - 1);
    precise float fx = x - float(x0);
    precise float fy = y - float(y0);
    float p = toValue(x0, y0, c);
    float q = toValue(x1, y0, c);
    float r = toValue(x0, y1, c);
    float s = toValue(x1, y1, c);
    precise float top = p + fx * (q - p);
    precise float bottom = r + fx * (s - r);

    return top + fy * (bottom - top);
}

void searchPatch(uint originX, uint originY, inout float u, inout float v)
{
    uint size = params.patchSize;
    precise float weightSum = 0.f;
    if (params.hasWeight != 0) {
        for (uint y = originY; y < originY + size; ++y) {
            for (uint x = originX; x < originX + size; ++x) {
                weightSum += weightAt(x, y);
            }
        }
    } else {
        weightSum = float(size * size);
    }
    if (weightSum <= 0.f) {
        return;
    }
    float meanGradX[kMaxChannels];
    float meanGradY[kMaxChannels];
    precise float hxx = 0.f;
    precise float hxy = 0.f;
    precise float hyy = 0.f;
    for (uint c = 0; c < params.channels; ++c) {
        precise float sumX = 0.f;
        precise float sumY = 0.f;
        for (uint y = originY; y < originY + size; ++y) {
            for (uint x = originX; x < originX + size; ++x) {
                uint pixel = fromPixel(x, y, c);
                float gx = fromBuf[pixel + 1];
                float gy = fromBuf[pixel + 2];
                float w = weightAt(x, y);
                precise float wgx = w * gx;
                precise float wgy = w * gy;
                sumX += wgx;
                sumY += wgy;
                hxx += wgx * gx;
                hxy += wgx * gy;
                hyy += wgy * gy;
            }
        }
        meanGradX[c] = sumX / weightSum;
        meanGradY[c] = sumY / weightSum;
        hxx -= sumX * meanGradX[c];
        hxy -= sumX * meanGradY[c];
        hyy -= sumY * meanGradY[c];
    }
    precise float damping = kHessianDamping * (hxx + hyy);
    hxx += damping;
    hyy += damping;
    precise float det = hxx * hyy - hxy * hxy;
    if (!(det > 0.f)) {
        return;
    }

    float initU = u;
    float initV = v;
    float limitSquared = float(size * size);
    int lastX = int(params.width) - 1;
    int lastY = int(params.height) - 1;
    for (uint iteration = 0; iteration < params.iterations; ++iteration) {
        float flooredU = floor(u);
        float flooredV = floor(v);
        int offsetX = int(flooredU);
        int offsetY = int(flooredV);
        precise float fx = u - flooredU;
        precise float fy = v - flooredV;
        precise float bx = 0.f;
        precise float by = 0.f;
        for (uint c = 0; c < params.channels; ++c) {
            precise float errorSum = 0.f;
            for (uint y = originY; y < originY + size; ++y) {
                int r0 = clamp(int(y) + offsetY, 0, lastY);
                int r1 = clamp(int(y) + offsetY + 1, 0, lastY);
                for (uint x = originX; x < originX + size; ++x) {
                    int x0 = clamp(int(x) + offsetX, 0, lastX);
                    int x1 = clamp(int(x) + offsetX + 1, 0, lastX);
                    float p = toValue(x0, r0, c);
                    float q = toValue(x1, r0, c);
                    float r = toValue(x0, r1, c);
                    float s = toValue(x1, r1, c);
                    precise float top = p + fx * (q - p);
                    precise float bottom = r + fx * (s - r);
                    uint pixel = fromPixel(x, y, c);
                    precise float error = weightAt(x, y) * (top + fy * (bottom - top) - fromBuf[pixel]);
                    errorSum += error;
                    bx += error * fromBuf[pixel + 1];
                    by += error * fromBuf[pixel + 2];
                }
            }
            bx -= errorSum * meanGradX[c];
            by -= errorSum * meanGradY[c];
        }
        precise float stepU = (hyy * bx - hxy * by) / det;
        precise float stepV = (hxx * by - hxy * bx) / det;
        u -= stepU;
        v -= stepV;
        precise float du = u - initU;
        precise float dv = v - initV;
        if (!(du * du + dv * dv <= limitSquared)) {
            u = initU;
            v = initV;

            return;
        }
    }
}

uint originOf(uint index, uint last)
{
    return min(index * params.patchStride, last);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_dis_search(uint3 tid : SV_DispatchThreadID)
{
    uint row = tid.y + params.firstRow;
    if ((tid.x >= params.patchesX) || (row >= params.patchesY)) {
        return;
    }
    uint originX = originOf(tid.x, params.lastX);
    uint originY = originOf(row, params.lastY);
    float centre = 0.5f * float(params.patchSize - 1);
    float u = sampleFlow(float(originX) + centre, float(originY) + centre, 0);
    float v = sampleFlow(float(originX) + centre, float(originY) + centre, 1);
    searchPatch(originX, originY, u, v);
    uint index = params.patchOffset + (row * params.patchesX + tid.x) * 2;
    patchBuf[index] = u;
    patchBuf[index + 1] = v;
}

// The index of the first patch reaching position, in the order patches are numbered.
uint firstPatchOver(uint position)
{
    if (position + 1 <= params.patchSize) {
        return 0;
    }

    return (position + 1 - params.patchSize + params.patchStride - 1) / params.patchStride;
}

// Patches are visited in the order of the CPU's rows of patches, so the sums round as its do.
[shader("compute")]
[numthreads(8, 8, 1)]
void flow_dis_densify(uint3 tid : SV_DispatchThreadID)
{
    uint y = tid.y + params.firstRow;
    if ((tid.x >= params.width) || (y >= params.height)) {
        return;
    }
    uint x = tid.x;
    uint size = params.patchSize;
    float channelScale = 1.f / float(params.channels);
    float pixelWeight = weightAt(x, y);
    precise float sumU = 0.f;
    precise float sumV = 0.f;
    precise float sumWeight = 0.f;
    for (uint iy = firstPatchOver(y); iy < params.patchesY; ++iy) {
        uint originY = originOf(iy, params.lastY);
        if (originY > y) {
            break;
        }
        for (uint ix = firstPatchOver(x); ix < params.patchesX; ++ix) {
            uint originX = originOf(ix, params.lastX);
            if (originX > x) {
                break;
            }
            uint index = params.patchOffset + (iy * params.patchesX + ix) * 2;
            float u = patchBuf[index];
            float v = patchBuf[index + 1];
            precise float errorSquared = 0.f;
            for (uint c = 0; c < params.channels; ++c) {
                precise float tx = float(x) + u;
                precise float ty = float(y) + v;
                precise float d = sampleTo(tx, ty, c) - fromBuf[fromPixel(x, y, c)];
                errorSquared += d * d;
            }
            precise float w = pixelWeight / max(kPhotometricFloorSquared, channelScale * errorSquared);
            sumU += w * u;
            sumV += w * v;
            sumWeight += w;
        }
    }
    if (sumWeight > 0.f) {
        uint pixel = params.flowOffset + (y * params.width + x) * 3;
        flowBuf[pixel] = sumU / sumWeight;
        flowBuf[pixel + 1] = sumV / sumWeight;
    }
}

float coarseComponent(uint x, uint y, uint component)
{
    return coarseBuf[params.coarseOffset + (y * params.coarseWidth + x) * 3 + component];
}

float sampleCoarse(float x, float y, uint component)
{
    x = clamp(x, 0.f, float(params.coarseWidth - 1));
    y = clamp(y, 0.f, float(params.coarseHeight - 1));
    uint x0 = min(uint(x), params.coarseWidth - 1);
    uint y0 = min(uint(y), params.coarseHeight - 1);
    uint x1 = min(x0 + 1, params.coarseWidth - 1);
    uint y1 = min(y0 + 1, params.coarseHeight - 1);
    precise float fx = x - float(x0);
    precise float fy = y - float(y0);
    float a = coarseComponent(x0, y0, component);
    float b = coarseComponent(x1, y0, component);
    float c = coarseComponent(x0, y1, component);
    float d = coarseComponent(x1, y1, component);
    precise float top = a + fx * (b - a);
    precise float bottom = c + fx * (d - c);

    return top + fy * (bottom - top);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flow_upsample(uint3 tid : SV_DispatchThreadID)
{
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    float cx = 0.5f * float(tid.x);
    float cy = 0.5f * float(tid.y);
    uint pixel = params.flowOffset + (tid.y * params.width + tid.x) * 3;
    flowBuf[pixel] = 2.f * sampleCoarse(cx, cy, 0);
    flowBuf[pixel + 1] = 2.f * sampleCoarse(cx, cy, 1);
    flowBuf[pixel + 2] = 0.f;
}
