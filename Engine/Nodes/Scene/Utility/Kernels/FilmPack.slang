// KernelFilmPackParams in UtilityRenderBuffer.cpp mirrors this. It writes a window of the film as
// HydraRender::PlaneWindow lays out a colour window: one component is alpha, more are rgba in order,
// and a pixel past the film is zero.
struct FilmPackParams
{
    uint filmWidth;
    uint filmHeight;
    int x1;
    int y1;
    uint width;
    uint height;
    uint nComps;
    uint outOffset;
    uint outRowStride;
    uint padding0;
    uint padding1;
    uint padding2;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint4> film;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> pixels;
[[vk::push_constant]] ConstantBuffer<FilmPackParams> params;

[shader("compute")]
[numthreads(8, 8, 1)]
void utilityFilmPack(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.width || id.y >= params.height)
        return;

    const int fx = params.x1 + int(id.x);
    const int fy = params.y1 + int(id.y);
    const bool inside = fx >= 0 && fy >= 0 && uint(fx) < params.filmWidth && uint(fy) < params.filmHeight;
    const uint4 pixel = inside ? film[uint(fy) * params.filmWidth + uint(fx)] : uint4(0, 0, 0, 0);
    const uint base = params.outOffset + id.y * params.outRowStride + id.x * params.nComps;

    for (uint c = 0; c < params.nComps; ++c) {
        const uint channel = params.nComps == 1 ? 3 : c;

        pixels[base + c] = channel < 4 ? pixel[channel] : 0;
    }
}
