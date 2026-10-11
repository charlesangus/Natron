# NatronGpu is engine-free (no Qt, Python or Engine headers) so tools/ can
# add_subdirectory it too; NATRON_GPU_CMAKE_DIR names where the Slang helpers live.
if(NOT DEFINED NATRON_GPU_CMAKE_DIR)
    set(NATRON_GPU_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
endif()
if(NOT DEFINED NATRON_VMA_INCLUDE_DIR)
    set(NATRON_VMA_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../libs/VulkanMemoryAllocator/include")
endif()

if(NOT Vulkan_FOUND)
    find_package(Vulkan REQUIRED)
endif()
find_package(Threads REQUIRED)

include("${NATRON_GPU_CMAKE_DIR}/FetchSlang.cmake")
include("${NATRON_GPU_CMAKE_DIR}/SlangKernels.cmake")

slang_add_kernel(grade Kernels/grade.slang ENTRY grade_main)
slang_add_kernel(blur Kernels/blur.slang ENTRY blurPass)
slang_add_kernel(pointop_grade Kernels/pointops.slang ENTRY pointop_grade)
slang_add_kernel(pointop_invert Kernels/pointops.slang ENTRY pointop_invert)
slang_add_kernel(pointop_chain Kernels/pointops.slang ENTRY pointop_chain)
slang_add_kernel(pointop_masked Kernels/pointops.slang ENTRY pointop_masked)

add_library(NatronGpu STATIC
    GpuDevice.cpp
    GpuBuffer.cpp
    GpuKernel.cpp
    GpuTransfer.cpp
    GpuBlur.cpp
    GlInterop.cpp
    Vma.cpp
)
target_compile_features(NatronGpu PUBLIC cxx_std_20)
target_include_directories(NatronGpu PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(NatronGpu SYSTEM PUBLIC "${NATRON_VMA_INCLUDE_DIR}")
target_link_libraries(NatronGpu
    PUBLIC
        Vulkan::Vulkan
        Threads::Threads
        grade
        blur
        pointop_grade
        pointop_invert
        pointop_chain
        pointop_masked
)
target_compile_options(NatronGpu PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
set_target_properties(NatronGpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
