#include "builtins.h"

extern "C" __attribute__((noinline)) std::int32_t
nk_nearpoint(const NearGrid* grid, float x, float y, float z)
{
    return gkNearPoint(grid, x, y, z);
}

extern "C" float
nk_noise3(float x, float y, float z)
{
    return gkNoise3(x, y, z);
}
