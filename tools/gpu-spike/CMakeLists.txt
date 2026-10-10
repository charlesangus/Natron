cmake_minimum_required(VERSION 3.22)
project(natron_gpu_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Vulkan REQUIRED)
find_package(GTest REQUIRED)

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
