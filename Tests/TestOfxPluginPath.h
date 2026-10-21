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

#ifndef TESTOFXPLUGINPATH_H
#define TESTOFXPLUGINPATH_H

#include "Global/Macros.h"

#include <QByteArray>
#include <QDir>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#ifndef NATRON_TEST_OFX_PLUGIN_DIR
#error "NATRON_TEST_OFX_PLUGIN_DIR must name the directory the test-only OFX bundles are built into"
#endif

// The test-only OFX bundles live in their own directory, which the test mains append to
// OFX_PLUGIN_PATH. Tests that ask whether the shipped bundles were put on the path look at what
// is left once that directory is taken out.

inline QString
testOfxPluginDir()
{
    return QDir::cleanPath(QString::fromUtf8(NATRON_TEST_OFX_PLUGIN_DIR));
}

inline QStringList
ofxPluginPathEntries()
{
    return QString::fromLocal8Bit(qgetenv("OFX_PLUGIN_PATH")).split(QDir::listSeparator(), Qt::SkipEmptyParts);
}

// OFX_PLUGIN_PATH as the user or the test runner set it, without the test bundle directory.
inline QString
userOfxPluginPath()
{
    const QString testDir = testOfxPluginDir();
    QStringList userEntries;

    for (const QString& entry : ofxPluginPathEntries()) {
        if (QDir::cleanPath(entry) != testDir) {
            userEntries << entry;
        }
    }

    return userEntries.join(QDir::listSeparator());
}

// Must run before AppManager::load(): the OFX plug-in cache reads OFX_PLUGIN_PATH once, when the
// plug-ins are first loaded. A value already set is kept, since some tests check that it names
// exactly the shipped bundles.
inline void
appendTestOfxPluginDir()
{
    const QString testDir = testOfxPluginDir();
    QStringList entries = ofxPluginPathEntries();

    for (const QString& entry : entries) {
        if (QDir::cleanPath(entry) == testDir) {
            return;
        }
    }
    entries << testDir;
    qputenv("OFX_PLUGIN_PATH", entries.join(QDir::listSeparator()).toLocal8Bit());
}

#endif // TESTOFXPLUGINPATH_H
