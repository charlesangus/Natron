// What FlowConfidence, FlowTemporal and FlowEstimate do around the solvers, and the node's output.
// A field holds u, v and confidence per pixel. FlowConfidencePush in Engine/Gpu/GpuFlow.cpp mirrors
// the push block.

StructuredBuffer<float> fieldA;
StructuredBuffer<float> fieldB;
StructuredBuffer<float> fieldC;
StructuredBuffer<float> fieldD;
StructuredBuffer<float> fieldE;
RWStructuredBuffer<float> dstBuf;
RWStructuredBuffer<uint> flagsBuf;

struct FlowConfidenceParams
{
    uint width;
    uint height;
    uint neighbours;
    uint mode;
    float scale;
    float strength;
    uint srcOffset;
    uint srcRowStride;
    uint srcPixelStride;
    int srcX0;
    int srcY0;
    int srcWidth;
    int srcHeight;
    uint dstOffset;
    uint dstRowStride;
    uint dstComps;
    int outX0;
    int outY0;
    uint hasA;
    uint hasB;
    uint firstRow;
};

[[vk::push_constant]] ConstantBuffer<FlowConfidenceParams> params;

// Keeps a pixel no field is confident about at its own vector, rather than dividing by zero.
static const float kMinOwnWeight = 1e-3f;
static const uint kMatteAboveZero = 1;
static const uint kMatteBelowOne = 2;
static const uint kModeVectors = 0;
static const uint kWeightInside = 0;

bool isFinite(float value)
{
    return (asuint(value) & 0x7f800000u) != 0x7f800000u;
}

bool matteSeparates()
{
    uint flags = flagsBuf[0];

    return (flags & (kMatteAboveZero | kMatteBelowOne)) == (kMatteAboveZero | kMatteBelowOne);
}

float sampleA(float x, float y, uint component)
{
    x = clamp(x, 0.f, float(params.width - 1));
    y = clamp(y, 0.f, float(params.height - 1));
    uint x0 = min(uint(x), params.width - 1);
    uint y0 = min(uint(y), params.height - 1);
    uint x1 = min(x0 + 1, params.width - 1);
    uint y1 = min(y0 + 1, params.height - 1);
    precise float fx = x - float(x0);
    precise float fy = y - float(y0);
    float p = fieldA[(y0 * params.width + x0) * 3 + component];
    float q = fieldA[(y0 * params.width + x1) * 3 + component];
    float r = fieldA[(y1 * params.width + x0) * 3 + component];
    float s = fieldA[(y1 * params.width + x1) * 3 + component];
    precise float top = p + fx * (q - p);
    precise float bottom = r + fx * (s - r);

    return top + fy * (bottom - top);
}

float sampleC(float x, float y, uint component)
{
    x = clamp(x, 0.f, float(params.width - 1));
    y = clamp(y, 0.f, float(params.height - 1));
    uint x0 = min(uint(x), params.width - 1);
    uint y0 = min(uint(y), params.height - 1);
    uint x1 = min(x0 + 1, params.width - 1);
    uint y1 = min(y0 + 1, params.height - 1);
    precise float fx = x - float(x0);
    precise float fy = y - float(y0);
    float p = fieldC[(y0 * params.width + x0) * 3 + component];
    float q = fieldC[(y0 * params.width + x1) * 3 + component];
    float r = fieldC[(y1 * params.width + x0) * 3 + component];
    float s = fieldC[(y1 * params.width + x1) * 3 + component];
    precise float top = p + fx * (q - p);
    precise float bottom = r + fx * (s - r);

    return top + fy * (bottom - top);
}

bool staysInside(float tx, float ty)
{
    return (tx >= -0.5f) && (ty >= -0.5f) && (tx <= float(params.width) - 0.5f) && (ty <= float(params.height) - 0.5f);
}

// fieldA is the flow back, dstBuf the flow there whose confidence this writes.
[shader("compute")]
[numthreads(8, 8, 1)]
void flow_confidence(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint pixel = (tid.y * params.width + tid.x) * 3;
    float u = dstBuf[pixel];
    float v = dstBuf[pixel + 1];
    precise float tx = float(tid.x) + u;
    precise float ty = float(tid.y) + v;
    if (!staysInside(tx, ty)) {
        dstBuf[pixel + 2] = 0.f;

        return;
    }
    precise float du = u + sampleA(tx, ty, 0);
    precise float dv = v + sampleA(tx, ty, 1);
    dstBuf[pixel + 2] = exp(-(du * du + dv * dv) * params.scale);
}

