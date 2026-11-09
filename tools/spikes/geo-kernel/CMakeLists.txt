# Geometry kernel execution spike: a batched bytecode interpreter against an LLVM JIT on one
# shared front end. Standalone like tools/spikes/noise; nothing here is linked into Natron.
# See README.md for build and run commands.

cmake_minimum_required(VERSION 3.22)
project(natron_geo_kernel_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# -O2 as Natron's kernels see it. Contraction off so every backend rounds each operation the
# same way and agreement can be checked bit for bit.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
add_compile_options(-ffp-contract=off)

set(NATRON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
find_package(Threads REQUIRED)
find_package(LLVM CONFIG)
if(LLVM_FOUND AND NOT LLVM_VERSION_MAJOR EQUAL 21)
  message(STATUS "geo-kernel spike: found LLVM ${LLVM_PACKAGE_VERSION}, want 21")
  set(LLVM_FOUND FALSE)
endif()

set(GK_BC "${CMAKE_CURRENT_BINARY_DIR}/builtins.bc")

if(LLVM_FOUND)
  find_program(GK_CLANGXX clang++ HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
  add_custom_command(
    OUTPUT "${GK_BC}"
    COMMAND "${GK_CLANGXX}" -std=c++20 -O2 -ffp-contract=off -emit-llvm -c
            "${CMAKE_CURRENT_SOURCE_DIR}/builtins_bc.cpp"
            -I "${CMAKE_CURRENT_SOURCE_DIR}" -I "${NATRON_ROOT}/libs/noise" -o "${GK_BC}"
    DEPENDS builtins_bc.cpp builtins.h "${NATRON_ROOT}/libs/noise/Noise4D.h"
    COMMENT "Compiling JIT builtins to bitcode")
  add_custom_target(gk_builtins_bc ALL DEPENDS "${GK_BC}")

  add_library(gk_jit OBJECT jit.cpp)
  separate_arguments(GK_LLVM_DEFS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  target_compile_definitions(gk_jit PRIVATE GK_WITH_LLVM=1)
  target_compile_options(gk_jit PRIVATE ${GK_LLVM_DEFS})
  target_include_directories(gk_jit PRIVATE ${LLVM_INCLUDE_DIRS} "${NATRON_ROOT}/libs/noise")
  if(TARGET LLVM)
    set(GK_LLVM_LIBS LLVM)
  else()
    llvm_map_components_to_libnames(GK_LLVM_LIBS orcjit native passes bitreader linker)
  endif()
  message(STATUS "geo-kernel spike: JIT backend on LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")

  # The image's LLVM development package is an assertions build, which slows compilation. The
  # distribution's libLLVM of the same version is a release build without headers, so build a
  # second JIT against it, through the same headers with the ABI-breaking checks switched off to
  # match it. This gives the compile latency a shipped release build would have.
  set(GK_RELEASE_LLVM "/usr/lib64/libLLVM.so.${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}")
  if(EXISTS "${GK_RELEASE_LLVM}" AND LLVM_ENABLE_ASSERTIONS)
    set(_overlay "${CMAKE_CURRENT_BINARY_DIR}/release-llvm-include")
    file(READ "${LLVM_INCLUDE_DIRS}/llvm/Config/abi-breaking.h" _abi)
    string(REPLACE "#define LLVM_ENABLE_ABI_BREAKING_CHECKS 1" "#define LLVM_ENABLE_ABI_BREAKING_CHECKS 0" _abi "${_abi}")
    file(WRITE "${_overlay}/llvm/Config/abi-breaking.h" "${_abi}")
    add_library(gk_jit_rel OBJECT jit.cpp)
    target_compile_definitions(gk_jit_rel PRIVATE GK_WITH_LLVM=1 __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)
    target_include_directories(gk_jit_rel BEFORE PRIVATE "${_overlay}")
    target_include_directories(gk_jit_rel PRIVATE ${LLVM_INCLUDE_DIRS} "${NATRON_ROOT}/libs/noise")
    set(GK_HAVE_RELEASE_LLVM ON)
  endif()
else()
  add_library(gk_jit OBJECT jit.cpp)
  target_include_directories(gk_jit PRIVATE "${NATRON_ROOT}/libs/noise")
  set(GK_LLVM_LIBS "")
  message(STATUS "geo-kernel spike: LLVM 21 not found, JIT backend disabled")
endif()

add_library(gk_frontend OBJECT frontend.cpp)
target_include_directories(gk_frontend PRIVATE "${NATRON_ROOT}/libs/noise")

# The interpreter, the native reference and the runner are built once for the x86-64 baseline
# Natron ships, and once for x86-64-v3 (AVX2/FMA) to show what multiversioning would buy. The
# JIT always targets the host CPU.
foreach(variant baseline v3)
  if(variant STREQUAL "baseline")
    set(exe gk_bench)
    set(arch x86-64)
  else()
    set(exe gk_bench_v3)
    set(arch x86-64-v3)
  endif()
  add_executable(${exe} bench.cpp interp.cpp builtins_host.cpp
                 $<TARGET_OBJECTS:gk_frontend> $<TARGET_OBJECTS:gk_jit>)
  target_compile_options(${exe} PRIVATE -march=${arch})
  target_compile_definitions(${exe} PRIVATE GK_BUILTINS_BC="${GK_BC}" GK_INTERP_ARCH="${arch}")
  target_include_directories(${exe} PRIVATE "${NATRON_ROOT}/libs/noise")
  target_link_libraries(${exe} PRIVATE Threads::Threads ${GK_LLVM_LIBS})
endforeach()

if(GK_HAVE_RELEASE_LLVM)
  add_executable(gk_bench_rel bench.cpp interp.cpp builtins_host.cpp
                 $<TARGET_OBJECTS:gk_frontend> $<TARGET_OBJECTS:gk_jit_rel>)
  target_compile_options(gk_bench_rel PRIVATE -march=x86-64)
  target_compile_definitions(gk_bench_rel PRIVATE GK_BUILTINS_BC="${GK_BC}" GK_INTERP_ARCH="x86-64")
  target_include_directories(gk_bench_rel PRIVATE "${NATRON_ROOT}/libs/noise")
  target_link_libraries(gk_bench_rel PRIVATE Threads::Threads "${GK_RELEASE_LLVM}")
  # Both libraries share the SONAME libLLVM.so.21.1; an old-style DT_RPATH is searched before
  # LD_LIBRARY_PATH, so the release copy wins.
  get_filename_component(_rel_dir "${GK_RELEASE_LLVM}" DIRECTORY)
  target_link_options(gk_bench_rel PRIVATE -Wl,--disable-new-dtags -Wl,-rpath,${_rel_dir})
endif()
