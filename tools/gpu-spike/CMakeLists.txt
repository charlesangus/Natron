cmake_minimum_required(VERSION 3.22)
project(natron_gpu_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATRON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(KERNEL_DIR "${NATRON_ROOT}/Engine/Gpu/Kernels")

# The engine's Grade runs through the wrapped pixel path, so the standalone grade kernel lives
# here. slangc resolves `import gradeops` next to the source file, hence the staging directory
# holding both; configure_file re-copies them whenever either source changes.
set(SPIKE_KERNEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/kernels")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/kernels/grade.slang" "${SPIKE_KERNEL_DIR}/grade.slang" COPYONLY)
configure_file("${KERNEL_DIR}/gradeops.slang" "${SPIKE_KERNEL_DIR}/gradeops.slang" COPYONLY)

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

include("${NATRON_ROOT}/cmake/FetchSlang.cmake")
include("${NATRON_ROOT}/cmake/SlangKernels.cmake")
include(SlangGlsl.cmake)

add_subdirectory("${NATRON_ROOT}/Engine/Gpu" natron_gpu)

add_executable(DeviceList tests/DeviceList_main.cpp)
target_link_libraries(DeviceList PRIVATE Vulkan::Vulkan -static-libstdc++ -static-libgcc)

# Bench-only references (fixtures, CPU oracles) are shared with the unit tests.
set(GPU_TEST_DIR "${NATRON_ROOT}/Tests/Gpu")

slang_add_kernel(grade ${SPIKE_KERNEL_DIR}/grade.slang ENTRY grade_main)

add_executable(GpuBench bench/GpuBench_main.cpp)
target_include_directories(GpuBench PRIVATE "${GPU_TEST_DIR}")
target_link_libraries(GpuBench PRIVATE NatronGpu grade -static-libstdc++ -static-libgcc)

add_executable(ImportCost bench/ImportCost_main.cpp)
target_link_libraries(ImportCost PRIVATE NatronGpu -static-libstdc++ -static-libgcc)

slang_add_glsl(grade_gl ${SPIKE_KERNEL_DIR}/grade.slang ENTRY grade_main)
slang_add_glsl(blur_gl ${KERNEL_DIR}/blur.slang ENTRY blurPass)

find_package(OpenGL COMPONENTS EGL REQUIRED)
add_executable(GlCompute ref-gl/GlCompute_main.cpp)
target_include_directories(GlCompute PRIVATE "${GPU_TEST_DIR}")
target_link_libraries(GlCompute PRIVATE grade_gl blur_gl OpenGL::EGL -static-libstdc++ -static-libgcc)

find_package(Qt6 QUIET COMPONENTS OpenGLWidgets)
if(Qt6OpenGLWidgets_FOUND)
  add_executable(GlInteropQt tests/GlInteropQt_main.cpp)
  target_link_libraries(GlInteropQt PRIVATE NatronGpu Qt6::OpenGLWidgets)
endif()

slang_add_kernel(grade_twin ${SPIKE_KERNEL_DIR}/grade.slang ENTRY grade_main CPP)
slang_add_kernel(blur_twin ${KERNEL_DIR}/blur.slang ENTRY blurPass CPP)
foreach(_t grade_twin blur_twin)
  target_compile_options(${_t} PRIVATE -O2 -DNDEBUG -march=x86-64-v3)
endforeach()

# The engine's blur filters, compiled from source against stand-ins for the two Natron headers
# they include, so the native CPU side is measured without linking the engine.
set(CPU_TWIN_SHIM "${CMAKE_CURRENT_BINARY_DIR}/cpu-twin-shim")
file(WRITE "${CPU_TWIN_SHIM}/Python.h" "")
file(WRITE "${CPU_TWIN_SHIM}/Global/Macros.h"
"#pragma once\n#define NATRON_NAMESPACE_ENTER namespace Natron {\n#define NATRON_NAMESPACE_EXIT }\n")
add_library(native_blur_kernels STATIC ${NATRON_ROOT}/Engine/Nodes/Filter/BlurKernels.cpp)
target_include_directories(native_blur_kernels PUBLIC "${CPU_TWIN_SHIM}"
  "${NATRON_ROOT}/Engine/Nodes/Filter")
target_compile_options(native_blur_kernels PRIVATE -O2 -DNDEBUG -march=x86-64-v3)

add_executable(CpuTwinBench bench/CpuTwinBench_main.cpp)
target_compile_options(CpuTwinBench PRIVATE -O2 -DNDEBUG -march=x86-64-v3)
target_include_directories(CpuTwinBench PRIVATE "${GPU_TEST_DIR}" "${NATRON_ROOT}/Engine/Gpu")
target_include_directories(CpuTwinBench SYSTEM PRIVATE "${NATRON_ROOT}/libs/VulkanMemoryAllocator/include")
target_link_libraries(CpuTwinBench PRIVATE grade_twin blur_twin native_blur_kernels Threads::Threads)
