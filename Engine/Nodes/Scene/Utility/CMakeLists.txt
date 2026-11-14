# The sources here compile into NatronEngine through the recursive globs in Engine/CMakeLists.txt.
# This file carries what only they need.
target_link_libraries(NatronEngine PRIVATE embree)

# Subdivision meshes refine through OpenSubdiv's Far directly, from the install the pinned USD links.
target_link_libraries(NatronEngine PRIVATE pxOsd OpenSubdiv::osdCPU)

# The kernels run on the CPU whether or not the Vulkan backend is built, so Slang is needed regardless
# of NATRON_GPU.
include(${CMAKE_SOURCE_DIR}/cmake/FetchSlang.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/SlangKernels.cmake)
slang_add_kernel(utility_raygen ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/RayGen.slang ENTRY utilityRayGen CPP)
slang_add_kernel(utility_shade_flat ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/ShadeFlat.slang ENTRY utilityShadeFlat CPP)
target_link_libraries(NatronEngine PRIVATE utility_raygen utility_shade_flat)

# Ray queries have no CPU target, so these kernels are SPIR-V only. Slang would add the capability on its own,
# with a warning that the profile was upgraded.
if(NATRON_GPU)
    slang_add_kernel(utility_intersect ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/Intersect.slang ENTRY utilityIntersect
                     FLAGS -capability spvRayQueryKHR)
    slang_add_kernel(utility_occluded ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/Intersect.slang ENTRY utilityOccluded
                     FLAGS -capability spvRayQueryKHR)
    target_link_libraries(NatronEngine PRIVATE utility_intersect utility_occluded)
    # The film of a GPU frame stays on the device; these run only there.
    slang_add_kernel(utility_film_clear ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/FilmClear.slang ENTRY utilityFilmClear)
    slang_add_kernel(utility_film_scatter ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/FilmScatter.slang ENTRY utilityFilmScatter)
    slang_add_kernel(utility_film_pack ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/FilmPack.slang ENTRY utilityFilmPack)
    target_link_libraries(NatronEngine PRIVATE utility_film_clear utility_film_scatter utility_film_pack)
endif()
