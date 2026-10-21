/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef PERPROCESSUSERDIRS_H
#define PERPROCESSUSERDIRS_H

#include "Global/Macros.h"

#include <QDir>
#include <QString>
#include <QTemporaryDir>
#include <QtGlobal>

// ctest runs every suite as its own process, possibly several at once, and each one starts with
// --clear-cache --clear-openfx-cache: left on the shared defaults, one suite's startup would wipe
// the disk/viewer tile caches and OFXLoadCache another suite is using, and the plugin defaults
// written to Natron.conf and the autosaves directory would be shared too. XDG_CACHE_HOME is left
// alone on purpose: Mesa's shader cache and fontconfig's cache live there and gain nothing from
// starting cold in every suite.
//
// Construct before the AppManager so it outlives it: the caches hold files open until teardown.
class PerProcessUserDirs {
public:
    PerProcessUserDirs()
        : _root(QDir::tempPath() + QString::fromUtf8("/natron-tests-XXXXXX"))
    {
        if (!_root.isValid()) {
            return;
        }
        qputenv(NATRON_DISK_CACHE_PATH_ENV_VAR, _root.filePath(QString::fromUtf8("cache")).toUtf8());
        qputenv("XDG_CONFIG_HOME", _root.filePath(QString::fromUtf8("config")).toUtf8());
        qputenv("XDG_DATA_HOME", _root.filePath(QString::fromUtf8("data")).toUtf8());
    }

private:
    QTemporaryDir _root;
};

#endif // PERPROCESSUSERDIRS_H
