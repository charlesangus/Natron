RWStructuredBuffer<uint> outBuf;

struct FillParams
{
    uint width;
    uint height;
    // 0 stores x + y*width as a uint, 1 stores it as the bits of a float.
    uint asFloat;
};
[[vk::push_constant]] ConstantBuffer<FillParams> params;

[shader("compute")]
[numthreads(16, 16, 1)]
void fill_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint v = tid.x + tid.y * params.width;
    outBuf[v] = params.asFloat != 0 ? asuint(float(v)) : v;
}
