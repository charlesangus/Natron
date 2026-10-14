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

slang_add_kernel(grade Kernels/grade.slang ENTRY grade_main REGISTRY NatronGpu)
slang_add_kernel(blur Kernels/blur.slang ENTRY blurPass REGISTRY NatronGpu)
slang_add_kernel(morph_direct Kernels/morphology.slang ENTRY morph_direct REGISTRY NatronGpu)
slang_add_kernel(morph_wide Kernels/morphology.slang ENTRY morph_wide REGISTRY NatronGpu)
slang_add_kernel(morph_mask_bbox Kernels/morphology.slang ENTRY morph_mask_bbox REGISTRY NatronGpu)
slang_add_kernel(morph_window Kernels/morphology.slang ENTRY morph_window REGISTRY NatronGpu)
slang_add_kernel(wrap_divide Kernels/pixelwrap.slang ENTRY wrap_divide REGISTRY NatronGpu)
slang_add_kernel(wrap_finish Kernels/pixelwrap.slang ENTRY wrap_finish REGISTRY NatronGpu)
slang_add_kernel(invert Kernels/invert.slang ENTRY invert_main REGISTRY NatronGpu)
slang_add_kernel(constant Kernels/generators.slang ENTRY constant_main REGISTRY NatronGpu)
slang_add_kernel(checkerboard Kernels/generators.slang ENTRY checkerboard_main REGISTRY NatronGpu)
slang_add_kernel(grade_wrapped Kernels/pixelwrap.slang ENTRY grade_wrapped REGISTRY NatronGpu)
slang_add_kernel(clamp_wrapped Kernels/clamp.slang ENTRY clamp_wrapped REGISTRY NatronGpu)
slang_add_kernel(colorlookup_wrapped Kernels/colorlookup.slang ENTRY colorlookup_wrapped REGISTRY NatronGpu)
slang_add_kernel(colorcorrect_wrapped Kernels/colorcorrect.slang ENTRY colorcorrect_wrapped REGISTRY NatronGpu)
slang_add_kernel(saturation_wrapped Kernels/saturation.slang ENTRY saturation_wrapped REGISTRY NatronGpu)
slang_add_kernel(add_wrapped Kernels/colormath.slang ENTRY add_wrapped REGISTRY NatronGpu)
slang_add_kernel(multiply_wrapped Kernels/colormath.slang ENTRY multiply_wrapped REGISTRY NatronGpu)
slang_add_kernel(gamma_wrapped Kernels/colormath.slang ENTRY gamma_wrapped REGISTRY NatronGpu)
slang_add_kernel(resample Kernels/resample.slang ENTRY resample_main REGISTRY NatronGpu)

slang_write_kernel_table(NatronGpu OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gen/GpuKernels.inc")
get_property(NatronGpu_KERNELS GLOBAL PROPERTY SLANG_REGISTRY_NatronGpu)

add_library(NatronGpu STATIC
    GpuDevice.cpp
    GpuBuffer.cpp
    GpuBufferPool.cpp
    GpuKernel.cpp
    GpuKernelRegistry.cpp
    GpuRecord.cpp
    GpuRuntime.cpp
    GpuTransfer.cpp
    GpuBlur.cpp
    GpuMorphology.cpp
    GpuResample.cpp
    GpuWrap.cpp
    GlInterop.cpp
    Vma.cpp
)
target_compile_features(NatronGpu PUBLIC cxx_std_20)
target_include_directories(NatronGpu PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_include_directories(NatronGpu SYSTEM PUBLIC "${NATRON_VMA_INCLUDE_DIR}")
target_link_libraries(NatronGpu
    PUBLIC
        Vulkan::Vulkan
        Threads::Threads
        ${NatronGpu_KERNELS}
)
target_compile_options(NatronGpu PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
set_target_properties(NatronGpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