// fieldE is the current field; fieldA carried through fieldB is the first neighbour and fieldC
// through fieldD the second.
[shader("compute")]
[numthreads(8, 8, 1)]
void flow_temporal(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint pixel = (tid.y * params.width + tid.x) * 3;
    float currentConfidence = fieldE[pixel + 2];
    float own = max(currentConfidence, kMinOwnWeight);
    precise float sumWeight = own;
    precise float sumU = own * fieldE[pixel];
    precise float sumV = own * fieldE[pixel + 1];
    if (params.neighbours > 0) {
        precise float tx = float(tid.x) + fieldB[pixel];
        precise float ty = float(tid.y) + fieldB[pixel + 1];
        float carried = staysInside(tx, ty) ? sampleA(tx, ty, 2) : 0.f;
        precise float weight = params.strength * carried * fieldB[pixel + 2];
        sumWeight += weight;
        sumU += weight * sampleA(tx, ty, 0);
        sumV += weight * sampleA(tx, ty, 1);
    }
    if (params.neighbours > 1) {
        precise float tx = float(tid.x) + fieldD[pixel];
        precise float ty = float(tid.y) + fieldD[pixel + 1];
        float carried = staysInside(tx, ty) ? sampleC(tx, ty, 2) : 0.f;
        precise float weight = params.strength * carried * fieldD[pixel + 2];
        sumWeight += weight;
        sumU += weight * sampleC(tx, ty, 0);
        sumV += weight * sampleC(tx, ty, 1);
    }
    dstBuf[pixel] = sumU / sumWeight;
    dstBuf[pixel + 1] = sumV / sumWeight;
    dstBuf[pixel + 2] = currentConfidence;
}

// fieldA is the source and srcOffset points at the matte's channel of its first pixel.
[shader("compute")]
[numthreads(8, 8, 1)]
void flow_matte_extract(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    int x = int(tid.x);
    int y = int(tid.y);
    float value = 0.f;
    if ((x >= params.srcX0) && (y >= params.srcY0) && (x < params.srcX0 + params.srcWidth) && (y < params.srcY0 + params.srcHeight)) {
        value = fieldA[params.srcOffset + uint(y - params.srcY0) * params.srcRowStride + uint(x - params.srcX0) * params.srcPixelStride];
    }
    value = isFinite(value) ? clamp(value, 0.f, 1.f) : 0.f;
    dstBuf[tid.y * params.width + tid.x] = value;
    uint flags = ((value > 0.f) ? kMatteAboveZero : 0) | ((value < 1.f) ? kMatteBelowOne : 0);
    if (flags != 0) {
        uint previous;
        InterlockedOr(flagsBuf[0], flags, previous);
    }
}

// A matte that separates nothing weighs every pixel 1, which is what the solvers do without one.
[shader("compute")]
[numthreads(8, 8, 1)]
void flow_matte_weight(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint pixel = tid.y * params.width + tid.x;
    float matte = fieldA[params.srcOffset + pixel];
    float weight = 1.f;
    if (matteSeparates()) {
        weight = (params.mode == kWeightInside) ? matte : 1.f - matte;
    }
    dstBuf[params.dstOffset + pixel] = weight;
}

// fieldA is inside, fieldB outside and fieldC the matte. A matte that separates nothing leaves the
// outside field, whose weights were all 1.
[shader("compute")]
[numthreads(8, 8, 1)]
void flow_composite(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= params.width) || (tid.y >= params.height)) {
        return;
    }
    uint plane = tid.y * params.width + tid.x;
    uint pixel = plane * 3;
    float matte = matteSeparates() ? fieldC[plane] : 0.f;
    for (uint component = 0; component < 3; ++component) {
        float inside = fieldA[pixel + component];
        float outside = fieldB[pixel + component];
        precise float mixed = outside + matte * (inside - outside);
        dstBuf[pixel + component] = mixed;
    }
}

// Writes a plane over a rect of the output view: the vectors of fieldA, or the confidence of fieldA
// then of fieldB. srcWidth and srcHeight are the rect's size, outX0 and outY0 its position in the
// solved window. What lies outside the window or has no field is 0.
[shader("compute")]
[numthreads(8, 8, 1)]
void flow_output(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint3 tid = uint3(dispatchId.x, dispatchId.y + params.firstRow, dispatchId.z);
    if ((tid.x >= uint(params.srcWidth)) || (tid.y >= uint(params.srcHeight))) {
        return;
    }
    int x = params.outX0 + int(tid.x);
    int y = params.outY0 + int(tid.y);
    float first = 0.f;
    float second = 0.f;
    if ((x >= 0) && (y >= 0) && (x < int(params.width)) && (y < int(params.height))) {
        uint pixel = (uint(y) * params.width + uint(x)) * 3;
        if (params.mode == kModeVectors) {
            if (params.hasA != 0) {
                first = fieldA[pixel];
                second = fieldA[pixel + 1];
            }
        } else {
            first = (params.hasA != 0) ? fieldA[pixel + 2] : 0.f;
            second = (params.hasB != 0) ? fieldB[pixel + 2] : 0.f;
        }
    }
    uint dst = params.dstOffset + tid.y * params.dstRowStride + tid.x * params.dstComps;
    for (uint c = 0; c < params.dstComps; ++c) {
        dstBuf[dst + c] = (c == 0) ? first : ((c == 1) ? second : 0.f);
    }
}
