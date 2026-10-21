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

# OFX plug-in bundles that only the test binaries load. They are built into
# NATRON_TEST_OFX_PLUGIN_DIR, which the test mains append to OFX_PLUGIN_PATH.
# Nothing here has an install() rule, and the release staging only copies
# build/assets/Plugins, so none of these bundles can ship.

# The top-level DEBUG define makes the Support layer open ofxTestLog.txt in the
# working directory of whichever test first logs.
remove_definitions(-DDEBUG)

set(NATRON_OFX_SUPPORT_DIR ${CMAKE_SOURCE_DIR}/libs/OpenFX/Support)

# The in-repo OFX C++ Support layer, compiled once for every bundle that uses it.
# OFXS_MANUAL_REGISTRATION makes each such bundle define exactly one
# OFX::Plugin::getPluginIDs(), which lists its factories in a fixed order.
add_library(NatronTestOfxSupport OBJECT
    ${NATRON_OFX_SUPPORT_DIR}/Library/ofxsCore.cpp
    ${NATRON_OFX_SUPPORT_DIR}/Library/ofxsImageEffect.cpp
    ${NATRON_OFX_SUPPORT_DIR}/Library/ofxsInteract.cpp
    ${NATRON_OFX_SUPPORT_DIR}/Library/ofxsLog.cpp
    ${NATRON_OFX_SUPPORT_DIR}/Library/ofxsMultiThread.cpp
    ${NATRON_OFX_SUPPORT_DIR}/Library/ofxsParams.cpp
    ${NATRON_OFX_SUPPORT_DIR}/Library/ofxsProperty.cpp
    ${NATRON_OFX_SUPPORT_DIR}/Library/ofxsPropertyValidation.cpp
)
set_target_properties(NatronTestOfxSupport PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(NatronTestOfxSupport
    PUBLIC
        ${CMAKE_SOURCE_DIR}/libs/OpenFX/include
        ${NATRON_OFX_SUPPORT_DIR}/include
    PRIVATE
        ${NATRON_OFX_SUPPORT_DIR}/Library
)
# Every translation unit that includes the Support headers must see the same
# extension set: it changes the layout of the action argument structs.
target_compile_definitions(NatronTestOfxSupport
    PUBLIC
        OFX_EXTENSIONS_NUKE
        OFX_EXTENSIONS_NATRON
        OFXS_MANUAL_REGISTRATION
)
# Third-party code, kept as upstream wrote it.
target_compile_options(NatronTestOfxSupport PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-w>)

# natron_add_test_ofx_bundle(<name> SOURCES <src>...)
#
# Builds <name>.ofx into NATRON_TEST_OFX_PLUGIN_DIR/<name>.ofx.bundle/Contents/Linux-x86-64/
# and makes the test binaries depend on it. Only the OFX C API headers are on the
# include path; a bundle built on the Support layer links NatronTestOfxSupport itself.
function(natron_add_test_ofx_bundle name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES")
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "natron_add_test_ofx_bundle(${name}): no SOURCES given")
    endif()

    add_library(${name} MODULE ${ARG_SOURCES})
    set(bundle_binary_dir "${NATRON_TEST_OFX_PLUGIN_DIR}/${name}.ofx.bundle/Contents/Linux-x86-64")
    set_target_properties(${name} PROPERTIES
        PREFIX ""
        SUFFIX ".ofx"
        LIBRARY_OUTPUT_DIRECTORY "${bundle_binary_dir}"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    # Multi-config generators would otherwise add a per-configuration subdirectory,
    # which the OFX host does not look in.
    foreach(config ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${config}" config_upper)
        set_target_properties(${name} PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${bundle_binary_dir}")
    endforeach()
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/libs/OpenFX/include)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        target_link_options(${name} PRIVATE "LINKER:--version-script=${NATRON_OFX_SUPPORT_DIR}/include/linuxSymbols")
    endif()

    foreach(test_target Tests GuiTests)
        if(TARGET ${test_target})
            add_dependencies(${test_target} ${name})
        endif()
    endforeach()
endfunction()

file(GLOB NATRON_TEST_OFX_BUNDLE_LISTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt)
list(SORT NATRON_TEST_OFX_BUNDLE_LISTS)
foreach(bundle_list ${NATRON_TEST_OFX_BUNDLE_LISTS})
    get_filename_component(bundle_dir "${bundle_list}" DIRECTORY)
    add_subdirectory(${bundle_dir})
endforeach()
