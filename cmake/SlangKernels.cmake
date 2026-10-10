# slang_add_kernel(<target> <file.slang> ENTRY <name> [CPP])
#
# Compiles one compute entry point to SPIR-V and exposes it through the header
# INTERFACE library <target>: <target>_spirv.h defines
# `inline constexpr uint32_t <target>_spirv[]`, `<target>_spirv_words` and
# `<target>_group_size[3]` (the numthreads values, read from the reflection
# JSON written beside it). The generated include dir is public on the target.
#
# CPP additionally compiles the C++ target against the Slang prelude headers,
# making <target> a static library that exports the C symbols <entry>,
# <entry>_Group and <entry>_Thread, for oracle tests.

if(CMAKE_SCRIPT_MODE_FILE)
  # Script mode: `cmake -DIN=.. -DOUT=.. -DSYMBOL=.. -P SlangKernels.cmake`
  # turns a SPIR-V binary into a header.
  file(READ "${IN}" _hex HEX)
  string(LENGTH "${_hex}" _hexlen)
  math(EXPR _bytes "${_hexlen} / 2")
  math(EXPR _rem "${_bytes} % 4")
  if(NOT _rem EQUAL 0)
    message(FATAL_ERROR "${IN} is not a whole number of 32-bit words")
  endif()
  # SPIR-V words are little-endian in the files slangc writes on this host.
  string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])"
         "0x\\4\\3\\2\\1u," _words "${_hex}")
  math(EXPR _count "${_bytes} / 4")
  file(READ "${JSON}" _json)
  string(JSON _gx GET "${_json}" entryPoints 0 threadGroupSize 0)
  string(JSON _gy GET "${_json}" entryPoints 0 threadGroupSize 1)
  string(JSON _gz GET "${_json}" entryPoints 0 threadGroupSize 2)
  file(WRITE "${OUT}"
"#pragma once
#include <cstddef>
#include <cstdint>

inline constexpr uint32_t ${SYMBOL}_spirv[] = {${_words}};
inline constexpr std::size_t ${SYMBOL}_spirv_words = ${_count};
inline constexpr uint32_t ${SYMBOL}_group_size[3] = {${_gx}u, ${_gy}u, ${_gz}u};
")
  return()
endif()

set(_SLANG_KERNELS_SELF "${CMAKE_CURRENT_LIST_FILE}")

function(slang_add_kernel target source)
  cmake_parse_arguments(ARG "CPP" "ENTRY" "" ${ARGN})
  if(NOT ARG_ENTRY)
    message(FATAL_ERROR "slang_add_kernel(${target}): ENTRY is required")
  endif()
  get_filename_component(_src "${source}" ABSOLUTE)
  get_filename_component(_srcdir "${_src}" DIRECTORY)
  file(GLOB _modules CONFIGURE_DEPENDS "${_srcdir}/*.slang")
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/${target}")
  set(_spv "${_gen}/${target}.spv")
  set(_hdr "${_gen}/${target}_spirv.h")
  set(_cpp "${_gen}/${target}_cpu.cpp")
  set(_json "${_gen}/${target}.reflection.json")

  add_custom_command(
    OUTPUT "${_hdr}" "${_json}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
    COMMAND "${SLANG_COMPILER}" "${_src}" -entry ${ARG_ENTRY} -stage compute
            -target spirv -profile spirv_1_5 -o "${_spv}"
            -reflection-json "${_json}"
    COMMAND ${CMAKE_COMMAND} -DIN=${_spv} -DOUT=${_hdr} -DSYMBOL=${target} -DJSON=${_json}
            -P "${_SLANG_KERNELS_SELF}"
    DEPENDS "${_src}" ${_modules} "${_SLANG_KERNELS_SELF}"
    VERBATIM)

  if(NOT ARG_CPP)
    add_custom_target(${target}_gen DEPENDS "${_hdr}")
    add_library(${target} INTERFACE)
    add_dependencies(${target} ${target}_gen)
    target_include_directories(${target} INTERFACE "${_gen}")
    return()
  endif()

  add_custom_command(
    OUTPUT "${_cpp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
    COMMAND "${SLANG_COMPILER}" "${_src}" -entry ${ARG_ENTRY} -stage compute
            -target cpp -o "${_cpp}"
    DEPENDS "${_src}" ${_modules}
    VERBATIM)

  add_library(${target} STATIC "${_cpp}" "${_hdr}")
  target_include_directories(${target} PUBLIC "${_gen}")
  target_include_directories(${target} SYSTEM PUBLIC "${SLANG_INCLUDE_DIR}")
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()
