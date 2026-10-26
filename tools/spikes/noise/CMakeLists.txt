# Noise library spike: a plain C++ port of FastNoise2's 4D Perlin, SuperSimplex, Cellular F1 and
# FBm (fn2port/, with a Slang mirror for the GPU) against FastNoise2 itself built strict and relaxed
# (fn2lib/) and FastNoiseLite's scalar 3D noise (fnl/) as the floor. Standalone like
# tools/gpu-spike; nothing here is linked into Natron. See README.md for build and run commands.

cmake_minimum_required(VERSION 3.22)
project(natron_noise_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# -O2 rather than CMake's Release -O3, matching the optimisation level Natron's kernels see.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

set(NATRON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

include("${NATRON_ROOT}/cmake/FetchSlang.cmake")
include("${NATRON_ROOT}/cmake/SlangKernels.cmake")

add_subdirectory("${NATRON_ROOT}/Engine/Gpu" natron_gpu)

# As slang_add_kernel, plus -fp-mode precise (no contraction, so the unfused kernel matches the
# unfused CPU build) and extra -D defines.
function(noise_spike_kernel target source entry)
  get_filename_component(_src "${source}" ABSOLUTE)
  get_filename_component(_srcdir "${_src}" DIRECTORY)
  file(GLOB _deps CONFIGURE_DEPENDS "${_srcdir}/*.slang")
  set(_defs "")
  foreach(_d ${ARGN})
    list(APPEND _defs "-D${_d}")
  endforeach()
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/${target}")
  set(_spv "${_gen}/${target}.spv")
  set(_hdr "${_gen}/${target}_spirv.h")
  set(_json "${_gen}/${target}.reflection.json")
  add_custom_command(
    OUTPUT "${_hdr}" "${_json}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
    COMMAND "${SLANG_COMPILER}" "${_src}" ${_defs} -entry ${entry} -stage compute
            -target spirv -profile spirv_1_5 -fp-mode precise -o "${_spv}"
            -reflection-json "${_json}"
    COMMAND ${CMAKE_COMMAND} -DIN=${_spv} -DOUT=${_hdr} -DSYMBOL=${target} -DJSON=${_json}
            -DSTRICT=ON -P "${NATRON_ROOT}/cmake/SlangKernels.cmake"
    DEPENDS ${_deps} "${NATRON_ROOT}/cmake/SlangKernels.cmake"
    VERBATIM)
  add_custom_target(${target}_gen DEPENDS "${_hdr}")
  add_library(${target} INTERFACE)
  add_dependencies(${target} ${target}_gen)
  target_include_directories(${target} INTERFACE "${_gen}")
endfunction()

noise_spike_kernel(fn2port_gpu fn2port/fn2port4d.slang main FN2P_FUSED=0)
noise_spike_kernel(fn2port_gpu_fma fn2port/fn2port4d.slang main FN2P_FUSED=1)

# The plain C++ port, once per variant. Every variant gets the same value-preserving flags:
# -ffp-contract=off so only the explicit fma() fuses, no fast-math, and -fno-math-errno /
# -fno-trapping-math, which change no result but are what lets GCC vectorise sqrtf and if-convert
# the selects. The vectoriser report for each variant is written to vec-report/<variant>.txt.
set(NOISE_SPIKE_VEC_REPORT_DIR "${CMAKE_CURRENT_BINARY_DIR}/vec-report")
file(MAKE_DIRECTORY "${NOISE_SPIKE_VEC_REPORT_DIR}")
set(NOISE_SPIKE_PORT_TARGETS "")
function(noise_spike_port_variant variant fused march needs_v3)
  set(_t fn2port_${variant})
  add_library(${_t} OBJECT fn2port/fn2port_rows.cpp)
  target_compile_definitions(${_t} PRIVATE FN2P_NS=fn2port_ns_${variant} FN2P_FUSED=${fused}
                             FN2P_VARIANT=fn2port_${variant} FN2P_NEEDS_V3=${needs_v3})
  target_compile_options(${_t} PRIVATE -march=${march} -ffp-contract=off -fno-fast-math
                         -fno-math-errno -fno-trapping-math
                         -fopt-info-vec-optimized-missed=${NOISE_SPIKE_VEC_REPORT_DIR}/${variant}.txt)
  set(NOISE_SPIKE_PORT_TARGETS ${NOISE_SPIKE_PORT_TARGETS} ${_t} PARENT_SCOPE)
endfunction()

noise_spike_port_variant(base 0 x86-64 0)
noise_spike_port_variant(base_fma 1 x86-64 0)
noise_spike_port_variant(v3 0 x86-64-v3 1)
noise_spike_port_variant(v3_fma 1 x86-64-v3 1)

add_subdirectory(fn2lib)

add_executable(NoiseSpikeBench bench.cpp)
target_include_directories(NoiseSpikeBench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/fnl")
target_compile_options(NoiseSpikeBench PRIVATE -ffp-contract=off -fno-fast-math)
target_link_libraries(NoiseSpikeBench PRIVATE ${NOISE_SPIKE_PORT_TARGETS} fn2spike_strict fn2spike_relaxed
  NatronGpu fn2port_gpu fn2port_gpu_fma Threads::Threads -static-libstdc++ -static-libgcc)
set_target_properties(NoiseSpikeBench PROPERTIES BUILD_RPATH "\$ORIGIN/fn2lib")
