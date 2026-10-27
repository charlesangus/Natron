# Bloom spike: an adaptive downsample pyramid against a sum of full-resolution blurs, on the CPU
# (quality, seams, timing) and on the GPU (kernel timing). Standalone like tools/gpu-spike;
# nothing here is linked into Natron.
#
# In the natron-dev container, from the repository root:
#   cmake -S tools/spikes/bloom -B build/spikes/bloom-build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/spikes/bloom-build
# CPU spike only, anywhere:
#   build/spikes/bloom-build/BloomSpikeBench --cpu-only
# Both spikes on the host GPU:
#   GPU_SPIKE_BUILD=build/spikes/bloom-build tools/gpu-spike/run-host.sh BloomSpikeBench

cmake_minimum_required(VERSION 3.22)
project(natron_bloom_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATRON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

include("${NATRON_ROOT}/cmake/FetchSlang.cmake")
include("${NATRON_ROOT}/cmake/SlangKernels.cmake")

add_subdirectory("${NATRON_ROOT}/Engine/Gpu" natron_gpu)

# One entry point per file: slangc reflects every global of the module, so two kernels with
# different push-constant blocks cannot share one.
slang_add_kernel(bloom_down kernels/down.slang ENTRY down_main)
slang_add_kernel(bloom_up kernels/up.slang ENTRY up_main)

# The engine's line filters, compiled from source against stand-ins for the two Natron headers
# they include, as tools/gpu-spike does, with NATRON_HAVE_GPU for toGpuLineFilter().
set(BLOOM_SHIM "${CMAKE_CURRENT_BINARY_DIR}/shim")
file(WRITE "${BLOOM_SHIM}/Python.h" "")
file(WRITE "${BLOOM_SHIM}/Global/Macros.h"
"#pragma once\n#define NATRON_NAMESPACE_ENTER namespace Natron {\n#define NATRON_NAMESPACE_EXIT }\n")
add_library(bloom_blur_kernels STATIC ${NATRON_ROOT}/Engine/Nodes/Filter/BlurKernels.cpp)
target_include_directories(bloom_blur_kernels PUBLIC "${BLOOM_SHIM}" "${NATRON_ROOT}"
  "${NATRON_ROOT}/Engine/Nodes/Filter")
target_compile_definitions(bloom_blur_kernels PUBLIC NATRON_HAVE_GPU)
target_compile_options(bloom_blur_kernels PRIVATE -O2 -DNDEBUG -march=x86-64-v3)
target_link_libraries(bloom_blur_kernels PUBLIC NatronGpu)

add_executable(BloomSpikeBench main.cpp pyramid.cpp cpu_spike.cpp gpu_spike.cpp)
target_include_directories(BloomSpikeBench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(BloomSpikeBench PRIVATE -O2 -DNDEBUG -march=x86-64-v3 -ffp-contract=off -fno-fast-math)
target_link_libraries(BloomSpikeBench PRIVATE bloom_blur_kernels NatronGpu bloom_down bloom_up
  Threads::Threads -static-libstdc++ -static-libgcc)
