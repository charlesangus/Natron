# Resolves the glslang source tree into GLSLANG_ROOT and builds it as static,
# position-independent libraries (targets glslang and
# glslang-default-resource-limits). The SPIR-V optimizer is left out: the
# driver optimizes, and SPIRV-Tools would add roughly 10 MB.

set(NATRON_GLSLANG_VERSION "16.6.0")
set(NATRON_GLSLANG_SHA256
    "9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2")
set(NATRON_GLSLANG_URL
    "https://github.com/KhronosGroup/glslang/archive/refs/tags/${NATRON_GLSLANG_VERSION}.tar.gz")

function(_natron_glslang_use_root root)
  if(NOT EXISTS "${root}/CMakeLists.txt" OR NOT EXISTS "${root}/glslang/Public/ShaderLang.h")
    message(FATAL_ERROR "GLSLANG_ROOT '${root}' is not a glslang source tree")
  endif()
  set(GLSLANG_ROOT "${root}" PARENT_SCOPE)
endfunction()

if(NOT GLSLANG_ROOT AND DEFINED ENV{GLSLANG_ROOT})
  set(GLSLANG_ROOT "$ENV{GLSLANG_ROOT}")
endif()

if(GLSLANG_ROOT)
  _natron_glslang_use_root("${GLSLANG_ROOT}")
else()
  if(DEFINED ENV{NATRON_DEPS_CACHE} AND NOT "$ENV{NATRON_DEPS_CACHE}" STREQUAL "")
    set(_cache "$ENV{NATRON_DEPS_CACHE}")
  else()
    # Generated targets bake this absolute path, and containers sharing one
    # build dir have different HOMEs, so the default lives in the repo.
    get_filename_component(_cache "${CMAKE_CURRENT_LIST_DIR}/../.natron-deps" ABSOLUTE)
  endif()
  set(_root "${_cache}/glslang-${NATRON_GLSLANG_VERSION}")

  if(NOT EXISTS "${_root}/CMakeLists.txt")
    set(_tarball "${_cache}/glslang-${NATRON_GLSLANG_VERSION}.tar.gz")
    message(STATUS "Downloading glslang ${NATRON_GLSLANG_VERSION} to ${_cache}")
    file(MAKE_DIRECTORY "${_cache}")
    file(DOWNLOAD "${NATRON_GLSLANG_URL}" "${_tarball}"
         EXPECTED_HASH SHA256=${NATRON_GLSLANG_SHA256}
         SHOW_PROGRESS)
    # Extract beside the final directory and rename so an interrupted
    # extraction is never mistaken for a complete one.
    set(_stage "${_root}.partial")
    file(REMOVE_RECURSE "${_stage}")
    file(MAKE_DIRECTORY "${_stage}")
    file(ARCHIVE_EXTRACT INPUT "${_tarball}" DESTINATION "${_stage}")
    file(REMOVE_RECURSE "${_root}")
    # GitHub archives wrap everything in one glslang-<tag> directory.
    file(RENAME "${_stage}/glslang-${NATRON_GLSLANG_VERSION}" "${_root}")
    file(REMOVE_RECURSE "${_stage}")
  endif()
  _natron_glslang_use_root("${_root}")
endif()

# A function scope keeps these switches from leaking into the rest of the tree.
function(_natron_glslang_add root)
  set(BUILD_SHARED_LIBS OFF)
  set(ENABLE_OPT OFF)
  set(ENABLE_HLSL OFF)
  set(ENABLE_GLSLANG_BINARIES OFF)
  set(GLSLANG_TESTS OFF)
  set(GLSLANG_ENABLE_INSTALL OFF)
  set(BUILD_EXTERNAL OFF)
  add_subdirectory("${root}" "${CMAKE_BINARY_DIR}/_deps/glslang" EXCLUDE_FROM_ALL)
endfunction()

if(NOT TARGET glslang)
  _natron_glslang_add("${GLSLANG_ROOT}")
endif()

message(STATUS "glslang: ${GLSLANG_ROOT}")
