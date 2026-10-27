# ***** BEGIN LICENSE BLOCK *****
# This file is part of Natron <https://natrongithub.github.io/>,
# (C) 2018-2023 The Natron developers
# (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
#
# Natron is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# Natron is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
# ***** END LICENSE BLOCK *****

cmake_minimum_required(VERSION 3.16.7)

project(Natron
    VERSION "2.6.0"
    DESCRIPTION "Open Source Compositing Software"
    HOMEPAGE_URL "https://natrongithub.github.io/"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)

include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(NATRON_SYSTEM_LIBS "use system versions of dependencies instead of bundled ones" OFF)
option(NATRON_BUILD_TESTS "build the Natron test suite" ON)
option(NATRON_GPU "Vulkan compute backend" ON)

if(NATRON_GPU)
    find_package(Vulkan)
    if(NOT Vulkan_FOUND)
        message(STATUS "Vulkan not found: building without the GPU backend (NATRON_GPU=OFF)")
        set(NATRON_GPU OFF)
    endif()
endif()

set(IS_DEBUG_BUILD OFF)
if(CMAKE_BUILD_TYPE MATCHES "^(debug|Debug|DEBUG)$")
    set(IS_DEBUG_BUILD ON)
    add_definitions(-DDEBUG)

    if(WIN32)
        # Debug builds need minimal optimizations to avoid excessive link times and link errors related to Eigen.
        add_compile_options(-O)
    endif()
else()
    add_definitions(-DQT_NO_DEBUG_OUTPUT)
endif()

if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Choose the type of build." FORCE)
    message(STATUS "Setting build type to '${CMAKE_BUILD_TYPE}' as none was specified.")
endif()

