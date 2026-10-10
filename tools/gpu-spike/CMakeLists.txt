cmake_minimum_required(VERSION 3.22)
project(natron_gpu_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Vulkan REQUIRED)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

include(FetchSlang)
include(SlangKernels)

enable_testing()

slang_add_kernel(smoke kernels/smoke.slang ENTRY smoke)

add_executable(Smoke_Test tests/Smoke_Test.cpp)
target_link_libraries(Smoke_Test PRIVATE smoke GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(Smoke_Test)

add_executable(DeviceList tests/DeviceList_main.cpp)
target_link_libraries(DeviceList PRIVATE Vulkan::Vulkan -static-libstdc++ -static-libgcc)

slang_add_kernel(blur kernels/blur.slang ENTRY blurPass)

add_executable(BlurCpu_Test tests/BlurCpu_Test.cpp)
target_link_libraries(BlurCpu_Test PRIVATE blur GTest::gtest_main)
gtest_discover_tests(BlurCpu_Test)

add_library(gpuspike_core STATIC src/GpuDevice.cpp src/GpuKernel.cpp src/GpuTransfer.cpp src/Vma.cpp)
target_include_directories(gpuspike_core PUBLIC src)
target_include_directories(gpuspike_core SYSTEM PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/VulkanMemoryAllocator/include")
target_link_libraries(gpuspike_core PUBLIC Vulkan::Vulkan Threads::Threads)
target_compile_options(gpuspike_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(GpuDevice_Test tests/GpuDevice_Test.cpp)
target_link_libraries(GpuDevice_Test PRIVATE gpuspike_core GTest::gtest_main
  -static-libstdc++ -static-libgcc)
gtest_discover_tests(GpuDevice_Test)

slang_add_kernel(grade kernels/grade.slang ENTRY grade_main)

add_executable(GradeCpu_Test tests/GradeCpu_Test.cpp)
target_link_libraries(GradeCpu_Test PRIVATE grade GTest::gtest_main)
gtest_discover_tests(GradeCpu_Test)

slang_add_kernel(fill kernels/fill.slang ENTRY fill_main)

add_executable(GpuKernel_Test tests/GpuKernel_Test.cpp)
target_link_libraries(GpuKernel_Test PRIVATE gpuspike_core fill GTest::gtest_main
  -static-libstdc++ -static-libgcc)
gtest_discover_tests(GpuKernel_Test)

add_executable(GpuTransfer_Test tests/GpuTransfer_Test.cpp)
target_link_libraries(GpuTransfer_Test PRIVATE gpuspike_core GTest::gtest_main
  -static-libstdc++ -static-libgcc)
gtest_discover_tests(GpuTransfer_Test DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 600)

add_executable(GradeGpu_Test tests/GradeGpu_Test.cpp)
target_link_libraries(GradeGpu_Test PRIVATE gpuspike_core grade GTest::gtest_main
  -static-libstdc++ -static-libgcc)
gtest_discover_tests(GradeGpu_Test DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 600)

add_executable(BlurGpu_Test tests/BlurGpu_Test.cpp)
target_link_libraries(BlurGpu_Test PRIVATE gpuspike_core blur GTest::gtest_main
  -static-libstdc++ -static-libgcc)
gtest_discover_tests(BlurGpu_Test DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 600)

add_executable(GpuBench bench/GpuBench_main.cpp)
target_link_libraries(GpuBench PRIVATE gpuspike_core grade blur GTest::gtest -static-libstdc++ -static-libgcc)

# The host has no gtest: stage the shared libs next to the binaries so
# run-host.sh can put them on LD_LIBRARY_PATH.
foreach(_t GTest::gtest GTest::gtest_main)
  get_target_property(_lib ${_t} IMPORTED_LOCATION)
  if(NOT _lib)
    get_target_property(_lib ${_t} IMPORTED_LOCATION_RELEASE)
  endif()
  if(NOT _lib)
    get_target_property(_lib ${_t} IMPORTED_LOCATION_NOCONFIG)
  endif()
  if(_lib)
    file(COPY "${_lib}" DESTINATION "${CMAKE_BINARY_DIR}/hostlibs" FOLLOW_SYMLINK_CHAIN)
  endif()
endforeach()

slang_add_kernel(pointop_grade kernels/pointops.slang ENTRY pointop_grade)
slang_add_kernel(pointop_invert kernels/pointops.slang ENTRY pointop_invert)
slang_add_kernel(pointop_chain kernels/pointops.slang ENTRY pointop_chain)
slang_add_kernel(pointop_masked kernels/pointops.slang ENTRY pointop_masked)

add_executable(PointOpCompose_Test tests/PointOpCompose_Test.cpp)
target_link_libraries(PointOpCompose_Test PRIVATE pointop_grade pointop_invert pointop_chain
  pointop_masked GTest::gtest_main)
gtest_discover_tests(PointOpCompose_Test)

slang_add_glsl(grade_gl kernels/grade.slang ENTRY grade_main)
slang_add_glsl(blur_gl kernels/blur.slang ENTRY blurPass)

find_package(OpenGL COMPONENTS EGL REQUIRED)
add_executable(GlCompute ref-gl/GlCompute_main.cpp)
target_include_directories(GlCompute PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(GlCompute PRIVATE grade_gl blur_gl OpenGL::EGL -static-libstdc++ -static-libgcc)

add_library(gpuspike_glinterop STATIC src/GlInterop.cpp)
target_link_libraries(gpuspike_glinterop PUBLIC gpuspike_core)
target_compile_options(gpuspike_glinterop PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(GlInterop_Test tests/GlInterop_Test.cpp)
target_link_libraries(GlInterop_Test PRIVATE gpuspike_glinterop fill OpenGL::EGL GTest::gtest_main
  -static-libstdc++ -static-libgcc)
# In the dev image /usr/local's glvnd only lists the NVIDIA EGL vendor, and /usr/local's libLLVM lacks
# the AMDGPU target that the system libgallium needs; without both overrides Mesa GL cannot load at all.
set(GL_INTEROP_TEST_ENV "")
if(EXISTS /usr/share/glvnd/egl_vendor.d/50_mesa.json AND EXISTS /usr/local/share/glvnd/egl_vendor.d)
  list(APPEND GL_INTEROP_TEST_ENV "__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json")
endif()
if(EXISTS /usr/lib64/libLLVM.so.21.1 AND EXISTS /usr/local/lib/libLLVM.so.21.1)
  list(APPEND GL_INTEROP_TEST_ENV "LD_PRELOAD=/usr/lib64/libLLVM.so.21.1")
endif()
string(REPLACE ";" "\\;" _gl_interop_env "${GL_INTEROP_TEST_ENV}")
gtest_discover_tests(GlInterop_Test DISCOVERY_TIMEOUT 30 PROPERTIES ENVIRONMENT "${_gl_interop_env}")

find_package(Qt6 QUIET COMPONENTS OpenGLWidgets)
if(Qt6OpenGLWidgets_FOUND)
  add_executable(GlInteropQt tests/GlInteropQt_main.cpp)
  target_link_libraries(GlInteropQt PRIVATE gpuspike_glinterop Qt6::OpenGLWidgets)
  find_program(XVFB_RUN xvfb-run)
  if(XVFB_RUN)
    add_test(NAME GlInteropQt COMMAND ${XVFB_RUN} -a -s "-screen 0 1024x768x24" $<TARGET_FILE:GlInteropQt>)
    set_tests_properties(GlInteropQt PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb;${GL_INTEROP_TEST_ENV}")
  endif()
endif()

slang_add_kernel(grade_twin kernels/grade.slang ENTRY grade_main)
slang_add_kernel(blur_twin kernels/blur.slang ENTRY blurPass)
foreach(_t grade_twin blur_twin)
  target_compile_options(${_t} PRIVATE -O2 -DNDEBUG -march=x86-64-v3)
endforeach()

# The engine's blur filters, compiled from source against stand-ins for the two Natron headers
# they include, so the native CPU side is measured without linking the engine.
set(CPU_TWIN_SHIM "${CMAKE_CURRENT_BINARY_DIR}/cpu-twin-shim")
file(WRITE "${CPU_TWIN_SHIM}/Python.h" "")
file(WRITE "${CPU_TWIN_SHIM}/Global/Macros.h"
"#pragma once\n#define NATRON_NAMESPACE_ENTER\n#define NATRON_NAMESPACE_EXIT\n")
add_library(native_blur_kernels STATIC ../../Engine/Nodes/Filter/BlurKernels.cpp)
target_include_directories(native_blur_kernels PUBLIC "${CPU_TWIN_SHIM}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Nodes/Filter")
target_compile_options(native_blur_kernels PRIVATE -O2 -DNDEBUG -march=x86-64-v3)

add_executable(CpuTwinBench bench/CpuTwinBench_main.cpp)
target_compile_options(CpuTwinBench PRIVATE -O2 -DNDEBUG -march=x86-64-v3)
target_link_libraries(CpuTwinBench PRIVATE grade_twin blur_twin native_blur_kernels Threads::Threads)
