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

file(GLOB NatronEngine_HEADERS ../Engine/*.h)
file(GLOB NatronGui_HEADERS *.h)
file(GLOB NatronGui_SOURCES *.cpp)
# OpenUSD's headers include its Python bindings' headers, which need the full Python API, and
# libshiboken puts Py_LIMITED_API on everything that links it.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/SceneViewport.cpp ${CMAKE_CURRENT_SOURCE_DIR}/SceneGizmo.cpp ${CMAKE_CURRENT_SOURCE_DIR}/SceneGuides.cpp ${CMAKE_CURRENT_SOURCE_DIR}/SceneTransformWidget.cpp ${CMAKE_CURRENT_SOURCE_DIR}/SceneTransformTarget.cpp PROPERTIES COMPILE_OPTIONS -UPy_LIMITED_API)

set(PYGUI_OUT ${CMAKE_CURRENT_BINARY_DIR}/Qt${QT_VERSION_MAJOR})
set(PYGUI_INCLUDE_DIRS . .. ../Engine ../Global ../libs/OpenFX/include)
list(APPEND PYGUI_INCLUDE_DIRS ${PYSIDE_INCLUDE_DIRS} ${PYSIDE_INCLUDE_DIRS}/QtCore
    ${PYSIDE_INCLUDE_DIRS}/QtGui ${PYSIDE_INCLUDE_DIRS}/QtWidgets)
list(APPEND PYGUI_INCLUDE_DIRS ${QTCORE_INCLUDE_DIRS} ${QTGUI_INCLUDE_DIRS}
    ${QTWIDGETS_INCLUDE_DIRS} ${Python3_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})
list(TRANSFORM PYGUI_INCLUDE_DIRS PREPEND "-I" OUTPUT_VARIABLE PYGUI_INCS)
set(PYGUI_HEADER PySide${PYSIDE_VERSION_MAJOR}_Gui_Python.h)

set(QT_OPENGLWIDGETS Qt::OpenGLWidgets)

set(shiboken_args
    "-std=c++17" "--enable-parent-ctor-heuristic" "--use-isnull-as-nb_nonzero"
    "--avoid-protected-hack" "--enable-pyside-extensions"
    ${PYGUI_INCS}
    "-T../Engine" "-T${PYSIDE_TYPESYSTEMS}" "--output-directory=${PYGUI_OUT}"
    "${PYGUI_HEADER}" "typesystem_natronGui.xml")

execute_process(COMMAND ${Python3_EXECUTABLE} ../tools/utils/sourceList.py typesystem_natronGui.xml ${PYGUI_OUT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE PyGui_SOURCES)
set_property(SOURCE ${PyGui_SOURCES} PROPERTY SKIP_AUTOGEN ON)
add_custom_command(OUTPUT ${PyGui_SOURCES}
    COMMAND $<TARGET_FILE:${SHIBOKEN_EXE}> ${shiboken_args}
    COMMAND ${Python3_EXECUTABLE} ../tools/utils/sourceCleanup.py typesystem_natronGui.xml ${PYGUI_OUT}
    DEPENDS ${SHIBOKEN_EXE} ${PYGUI_HEADER} NatronEngine ${NatronGui_HEADERS} ../tools/utils/sourceCleanup.py typesystem_natronGui.xml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

list(APPEND NatronGui_SOURCES
    ${NatronGui_SOURCES}
    ${PyGui_SOURCES}
    GuiResources.qrc
)
add_library(NatronGui STATIC ${NatronGui_HEADERS} ${NatronGui_SOURCES})

# Adding an icon PNG to this directory is enough; no qrc edit is needed.
# CONFIGURE_DEPENDS re-runs the glob at build time so a new file triggers regeneration.
set(NATIVE_NODE_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Images/NativeNodes")
file(GLOB NATIVE_NODE_ICONS CONFIGURE_DEPENDS "${NATIVE_NODE_ICON_DIR}/*.png")
list(SORT NATIVE_NODE_ICONS)
qt_add_resources(NatronGui NativeNodes
    PREFIX "/Resources/Images/NativeNodes"
    BASE "${NATIVE_NODE_ICON_DIR}"
    FILES ${NATIVE_NODE_ICONS}
)
target_link_libraries(NatronGui
    PUBLIC
        ${SHIBOKEN_LIB}
        ${PYSIDE_LIB}
        Qt::Core
        Qt::Concurrent
        Qt::Network
        Qt::Gui
        Qt::Widgets
        ${QT_OPENGLWIDGETS}
        Qt::Svg
        Python3::Module
        NatronEngine
    PRIVATE
        qhttpserver
        ${NATRON_USD_GUI_LIBRARIES}
)
target_include_directories(NatronGui
    PRIVATE
        ${PYSIDE_INCLUDE_DIRS}/QtCore
        ${PYSIDE_INCLUDE_DIRS}/QtGui
        ${PYSIDE_INCLUDE_DIRS}/QtWidgets
        .
        ..
        ../Engine
        ../Global
        ../libs/SequenceParsing
        ../libs/qhttpserver/src
        ${CMAKE_BINARY_DIR}/Engine/Qt${QT_VERSION_MAJOR}/NatronEngine
)
target_compile_definitions(NatronGui
    PRIVATE
        NATRON_CUSTOM_BUILD_USER_TOKEN="${BUILD_USER_NAME}"
        NATRON_BUILD_NUMBER=0
        QT_NO_KEYWORDS
)
