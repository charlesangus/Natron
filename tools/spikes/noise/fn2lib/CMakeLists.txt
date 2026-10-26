# Vendored FastNoise2 and FastSIMD built twice, strict and relaxed floating point, with no network
# fetch. Pins: Auburn/FastNoise2 721400f9560831be2fc9790be9b72c44bee7d33f and the FastSIMD commit it
# pins, Auburn/FastSIMD f7e9ce6fca475731c0a3c115ec29dcfd36886b2a. Only the files these builds need
# are vendored (no tests, examples, tools or node editor).
#
# Each mode becomes a shared object exporting only the fn2spike_<mode>_* functions of fn2shim.h:
# both modes instantiate the same FastSIMD/FastNoise symbols with different code, so they must not
# share a link namespace.

set(FASTSIMD_TESTS OFF CACHE BOOL "" FORCE)
set(FASTSIMD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(FastSIMD fastsimd)
set(FastSIMD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/FastSIMD")

set(FN2SPIKE_FASTNOISE_SOURCES
    FastNoise2/src/FastNoise/Metadata.cpp
    FastNoise2/src/FastNoise/SmartNode.cpp)

function(fn2spike_add_mode mode)
    set(dispatch FastSIMD_FN2_${mode})
    if(mode STREQUAL "relaxed")
        fastsimd_create_dispatch_library(${dispatch} RELAXED SOURCES "FastNoise2/src/FastNoise/FastSIMD_Build.inl")
    else()
        fastsimd_create_dispatch_library(${dispatch} SOURCES "FastNoise2/src/FastNoise/FastSIMD_Build.inl")
    endif()
    target_include_directories(${dispatch} PRIVATE FastNoise2/include)
    target_compile_definitions(${dispatch} PRIVATE FASTNOISE_STATIC_LIB)
    target_compile_options(${dispatch} PRIVATE -fno-stack-protector -Wno-nan-infinity-disabled)
    set_target_properties(${dispatch} PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17)

    add_library(fn2spike_${mode} SHARED fn2shim.cpp ${FN2SPIKE_FASTNOISE_SOURCES})
    target_include_directories(fn2spike_${mode} PRIVATE FastNoise2/include)
    target_compile_definitions(fn2spike_${mode} PRIVATE FASTNOISE_STATIC_LIB FASTNOISE_EXPORT
                               FASTNOISE2_VERSION="1.1.1" FN2SPIKE_MODE=${mode})
    target_link_libraries(fn2spike_${mode} PRIVATE ${dispatch} FastSIMD)
    target_link_options(fn2spike_${mode} PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/fn2shim.map"
        -static-libstdc++ -static-libgcc)
    set_target_properties(fn2spike_${mode} PROPERTIES CXX_STANDARD 17)
    if(mode STREQUAL "relaxed")
        target_compile_options(${dispatch} PRIVATE -ffast-math)
    else()
        target_compile_options(fn2spike_${mode} PRIVATE -ffp-contract=off)
    endif()
endfunction()

fn2spike_add_mode(strict)
fn2spike_add_mode(relaxed)
