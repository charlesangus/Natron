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

add_library(gpuspike_core STATIC src/GpuDevice.cpp src/GpuKernel.cpp src/Vma.cpp)
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
