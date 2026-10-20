# Host-runnable GPU tests: links only NatronGpu (no Qt, Python or Engine) plus the bundled gtest,
# with the C++ runtime linked statically so the binary also runs outside the dev container.
find_package(OpenGL COMPONENTS EGL)

include(${CMAKE_SOURCE_DIR}/cmake/FetchSlang.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/SlangKernels.cmake)
set(NATRON_GPU_KERNEL_DIR ${CMAKE_SOURCE_DIR}/Engine/Gpu/Kernels)

# The CPU twins and the extra test kernels get their own names; NatronGpu already owns the
# SPIR-V targets named after its registry kernels.
slang_add_kernel(test_blur ${NATRON_GPU_KERNEL_DIR}/blur.slang ENTRY blurPass CPP)
foreach(_e pixelwrap_identity pixelwrap_scale pixelwrap_grade pixelwrap_invert pixelwrap_chain)
    slang_add_kernel(${_e} ${NATRON_GPU_KERNEL_DIR}/pixelwrap.slang ENTRY ${_e} CPP)
endforeach()
foreach(_e wrap_divide wrap_finish)
    slang_add_kernel(test_${_e} ${NATRON_GPU_KERNEL_DIR}/pixelwrap.slang ENTRY ${_e} CPP)
endforeach()
foreach(_e compositewrap_two compositewrap_four)
    slang_add_kernel(${_e} ${NATRON_GPU_KERNEL_DIR}/compositewrap.slang ENTRY ${_e} CPP)
endforeach()
slang_add_kernel(fill kernels/fill.slang ENTRY fill_main)
slang_add_kernel(smoke kernels/smoke.slang ENTRY smoke)
slang_add_kernel(test_smoke kernels/smoke.slang ENTRY smoke CPP)

set(GpuTests_SOURCES
    ../google-test/src/gtest-all.cc
    ../google-test/src/gtest_main.cc
    BlurCpu_Test.cpp
    BlurGpu_Test.cpp
    GpuBuffer_Test.cpp
    GpuBufferPool_Test.cpp
    GpuDevice_Test.cpp
    GpuKernel_Test.cpp
    GpuKernelRegistry_Test.cpp
    GpuPipelineCache_Test.cpp
    GpuRuntime_Test.cpp
    GpuRuntimeShader_Test.cpp
    GpuShaderCompiler_Test.cpp
    GpuTransfer_Test.cpp
    PixelWrapCompose_Test.cpp
    PixelWrap_Test.cpp
    Smoke_Test.cpp
)
if(TARGET OpenGL::EGL)
    list(APPEND GpuTests_SOURCES GlInterop_Test.cpp)
endif()

add_executable(GpuTests ${GpuTests_SOURCES})
target_include_directories(GpuTests
    PRIVATE
        .
        ../google-test/include
        ../google-test
)
target_link_libraries(GpuTests
    PRIVATE
        NatronGpu
        fill
        smoke
        test_blur
        pixelwrap_identity
        pixelwrap_scale
        pixelwrap_grade
        pixelwrap_invert
        pixelwrap_chain
        test_wrap_divide
        test_wrap_finish
        compositewrap_two
        compositewrap_four
        test_smoke
        -static-libstdc++
        -static-libgcc
)
if(TARGET OpenGL::EGL)
    target_link_libraries(GpuTests PRIVATE ${OPENGL_egl_LIBRARY})
    target_include_directories(GpuTests SYSTEM PRIVATE ${OPENGL_EGL_INCLUDE_DIRS})
endif()

# In the dev image /usr/local's glvnd only lists the NVIDIA EGL vendor, so Mesa's vendor file is
# named explicitly; elsewhere the system default already works.
set(GpuTests_GL_ENV "")
if(EXISTS /usr/share/glvnd/egl_vendor.d/50_mesa.json AND EXISTS /usr/local/share/glvnd/egl_vendor.d)
    list(APPEND GpuTests_GL_ENV "__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json")
endif()
string(REPLACE ";" "\;" GpuTests_GL_ENV "${GpuTests_GL_ENV}")

# Registered twice: as the device offers it, and with VK_KHR_push_descriptor masked off so the
# descriptor-pool path in GpuKernel runs on every driver. GlInterop is registered once with the
# EGL environment; the second registration filters it out. Every ctest name starts with Gpu so
# `ctest -R '^Gpu'` selects them all, and the prefixes keep the three registrations distinct.
gtest_discover_tests(GpuTests
    TEST_PREFIX Gpu.
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
    TEST_FILTER -GlInteropTest.*
    PROPERTIES TIMEOUT 600 LABELS gpu
)
gtest_discover_tests(GpuTests
    TEST_PREFIX GpuNoPushDescriptor.
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
    TEST_FILTER -GlInteropTest.*
    PROPERTIES TIMEOUT 600 LABELS gpu ENVIRONMENT NATRON_TEST_GPU_NO_PUSH_DESCRIPTOR=1
)
gtest_discover_tests(GpuTests
    TEST_PREFIX Gpu.
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
    TEST_FILTER GlInteropTest.*
    PROPERTIES TIMEOUT 600 LABELS gpu ENVIRONMENT "${GpuTests_GL_ENV}"
)