find_package(Boost COMPONENTS headers serialization CONFIG)
# yaml-cpp and zlib are consumed PRIVATE by the static NatronSerialization, but
# a static library's private link dependencies still reach every executable
# that links it, so the imported targets must be visible from the top level.
# The ASWF image ships yaml-cpp 0.9 with a CMake config, which satisfies 0.8.
find_package(yaml-cpp 0.8 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
if(WIN32)
    # Prevent looking in the registry so that the MinGW python can be found.
    set(Python3_FIND_REGISTRY NEVER)
endif()
find_package(Python3 COMPONENTS Interpreter Development)
# OpenUSD, for the scene data kind; checked against the Python just found.
include(NatronUsd)

# For every executable that embeds the interpreter. libshiboken exports its own PyStaticMethod_New,
# a shim that only works once the interpreter is running; when libshiboken precedes libpython in the
# executable's DT_NEEDED order, the shim interposes CPython's own during Py_Initialize() and the
# interpreter segfaults building its builtin types. The executable listing Python3::Python does not
# fix that order: CMake emits a shared library once, after the last target linking it, and OpenUSD's
# targets link Python3::Python after libshiboken. So libpython is named ahead of everything.
function(natron_link_embedded_python target)
    target_link_options(${target} BEFORE PRIVATE
        "LINKER:--push-state,--no-as-needed"
        "$<TARGET_LINKER_FILE:Python3::Python>"
        "LINKER:--pop-state")
endfunction()

if(IS_DEBUG_BUILD AND WIN32)
    # Explicitly setting SHIBOKEN_PYTHON_LIBRARIES variable to avoid PYTHON_DEBUG_LIBRARY-NOTFOUND
    # link errors on Windows debug builds.
    set(SHIBOKEN_PYTHON_LIBRARIES ${Python3_LIBRARIES})
endif()

set(QT_COMPONENTS Core Gui Network Widgets Concurrent)
list(APPEND QT_COMPONENTS OpenGLWidgets Svg)
find_package(Qt6 6.8 CONFIG REQUIRED COMPONENTS ${QT_COMPONENTS})
find_package(Shiboken6 6.8 CONFIG REQUIRED COMPONENTS libshiboken6)
find_package(Shiboken6Tools 6.8 CONFIG REQUIRED COMPONENTS shiboken6)
find_package(PySide6 6.8 CONFIG REQUIRED COMPONENTS pyside6)
set(QT_VERSION_MAJOR 6)
set(PYSIDE_VERSION_MAJOR 6)

# The ASWF image's HarfBuzz config exports harfbuzz::harfbuzz but no package
# version file, so requesting a version rejects an otherwise usable config.
# Found here (rather than in Engine/CMakeLists.txt, where it is consumed) so
# the imported target is visible in every directory that links NatronEngine:
# find_package() imported targets are scoped to the directory that finds
# them (and its subdirectories), and Renderer/Gui/Tests/App are siblings of
# Engine, not descendants of it.
find_package(harfbuzz CONFIG REQUIRED)

# rawspeed is built and installed by tools/ci/local/fetch-assets.sh, which writes the package
# config found here. Found at the top level for the same reason as harfbuzz: NatronEngine links it
# statically, so every executable linking NatronEngine links it too.
set(NATRON_RAWSPEED_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/assets/rawspeed" CACHE PATH "The rawspeed install fetch-assets.sh produces")
if(NOT EXISTS "${NATRON_RAWSPEED_ROOT}/lib/cmake/rawspeed/rawspeedConfig.cmake")
    message(FATAL_ERROR "rawspeed is not installed at ${NATRON_RAWSPEED_ROOT}. Run tools/ci/local/fetch-assets.sh first.")
endif()
find_package(rawspeed CONFIG REQUIRED PATHS "${NATRON_RAWSPEED_ROOT}" NO_DEFAULT_PATH)
# fetch-assets.sh rewrites the config when the pin moves; rawspeed_REF must follow it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NATRON_RAWSPEED_ROOT}/lib/cmake/rawspeed/rawspeedConfig.cmake")

# ONNX Runtime is unpacked by tools/ci/local/fetch-assets.sh. Only its C API headers are used at
# build time: Engine/Ai dlopen()s the library, which tools/release/stage-bundle.sh ships.
set(NATRON_ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/assets/onnxruntime" CACHE PATH "The ONNX Runtime release fetch-assets.sh unpacks")
if(NOT EXISTS "${NATRON_ONNXRUNTIME_ROOT}/include/onnxruntime_c_api.h")
    message(FATAL_ERROR "ONNX Runtime is not unpacked at ${NATRON_ONNXRUNTIME_ROOT}. Run tools/ci/local/fetch-assets.sh first.")
endif()
add_library(onnxruntime::headers INTERFACE IMPORTED GLOBAL)
set_target_properties(onnxruntime::headers PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${NATRON_ONNXRUNTIME_ROOT}/include"
    INTERFACE_INCLUDE_DIRECTORIES "${NATRON_ONNXRUNTIME_ROOT}/include")

get_target_property(PYSIDE_INCLUDE_DIRS PySide6::pyside6 INTERFACE_INCLUDE_DIRECTORIES)
set(QTCORE_INCLUDE_DIRS ${Qt6Core_INCLUDE_DIRS})
set(QTGUI_INCLUDE_DIRS ${Qt6Gui_INCLUDE_DIRS})
set(QTWIDGETS_INCLUDE_DIRS ${Qt6Widgets_INCLUDE_DIRS})

set(SHIBOKEN_EXE Shiboken6::shiboken6)
set(SHIBOKEN_LIB Shiboken6::libshiboken)
set(PYSIDE_LIB PySide6::pyside6)

if(IS_DEBUG_BUILD)
    # The installed Shiboken6 package exports NDEBUG as an interface definition, which would
    # silence assert() in every target linking it.
    get_property(SHIBOKEN_DEFS TARGET ${SHIBOKEN_LIB} PROPERTY INTERFACE_COMPILE_DEFINITIONS)
    list(REMOVE_ITEM SHIBOKEN_DEFS NDEBUG)
    set_property(TARGET ${SHIBOKEN_LIB} PROPERTY INTERFACE_COMPILE_DEFINITIONS "${SHIBOKEN_DEFS}")
endif()

#Since in Natron and OpenFX all strings are supposed UTF-8 and that the constructor
#for QString(const char*) assumes ASCII strings, we may run into troubles
add_compile_definitions(QT_NO_CAST_FROM_ASCII)

if (MSVC)
    # Allow __cplusplus to properly reflect the c++ standard version.
    add_compile_options(/Zc:__cplusplus)
    add_compile_definitions(__STDC_LIMIT_MACROS WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
else()
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wmissing-prototypes" )
    add_compile_options(-Wall -Wextra -Wmissing-declarations -Wno-multichar -Winit-self -Wno-long-long
                        -Wvla -Wdate-time -Wno-format-overflow -Wno-deprecated-copy)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wshift-overflow=2 -Wduplicated-cond -Wno-stringop-overflow)
    endif()
endif()

if(NATRON_SYSTEM_LIBS)
    find_package(glog)
    find_package(Ceres)
endif()

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
elseif(APPLE)
    enable_language(OBJCXX)
elseif(WIN32)
    enable_language(RC)
    add_compile_definitions(WINDOWS WIN32 _UNICODE UNICODE NOMINMAX QHTTP_SERVER_STATIC)
    add_compile_definitions(COMPILED_FROM_DSP XML_STATIC) # for expat
    if (NOT MSVC)
        add_compile_options(-municode)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_compile_options(-mwindows -mthreads)
        endif()
        add_link_options(-mwindows -municode)
        set(CMAKE_RC_COMPILER_INIT windres)
        set(CMAKE_RC_COMPILE_OBJECT
            "<CMAKE_RC_COMPILER> <FLAGS> -O coff <DEFINES> -i <SOURCE> -o <OBJECT>")
    endif()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

enable_testing()

add_subdirectory(libs)

add_subdirectory(HostSupport)

add_subdirectory(Serialization)

add_subdirectory(Engine)

add_subdirectory(PythonBin)

add_subdirectory(Renderer)

add_subdirectory(Gui)

if(NATRON_BUILD_TESTS)
    add_subdirectory(Tests)
endif()

add_subdirectory(App)

include(cmake/NatronBundleAssets.cmake)
