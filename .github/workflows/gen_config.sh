#!/usr/bin/env bash
# ***** BEGIN LICENSE BLOCK *****
# This file is part of Natron <http://www.natron.fr/>,
# Copyright (C) 2018-2023 The Natron developers
# Copyright (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
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

# config.pri
if [[ ${RUNNER_OS} == "Linux" ]]; then
    # Ubuntu 12.04 precise doesn't have a pkg-config file for expat (expat.pc)
    {
        echo 'boost: LIBS += -lboost_serialization'
        echo 'expat: LIBS += -lexpat'
        echo 'expat: PKGCONFIG -= expat'
        # pyside and shiboken for python3 cannot be configured with pkg-config on Ubuntu 12.04LTS Precise
        # pyside and shiboken for python2 still need the extra QtCore and QtGui include directories
        # shellcheck disable=SC2016 # $$ is qmake variable syntax, not a shell variable; must stay unexpanded
        echo 'PYSIDE_PKG_CONFIG_PATH = $$system($$PYTHON_CONFIG --prefix)/lib/pkgconfig'
        echo 'pyside: PKGCONFIG -= pyside'
        # shellcheck disable=SC2016 # $$ is qmake variable syntax, not a shell variable; must stay unexpanded
        echo 'pyside: INCLUDEPATH += $$system(env PKG_CONFIG_PATH=$$PYSIDE_PKG_CONFIG_PATH pkg-config --variable=includedir pyside)'
        # shellcheck disable=SC2016 # $$ is qmake variable syntax, not a shell variable; must stay unexpanded
        echo 'pyside: INCLUDEPATH += $$system(env PKG_CONFIG_PATH=$$PYSIDE_PKG_CONFIG_PATH pkg-config --variable=includedir pyside)/QtCore'
        # shellcheck disable=SC2016 # $$ is qmake variable syntax, not a shell variable; must stay unexpanded
        echo 'pyside: INCLUDEPATH += $$system(env PKG_CONFIG_PATH=$$PYSIDE_PKG_CONFIG_PATH pkg-config --variable=includedir pyside)/QtGui'
        # shellcheck disable=SC2016 # $$ is qmake variable syntax, not a shell variable; must stay unexpanded
        echo 'pyside: INCLUDEPATH += $$system(env PKG_CONFIG_PATH=$$PYSIDE_PKG_CONFIG_PATH pkg-config --variable=includedir QtGui)'
        #echo 'pyside: LIBS += -lpyside.cpython-32mu'
        echo 'pyside: LIBS += -lpyside-python2.7'
        # pyside doesn't have PySide::getWrapperForQObject on Ubuntu 12.04LTS Precise
        echo 'pyside: DEFINES += PYSIDE_OLD'
        #echo 'shiboken: PKGCONFIG -= shiboken'
        #echo 'shiboken: INCLUDEPATH += $$system(pkg-config --variable=includedir shiboken)'
        #echo 'shiboken: LIBS += -lshiboken.cpython-32mu'
    } > config.pri
elif [[ ${RUNNER_OS} == "Linux" ]]; then
    {
        echo 'boost: INCLUDEPATH += /usr/local/include'
        echo 'boost: LIBS += -L/usr/local/lib -lboost_serialization-mt -lboost_thread-mt -lboost_system-mt'
        echo 'expat: PKGCONFIG -= expat'
        echo 'expat: INCLUDEPATH += /usr/local/opt/expat/include'
        echo 'expat: LIBS += -L/usr/local/opt/expat/lib -lexpat'
    } > config.pri
fi
