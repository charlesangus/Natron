// Compiled to LLVM bitcode with clang and linked into every JIT snippet module.

#include "builtins.h"

extern "C" float
nk_noise3(float x, float y, float z)
{
    return gkNoise3(x, y, z);
}
