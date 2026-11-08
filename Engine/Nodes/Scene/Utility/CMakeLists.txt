# The sources here compile into NatronEngine through the recursive globs in Engine/CMakeLists.txt.
# This file carries what only they need.
target_link_libraries(NatronEngine PRIVATE embree)

# The kernels run on the CPU whether or not the Vulkan backend is built, so Slang is needed regardless
# of NATRON_GPU.
include(${CMAKE_SOURCE_DIR}/cmake/FetchSlang.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/SlangKernels.cmake)
slang_add_kernel(utility_raygen ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/RayGen.slang ENTRY utilityRayGen CPP)
slang_add_kernel(utility_shade_flat ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/ShadeFlat.slang ENTRY utilityShadeFlat CPP)
target_link_libraries(NatronEngine PRIVATE utility_raygen utility_shade_flat)
