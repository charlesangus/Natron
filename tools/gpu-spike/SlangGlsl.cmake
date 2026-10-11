# slang_add_glsl(<target> <file.slang> ENTRY <name>)
#
# Header-only INTERFACE target exposing the entry point as GLSL 430 source in
# `inline constexpr const char* <target>_glsl` in <target>.h. Needs
# SLANG_COMPILER from FetchSlang.cmake.

if(CMAKE_SCRIPT_MODE_FILE)
  # Script mode: `cmake -DGLSL_IN=.. -DOUT=.. -DSYMBOL=.. -P SlangGlsl.cmake`
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

set(_SLANG_GLSL_SELF "${CMAKE_CURRENT_LIST_FILE}")

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
            -P "${_SLANG_GLSL_SELF}"
    DEPENDS "${_src}" ${_modules} "${_SLANG_GLSL_SELF}"
    VERBATIM)
  add_custom_target(${target}_glsl_gen DEPENDS "${_hdr}")
  add_library(${target} INTERFACE)
  add_dependencies(${target} ${target}_glsl_gen)
  target_include_directories(${target} INTERFACE "${_gen}")
endfunction()
