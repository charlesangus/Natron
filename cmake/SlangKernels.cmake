# slang_add_kernel(<target> <file.slang> ENTRY <name> [REGISTRY <name>] [CPP] [FLAGS <flag>...])
#
# Compiles one compute entry point to SPIR-V and exposes it through the header
# INTERFACE library <target>: <target>_spirv.h defines
# `inline constexpr uint32_t <target>_spirv[]`, `<target>_spirv_words` and
# `<target>_group_size[3]` (the numthreads values, read from the reflection
# JSON written beside it). The generated include dir is public on the target.
#
# The header also defines `<target>_entry`, `<target>_storage_buffers` and
# `<target>_push_bytes`. They come from the reflection JSON rather than from
# arguments so the shader stays the single source of truth: a binding or
# push-constant field added to the .slang can never drift from a hand-written
# count. For a REGISTRY kernel, reflection that does not fit the GpuKernel
# convention (anything but storage buffers in set 0 plus one push-constant
# block) fails the build; other kernels get best-effort counts.
#
# REGISTRY <name> adds the kernel to the table written by
# slang_write_kernel_table(<name> OUTPUT <path>), which emits an X-macro style
# GpuKernels.inc: one enum of kernel ids and one static GpuKernelDesc table.
#
# FLAGS are passed to slangc for both targets, e.g. `FLAGS -fp-mode precise` to
# forbid the contractions that would part a kernel from its unfused C++ mirror.
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

  set(_buffers 0)
  set(_maxIndex -1)
  set(_push 0)
  string(JSON _nparams LENGTH "${_json}" parameters)
  if(_nparams GREATER 0)
    math(EXPR _last "${_nparams} - 1")
    foreach(_i RANGE ${_last})
      string(JSON _kind GET "${_json}" parameters ${_i} binding kind)
      string(JSON _tkind GET "${_json}" parameters ${_i} type kind)
      string(JSON _pname GET "${_json}" parameters ${_i} name)
      if(_kind STREQUAL "descriptorTableSlot" AND NOT _tkind STREQUAL "constantBuffer")
        string(JSON _idx GET "${_json}" parameters ${_i} binding index)
        math(EXPR _buffers "${_buffers} + 1")
        if(_idx GREATER _maxIndex)
          set(_maxIndex ${_idx})
        endif()
      elseif(_kind STREQUAL "pushConstantBuffer")
        string(JSON _nfields LENGTH "${_json}" parameters ${_i} type elementType fields)
        if(_nfields GREATER 0)
          math(EXPR _lastField "${_nfields} - 1")
          foreach(_f RANGE ${_lastField})
            string(JSON _off GET "${_json}" parameters ${_i} type elementType fields ${_f} binding offset)
            string(JSON _sz GET "${_json}" parameters ${_i} type elementType fields ${_f} binding size)
            math(EXPR _end "${_off} + ${_sz}")
            if(_end GREATER _push)
              set(_push ${_end})
            endif()
          endforeach()
        endif()
      elseif(STRICT)
        message(FATAL_ERROR "${IN}: parameter '${_pname}' (${_kind}/${_tkind}) is not a storage buffer or the push-constant block")
      endif()
    endforeach()
  endif()
  math(EXPR _expected "${_maxIndex} + 1")
  if(STRICT AND NOT _buffers EQUAL _expected)
    message(FATAL_ERROR "${IN}: storage buffer bindings are not contiguous from 0")
  endif()
  math(EXPR _push "(${_push} + 3) / 4 * 4")

  file(WRITE "${OUT}"
"#pragma once
#include <cstddef>
#include <cstdint>

inline constexpr uint32_t ${SYMBOL}_spirv[] = {${_words}};
inline constexpr std::size_t ${SYMBOL}_spirv_words = ${_count};
inline constexpr uint32_t ${SYMBOL}_group_size[3] = {${_gx}u, ${_gy}u, ${_gz}u};
// Slang names the SPIR-V entry point main regardless of the source name.
inline constexpr const char* ${SYMBOL}_entry = \"main\";
inline constexpr uint32_t ${SYMBOL}_storage_buffers = ${_buffers}u;
inline constexpr uint32_t ${SYMBOL}_push_bytes = ${_push}u;
")
  return()
endif()

set(_SLANG_KERNELS_SELF "${CMAKE_CURRENT_LIST_FILE}")

function(slang_add_kernel target source)
  cmake_parse_arguments(ARG "CPP" "ENTRY;REGISTRY" "FLAGS" ${ARGN})
  if(NOT ARG_ENTRY)
    message(FATAL_ERROR "slang_add_kernel(${target}): ENTRY is required")
  endif()
  if(ARG_REGISTRY)
    set_property(GLOBAL APPEND PROPERTY SLANG_REGISTRY_${ARG_REGISTRY} "${target}")
  endif()
  get_filename_component(_src "${source}" ABSOLUTE)
  get_filename_component(_srcdir "${_src}" DIRECTORY)
  file(GLOB _modules CONFIGURE_DEPENDS "${_srcdir}/*.slang")
  set(_strict OFF)
  if(ARG_REGISTRY)
    set(_strict ON)
  endif()
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/${target}")
  set(_spv "${_gen}/${target}.spv")
  set(_hdr "${_gen}/${target}_spirv.h")
  set(_cpp "${_gen}/${target}_cpu.cpp")
  set(_json "${_gen}/${target}.reflection.json")

  add_custom_command(
    OUTPUT "${_hdr}" "${_json}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
    COMMAND "${SLANG_COMPILER}" "${_src}" -entry ${ARG_ENTRY} -stage compute
            -target spirv -profile spirv_1_5 ${ARG_FLAGS} -o "${_spv}"
            -reflection-json "${_json}"
    COMMAND ${CMAKE_COMMAND} -DIN=${_spv} -DOUT=${_hdr} -DSYMBOL=${target} -DJSON=${_json}
            -DSTRICT=${_strict}
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
            -target cpp ${ARG_FLAGS} -o "${_cpp}"
    DEPENDS "${_src}" ${_modules}
    VERBATIM)

  add_library(${target} STATIC "${_cpp}" "${_hdr}")
  target_include_directories(${target} PUBLIC "${_gen}")
  target_include_directories(${target} SYSTEM PUBLIC "${SLANG_INCLUDE_DIR}")
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

# slang_write_kernel_table(<registry> OUTPUT <path>)
#
# Writes <path> from the kernels added with REGISTRY <registry>. It is written at
# configure time because it only names symbols the per-kernel headers define at
# build time, and only when the content changes so builds are not retriggered.
# The file is included twice by GpuKernelRegistry: with GPU_KERNELS_ENUM defined
# for the id enumerators, and with GPU_KERNELS_TABLE defined at global scope
# for the headers and the desc table.
function(slang_write_kernel_table registry)
  cmake_parse_arguments(ARG "" "OUTPUT" "" ${ARGN})
  if(NOT ARG_OUTPUT)
    message(FATAL_ERROR "slang_write_kernel_table(${registry}): OUTPUT is required")
  endif()
  get_property(_kernels GLOBAL PROPERTY SLANG_REGISTRY_${registry})
  set(_enum "")
  set(_inc "")
  set(_rows "")
  foreach(_k IN LISTS _kernels)
    string(APPEND _enum "    ${_k},\n")
    string(APPEND _inc "#include \"${_k}_spirv.h\"\n")
    string(APPEND _rows
"    gpu::GpuKernelDesc { ${_k}_spirv, ${_k}_spirv_words, ${_k}_entry, ${_k}_storage_buffers, ${_k}_push_bytes,
                         { ${_k}_group_size[0], ${_k}_group_size[1], ${_k}_group_size[2] } },\n")
  endforeach()
  set(_content
"// Generated by slang_write_kernel_table(${registry}).
#if defined(GPU_KERNELS_ENUM)
${_enum}#elif defined(GPU_KERNELS_TABLE)
${_inc}
inline const gpu::GpuKernelDesc kGpuKernelTable[] = {
${_rows}};
#endif
")
  set(_old "")
  if(EXISTS "${ARG_OUTPUT}")
    file(READ "${ARG_OUTPUT}" _old)
  endif()
  if(NOT _old STREQUAL _content)
    get_filename_component(_dir "${ARG_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${ARG_OUTPUT}" "${_content}")
  endif()
endfunction()
