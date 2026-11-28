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
slang_add_kernel(utility_raygen_uv ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/RayGenUv.slang ENTRY utilityRayGenUv CPP)
slang_add_kernel(utility_shade_surface ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/ShadeSurface.slang ENTRY utilityShadeSurface CPP)
slang_add_kernel(utility_shade_lights ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/ShadeLights.slang ENTRY utilityShadeLights CPP)
slang_add_kernel(utility_resolve_hits ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/ResolveHits.slang ENTRY utilityResolveHits CPP)
target_link_libraries(NatronEngine PRIVATE utility_raygen utility_raygen_uv utility_shade_surface utility_shade_lights utility_resolve_hits)

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
    slang_add_kernel(utility_film_pick ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/FilmPick.slang ENTRY utilityFilmPick)
    target_link_libraries(NatronEngine PRIVATE utility_film_clear utility_film_scatter utility_film_pack utility_film_pick)
    # The visibility buffer of Raster mode: GpuTracer registers it with the registry it is given.
    slang_add_raster(utility_raster_visibility ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/RasterVisibility.slang
                     VERTEX visibilityVertex FRAGMENT visibilityFragment)
    target_link_libraries(NatronEngine PRIVATE utility_raster_visibility)
    # The same, leaving undrawn the texels where a surface has a hole, for frames that trace nothing.
    slang_add_raster(utility_raster_cutout ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/RasterCutout.slang
                     VERTEX cutoutVertex FRAGMENT cutoutFragment)
    target_link_libraries(NatronEngine PRIVATE utility_raster_cutout)
    # Rebuilds a raster pass's hits from its visibility buffer; device addresses keep it off the CPU.
    slang_add_kernel(utility_raster_hits ${CMAKE_CURRENT_SOURCE_DIR}/Kernels/RasterHits.slang ENTRY utilityRasterHits)
    target_link_libraries(NatronEngine PRIVATE utility_raster_hits)
endif()
