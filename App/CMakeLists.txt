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

set(Natron_SOURCES NatronApp_main.cpp)
add_executable(Natron ${Natron_SOURCES})
if(APPLE)
    set_target_properties(Natron PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}"
        MACOSX_BUNDLE_INFO_STRING "${PROJECT_NAME} Copyright (c) 2023 The Natron Developers"
        MACOSX_BUNDLE_ICON_FILE Icon.icns
        MACOSX_BUNDLE_GUI_IDENTIFIER "${PROJECT_NAME}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (c) 2023 The Natron Developers"
    )
endif()
target_link_libraries(Natron
    PRIVATE
        NatronGui
        Python3::Python
)
natron_link_embedded_python(Natron)
target_include_directories(Natron
    PRIVATE
        ..
        ../Gui
        ../Engine
        ../Global
)

# TabMenuBridge::bootstrap() looks for the vendored tabtabtab files at
# <bin>/../Resources/tabtabtab, so a dev build at build/fast/App/Natron needs
# them at build/fast/Resources/tabtabtab. Natron depends on the copy, rather
# than the copy running after Natron, so building just the Natron target also
# refreshes edited scripts when nothing relinks. The destination is spelled
# from Natron's default output directory: $<TARGET_FILE_DIR:Natron> would make
# the copy depend on Natron and form a cycle. No install() rule is added, as
# for PyPlugs: an FHS prefix would put this under /usr/Resources.
set(NatronTabTabTab_SOURCE_DIR ${CMAKE_SOURCE_DIR}/Gui/Resources/TabTabTab)
set(NatronTabTabTab_STAMP ${CMAKE_CURRENT_BINARY_DIR}/NatronTabTabTabResources.stamp)
file(GLOB_RECURSE NatronTabTabTab_FILES CONFIGURE_DEPENDS ${NatronTabTabTab_SOURCE_DIR}/*)
list(FILTER NatronTabTabTab_FILES EXCLUDE REGEX "/__pycache__/")
add_custom_command(
    OUTPUT ${NatronTabTabTab_STAMP}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${NatronTabTabTab_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/../Resources/tabtabtab
    COMMAND ${CMAKE_COMMAND} -E touch ${NatronTabTabTab_STAMP}
    DEPENDS ${NatronTabTabTab_FILES}
    COMMENT "Copying tabtabtab resources"
    VERBATIM
)
add_custom_target(NatronTabTabTabResources DEPENDS ${NatronTabTabTab_STAMP})
add_dependencies(Natron NatronTabTabTabResources)

install(TARGETS Natron DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES ../Gui/Resources/Applications/fr.natron.Natron.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES ../Gui/Resources/Metainfo/fr.natron.Natron.appdata.xml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
install(FILES ../Gui/Resources/Mime/x-natron.xml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mime")
install(FILES ../Gui/Resources/Images/natronIcon256_linux.png
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps")
install(FILES ../Gui/Resources/Images/natronProjectIcon_linux.png
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps")
