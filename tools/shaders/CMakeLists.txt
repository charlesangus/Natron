# Standalone build of the Cycles manifest dump tool. It is not part of Natron's build: Natron reads
# the committed manifests and needs no renderer. Configure it inside the dev image, where hdCycles
# is installed, against the Cycles headers of the commit hdCycles was built from, which the image
# keeps in /usr/local/share/cycles-src:
#
#   cmake -S tools/shaders -B build/shader-tools -DCYCLES_SOURCE_DIR=/usr/local/share/cycles-src
#   cmake --build build/shader-tools
#   build/shader-tools/dump_cycles_manifest --library /usr/local/plugin/usd/hdCycles.so \
#       --source /usr/local/plugin/usd/hdCycles/SOURCE \
#       --output Engine/Nodes/Scene/Shaders/manifest/cycles.json
#
# Only Cycles' headers are used; the registry itself is the one compiled into hdCycles.so.

cmake_minimum_required(VERSION 3.20)
project(NatronShaderTools LANGUAGES CXX)

set(CYCLES_SOURCE_DIR "/usr/local/share/cycles-src" CACHE PATH "Cycles headers at the commit hdCycles was built from")
if(NOT EXISTS "${CYCLES_SOURCE_DIR}/src/graph/node_type.h")
    message(FATAL_ERROR "CYCLES_SOURCE_DIR must name a Cycles source or header tree (src/graph/node_type.h)")
endif()

find_path(OIIO_INCLUDE_DIR OpenImageIO/ustring.h HINTS /usr/local/include REQUIRED)
find_library(OIIO_UTIL_LIBRARY OpenImageIO_Util HINTS /usr/local/lib /usr/local/lib64 REQUIRED)

add_executable(dump_cycles_manifest dump_cycles_manifest.cpp)
target_compile_features(dump_cycles_manifest PRIVATE cxx_std_20)
target_compile_definitions(dump_cycles_manifest PRIVATE
    "CCL_NAMESPACE_BEGIN=namespace ccl {"
    "CCL_NAMESPACE_END=}")
target_include_directories(dump_cycles_manifest PRIVATE "${CYCLES_SOURCE_DIR}/src" "${OIIO_INCLUDE_DIR}")
target_link_libraries(dump_cycles_manifest PRIVATE "${OIIO_UTIL_LIBRARY}" ${CMAKE_DL_LIBS})
