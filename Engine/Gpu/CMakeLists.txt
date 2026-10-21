# NatronGpu is engine-free (no Qt, Python or Engine headers) so tools/ can
# add_subdirectory it too; NATRON_GPU_CMAKE_DIR names where the Slang helpers live.
if(NOT DEFINED NATRON_GPU_CMAKE_DIR)
    set(NATRON_GPU_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
endif()
if(NOT DEFINED NATRON_VMA_INCLUDE_DIR)
    set(NATRON_VMA_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../libs/VulkanMemoryAllocator/include")
endif()
if(NOT DEFINED NATRON_SPIRV_REFLECT_DIR)
    set(NATRON_SPIRV_REFLECT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../libs/SPIRV-Reflect")
endif()

if(NOT Vulkan_FOUND)
    find_package(Vulkan REQUIRED)
endif()
find_package(Threads REQUIRED)
enable_language(C)

include("${NATRON_GPU_CMAKE_DIR}/FetchSlang.cmake")
include("${NATRON_GPU_CMAKE_DIR}/FetchGlslang.cmake")
include("${NATRON_GPU_CMAKE_DIR}/SlangKernels.cmake")

# Each Kernels/*.cmake holds the slang_add_kernel lines of the .slang it is named after.
# Sorted so the generated GpuKernelId enum order does not depend on glob order.
file(GLOB NATRON_GPU_KERNEL_FRAGMENTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Kernels/*.cmake")
list(SORT NATRON_GPU_KERNEL_FRAGMENTS)
foreach(_fragment IN LISTS NATRON_GPU_KERNEL_FRAGMENTS)
    include("${_fragment}")
endforeach()

slang_write_kernel_table(NatronGpu OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gen/GpuKernels.inc")
get_property(NatronGpu_KERNELS GLOBAL PROPERTY SLANG_REGISTRY_NatronGpu)

add_library(NatronGpu STATIC
    GpuDevice.cpp
    GpuBuffer.cpp
    GpuBufferPool.cpp
    GpuKernel.cpp
    GpuKernelRegistry.cpp
    GpuPipelineCache.cpp
    GpuRecord.cpp
    GpuRuntime.cpp
    GpuShaderCompiler.cpp
    GpuTexture.cpp
    GpuTransfer.cpp
    GpuBlur.cpp
    GpuMorphology.cpp
    GpuResample.cpp
    GpuWrap.cpp
    GlInterop.cpp
    Vma.cpp
    ${NATRON_SPIRV_REFLECT_DIR}/spirv_reflect.c
)
target_compile_features(NatronGpu PUBLIC cxx_std_20)
target_include_directories(NatronGpu PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_include_directories(NatronGpu SYSTEM PUBLIC "${NATRON_VMA_INCLUDE_DIR}")
target_include_directories(NatronGpu SYSTEM PRIVATE "${NATRON_SPIRV_REFLECT_DIR}")
set_source_files_properties("${NATRON_SPIRV_REFLECT_DIR}/spirv_reflect.c" PROPERTIES COMPILE_OPTIONS -w)
target_link_libraries(NatronGpu
    PUBLIC
        Vulkan::Vulkan
        Threads::Threads
        ${NatronGpu_KERNELS}
    PRIVATE
        glslang
        glslang-default-resource-limits
)
target_compile_options(NatronGpu PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
set_target_properties(NatronGpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
