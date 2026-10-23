// The colour filter array of a raw image as a raw op's parameter block carries it, 16 words from
// `base`. RawLayoutGpu in Engine/Nodes/Raw/RawGpuParams.h mirrors it; colourAt() is
// RawLayout::colourAt().
struct RawLayout
{
    int period;
    int originX;
    int originY;
    uint cells[9];

    [mutating] void load(StructuredBuffer<uint> params, uint base)
    {
        period = int(params[base]);
        originX = int(params[base + 1]);
        originY = int(params[base + 2]);
        [ForceUnroll]
        for (uint i = 0; i < 9; ++i)
            cells[i] = params[base + 4 + i];
    }

    bool hasCfa()
    {
        return period > 0;
    }

    int wrapCell(int v)
    {
        const int m = v % period;
        return m < 0 ? m + period : m;
    }

    // 0 red, 1 green, 2 blue; 3 without a CFA.
    uint colourAt(int x, int y)
    {
        if (period <= 0)
            return 3;
        const uint i = uint(wrapCell(y - originY) * period + wrapCell(x - originX));
        return (cells[i >> 2] >> ((i & 3u) * 8u)) & 0xffu;
    }

    // The cell of the 2x2 block at the origin that photosite (x, y) is on, row by row, as
    // RawPrepare indexes its black levels.
    uint blackCell(int x, int y)
    {
        return uint(((y - originY) & 1) * 2 + ((x - originX) & 1));
    }
}

// std::fmin(a, b): a NaN operand yields the other one, which GLSL min leaves undefined.
float rawFmin(float a, float b)
{
    if (isnan(a))
        return b;
    if (isnan(b))
        return a;
    return a < b ? a : b;
}

// std::fmax(a, b), with the same NaN rule as rawFmin().
float rawFmax(float a, float b)
{
    if (isnan(a))
        return b;
    if (isnan(b))
        return a;
    return a > b ? a : b;
}
