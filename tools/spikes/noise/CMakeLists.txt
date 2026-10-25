# Noise library spike: Cycles 4D Perlin / Voronoi F1 against OpenSimplex2S 4D / our Voronoi F1,
# CPU against GPU. Standalone like tools/gpu-spike; nothing here is linked into Natron.
#
# In the natron-dev container, from the repository root:
#   cmake -S tools/spikes/noise -B build/spikes/noise-build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/spikes/noise-build
# On the host GPU:
#   tools/gpu-spike/run-host.sh build/spikes/noise-build/NoiseSpikeBench --out build/spikes/noise

cmake_minimum_required(VERSION 3.22)
project(natron_noise_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATRON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

include("${NATRON_ROOT}/cmake/FetchSlang.cmake")
include("${NATRON_ROOT}/cmake/SlangKernels.cmake")

add_subdirectory("${NATRON_ROOT}/Engine/Gpu" natron_gpu)

# As slang_add_kernel, plus -fp-mode precise: the comparison against the CPU is only meaningful
# when the GPU compiler may not fuse multiply-adds either.
function(noise_spike_kernel target source entry)
  get_filename_component(_src "${source}" ABSOLUTE)
  get_filename_component(_srcdir "${_src}" DIRECTORY)
  file(GLOB _deps CONFIGURE_DEPENDS "${_srcdir}/*.slang" "${_srcdir}/*.inc")
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/${target}")
  set(_spv "${_gen}/${target}.spv")
  set(_hdr "${_gen}/${target}_spirv.h")
  set(_json "${_gen}/${target}.reflection.json")
  add_custom_command(
    OUTPUT "${_hdr}" "${_json}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
    COMMAND "${SLANG_COMPILER}" "${_src}" -entry ${entry} -stage compute
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

noise_spike_kernel(cycles_perlin cycles/noise4d.slang perlin_main)
noise_spike_kernel(cycles_voronoi cycles/noise4d.slang voronoi_main)
noise_spike_kernel(os2s opensimplex/os2s4d.slang os2s_main)
noise_spike_kernel(os2s_voronoi opensimplex/os2s4d.slang voronoi_main)

add_executable(NoiseSpikeBench bench.cpp)
target_include_directories(NoiseSpikeBench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(NoiseSpikeBench PRIVATE -O2 -ffp-contract=off -fno-fast-math)
target_link_libraries(NoiseSpikeBench PRIVATE NatronGpu cycles_perlin cycles_voronoi os2s os2s_voronoi
  Threads::Threads -static-libstdc++ -static-libgcc)
