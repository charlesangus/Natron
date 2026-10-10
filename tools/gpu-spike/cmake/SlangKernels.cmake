# slang_add_kernel(<target> <file.slang> ENTRY <name>)
#
# Compiles one compute entry point two ways and exposes both through a static
# library <target>:
#   - SPIR-V embedded as `inline constexpr uint32_t <target>_spirv[]` in
#     <target>_spirv.h (generated include dir is public on the target).
#   - The C++ target, built against the Slang prelude headers, exporting the
#     C symbols <entry>, <entry>_Group and <entry>_Thread.
# <target>.reflection.json is written beside them; the numthreads values live
# as `threadGroupSize` in its entryPoints array and are also emitted into the
# header as `inline constexpr uint32_t <target>_group_size[3]`.

if(CMAKE_SCRIPT_MODE_FILE AND GLSL_IN)
  # Script mode: `cmake -DGLSL_IN=.. -DOUT=.. -DSYMBOL=.. -P SlangKernels.cmake`
  # embeds slangc's GLSL as a string, downgraded to what GL 4.3 accepts.
  file(READ "${GLSL_IN}" _glsl)
  string(REPLACE "#version 450" "#version 430" _glsl "${_glsl}")
  # GL 4.3 has no GL_KHR_memory_scope_semantics; barrier() plus a shared
  # memory barrier is the equivalent workgroup sync.
  string(REPLACE "#extension GL_KHR_memory_scope_semantics : require\n" "" _glsl "${_glsl}")
  string(REGEX REPLACE "controlBarrier\\([^;]*\\);" "memoryBarrierShared(); barrier();" _glsl "${_glsl}")
  # push_constant does not exist in GL: turn the parameter block into a UBO
  # bound right after the storage buffers, which is where the host binds it.
  if(_glsl MATCHES "layout\\(push_constant\\)[ \t\n]*layout\\(std430\\) uniform")
    string(REGEX MATCHALL "layout\\(std430, binding = [0-9]+\\)" _ssbos "${_glsl}")
    list(LENGTH _ssbos _ubo_binding)
    string(REGEX REPLACE "layout\\(push_constant\\)[ \t\n]*layout\\(std430\\) uniform"
           "layout(std140, binding = ${_ubo_binding}) uniform" _glsl "${_glsl}")
  endif()
  file(WRITE "${OUT}"
"#pragma once

inline constexpr const char* ${SYMBOL}_glsl = R\"glsl(${_glsl})glsl\";
")
  return()
endif()

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
  cmake_parse_arguments(ARG "" "ENTRY" "" ${ARGN})
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
    DEPENDS "${_src}" ${_modules}
    VERBATIM)

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

# slang_add_glsl(<target> <file.slang> ENTRY <name>)
#
# Header-only INTERFACE target exposing the entry point as GLSL 430 source in
# `inline constexpr const char* <target>_glsl` in <target>.h.
function(slang_add_glsl target source)
  cmake_parse_arguments(ARG "" "ENTRY" "" ${ARGN})
  if(NOT ARG_ENTRY)
    message(FATAL_ERROR "slang_add_glsl(${target}): ENTRY is required")
  endif()
  get_filename_component(_src "${source}" ABSOLUTE)
  get_filename_component(_srcdir "${_src}" DIRECTORY)
  file(GLOB _modules CONFIGURE_DEPENDS "${_srcdir}/*.slang")
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/${target}")
  set(_comp "${_gen}/${target}.comp")
  set(_hdr "${_gen}/${target}.h")
  add_custom_command(
    OUTPUT "${_hdr}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
    COMMAND "${SLANG_COMPILER}" "${_src}" -entry ${ARG_ENTRY} -stage compute
            -target glsl -profile glsl_430 -o "${_comp}"
    COMMAND ${CMAKE_COMMAND} -DGLSL_IN=${_comp} -DOUT=${_hdr} -DSYMBOL=${target}
            -P "${_SLANG_KERNELS_SELF}"
    DEPENDS "${_src}" ${_modules} "${_SLANG_KERNELS_SELF}"
    VERBATIM)
  add_custom_target(${target}_glsl_gen DEPENDS "${_hdr}")
  add_library(${target} INTERFACE)
  add_dependencies(${target} ${target}_glsl_gen)
  target_include_directories(${target} INTERFACE "${_gen}")
endfunction()
