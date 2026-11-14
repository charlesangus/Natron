import UtilityTypes;

// KernelRayGenParams in UtilityKernels.h mirrors this. The frustum is given as world positions on the
// near and far planes: each is the value at the window's centre plus how far it moves per unit of
// normalised device x and y. Perspective and orthographic cameras are both linear in those terms.
//
// A pixel takes samplesPerSide squared samples, one per call, over a square filterWidth pixels wide
// centred on it; sampleIndex says which.
struct RayGenParams
{
    float3 nearAtCentre;
    float filterWidth;
    float3 nearPerX;
    uint sampleIndex;
    float3 nearPerY;
    uint seed;
    float3 farAtCentre;
    uint samplesPerSide;
    float3 farPerX;
    uint filter;
    float3 farPerY;
    uint padding;
    int windowX;
    int windowY;
    int windowWidth;
    int windowHeight;
    int tileX;
    int tileY;
    int tileWidth;
    int tileHeight;
};

// KernelPixelFilter in UtilityKernels.h.
static const uint kFilterBox = 0;
static const uint kFilterTriangle = 1;
static const uint kFilterGaussian = 2;
static const uint kFilterBlackmanHarris = 3;

static const float kPi = 3.14159265358979;

[[vk::binding(0, 0)]] RWStructuredBuffer<float> rays;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> weights;
[[vk::push_constant]] ConstantBuffer<RayGenParams> params;

// Integer only, so the host and the device draw the same samples.
uint pcgHash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float unitFloat(uint bits)
{
    return float(bits >> 8u) * (1.0 / 16777216.0);
}

// The filter's weight at t across its support, 0 and 1 being its edges.
float filterWeight(float t)
{
    const float x = 2.0 * t - 1.0;
    switch (params.filter) {
    case kFilterTriangle:
        return max(0.0, 1.0 - abs(x));
    case kFilterGaussian:
        return exp(-4.5 * x * x);
    case kFilterBlackmanHarris:
        return max(0.0, 0.35875 - 0.48829 * cos(2.0 * kPi * t) + 0.14128 * cos(4.0 * kPi * t) - 0.01168 * cos(6.0 * kPi * t));
    default:
        return 1.0;
    }
}

// Correlated multi-jitter: one sample per cell of the samplesPerSide grid, and one per column and row
// of the grid's samplesPerSide squared subdivisions, so that an edge along either axis is covered to
// within one sample. Each pixel visits the cells in its own order, so a partial pass shows noise
// rather than a pattern.
float2 samplePosition(int pixelX, int pixelY)
{
    const uint n = max(params.samplesPerSide, 1u);
    if (n == 1u) {
        return float2(0.5, 0.5);
    }
    const uint pixelHash = pcgHash(uint(pixelX) ^ pcgHash(uint(pixelY) ^ pcgHash(params.seed)));
    const uint cell = (params.sampleIndex + pixelHash) % (n * n);
    const uint i = cell % n;
    const uint j = cell / n;
    const uint jitterX = pcgHash(pixelHash ^ (params.sampleIndex * 2654435769u));
    const uint jitterY = pcgHash(jitterX);
    return float2((float(i) + (float(j) + unitFloat(jitterX)) / float(n)) / float(n),
                  (float(j) + (float(i) + unitFloat(jitterY)) / float(n)) / float(n));
}

// Rays start on the near plane and end on the far plane, so a hit is clipped to the camera's range
// without a test, and its t over the ray's tMax gives the depth.
[shader("compute")]
[numthreads(64, 1, 1)]
void utilityRayGen(uint3 id : SV_DispatchThreadID)
{
    const uint width = uint(params.tileWidth);
    const uint count = width * uint(params.tileHeight);
    if (id.x >= count)
        return;
    const int pixelX = params.tileX + int(id.x % width);
    const int pixelY = params.tileY + int(id.x / width);
    const float2 t = samplePosition(pixelX, pixelY);
    const float support = max(params.samplesPerSide, 1u) == 1u ? 1.0 : params.filterWidth;
    const float offsetX = (t.x - 0.5) * support;
    const float offsetY = (t.y - 0.5) * support;
    const float ndcX = 2.0 * (float(pixelX - params.windowX) + 0.5 + offsetX) / float(params.windowWidth) - 1.0;
    const float ndcY = 2.0 * (float(pixelY - params.windowY) + 0.5 + offsetY) / float(params.windowHeight) - 1.0;
    const float3 origin = params.nearAtCentre + ndcX * params.nearPerX + ndcY * params.nearPerY;
    const float3 end = params.farAtCentre + ndcX * params.farPerX + ndcY * params.farPerY;
    const float3 span = end - origin;
    const float tMax = length(span);
    const float3 direction = span / tMax;
    weights[id.x] = max(params.samplesPerSide, 1u) == 1u ? 1.0 : filterWeight(t.x) * filterWeight(t.y);
    rays[kRayOriginX * count + id.x] = origin.x;
    rays[kRayOriginY * count + id.x] = origin.y;
    rays[kRayOriginZ * count + id.x] = origin.z;
    rays[kRayDirectionX * count + id.x] = direction.x;
    rays[kRayDirectionY * count + id.x] = direction.y;
    rays[kRayDirectionZ * count + id.x] = direction.z;
    rays[kRayTMin * count + id.x] = 0.0;
    rays[kRayTMax * count + id.x] = tMax;
}
