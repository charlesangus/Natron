# Resolves the Slang compiler and prelude headers into SLANG_ROOT,
# SLANG_COMPILER and SLANG_INCLUDE_DIR. Only slangc and the headers are used;
# libslang is never linked into a spike binary.

set(GPU_SPIKE_SLANG_VERSION "2026.19")
set(GPU_SPIKE_SLANG_SHA256
    "bd6cfc47b7353b2cffa36a866b1584749f76f4b2dbebf6dda26bc3fb40dc3c0e")
set(GPU_SPIKE_SLANG_URL
    "https://github.com/shader-slang/slang/releases/download/v${GPU_SPIKE_SLANG_VERSION}/slang-${GPU_SPIKE_SLANG_VERSION}-linux-x86_64.tar.gz")

function(_gpu_spike_use_slang_root root)
  find_program(_slangc slangc PATHS "${root}/bin" NO_DEFAULT_PATH)
  find_path(_slang_inc slang-cpp-prelude.h PATHS "${root}/include" NO_DEFAULT_PATH)
  if(NOT _slangc OR NOT _slang_inc)
    message(FATAL_ERROR "SLANG_ROOT '${root}' lacks bin/slangc or include/slang-cpp-prelude.h")
  endif()
  set(SLANG_ROOT "${root}" PARENT_SCOPE)
  set(SLANG_COMPILER "${_slangc}" PARENT_SCOPE)
  set(SLANG_INCLUDE_DIR "${_slang_inc}" PARENT_SCOPE)
endfunction()

if(NOT SLANG_ROOT AND DEFINED ENV{SLANG_ROOT})
  set(SLANG_ROOT "$ENV{SLANG_ROOT}")
endif()

if(SLANG_ROOT)
  _gpu_spike_use_slang_root("${SLANG_ROOT}")
else()
  if(DEFINED ENV{NATRON_DEPS_CACHE} AND NOT "$ENV{NATRON_DEPS_CACHE}" STREQUAL "")
    set(_cache "$ENV{NATRON_DEPS_CACHE}")
  else()
    set(_cache "$ENV{HOME}/.cache/natron-deps")
  endif()
  set(_root "${_cache}/slang-${GPU_SPIKE_SLANG_VERSION}")

  if(NOT EXISTS "${_root}/bin/slangc")
    set(_tarball "${_cache}/slang-${GPU_SPIKE_SLANG_VERSION}-linux-x86_64.tar.gz")
    message(STATUS "Downloading Slang ${GPU_SPIKE_SLANG_VERSION} to ${_cache}")
    file(MAKE_DIRECTORY "${_cache}")
    file(DOWNLOAD "${GPU_SPIKE_SLANG_URL}" "${_tarball}"
         EXPECTED_HASH SHA256=${GPU_SPIKE_SLANG_SHA256}
         SHOW_PROGRESS)
    # Extract beside the final directory and rename so an interrupted
    # extraction is never mistaken for a complete install.
    set(_stage "${_root}.partial")
    file(REMOVE_RECURSE "${_stage}")
    file(MAKE_DIRECTORY "${_stage}")
    file(ARCHIVE_EXTRACT INPUT "${_tarball}" DESTINATION "${_stage}")
    file(REMOVE_RECURSE "${_root}")
    file(RENAME "${_stage}" "${_root}")
  endif()
  _gpu_spike_use_slang_root("${_root}")
endif()

message(STATUS "Slang: ${SLANG_COMPILER} (headers ${SLANG_INCLUDE_DIR})")
