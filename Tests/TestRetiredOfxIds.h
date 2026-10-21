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

#ifndef TESTRETIREDOFXIDS_H
#define TESTRETIREDOFXIDS_H

#include "Engine/OfxHost.h"

#include <list>
#include <string>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

// Comma-separated OFX plug-in IDs the Tests binary retires on top of the shipped markers. The OFX
// plug-ins are loaded once per process, and the test bundle's plug-ins are fixtures for other
// tests, so OfxRetired_Test retires one in a child Tests process that it starts with this set,
// rather than in the process every other test shares. Read by the test main only, so the shipped
// binary never looks at it.
#define NATRON_TEST_RETIRED_OFX_IDS_ENV_VAR "NATRON_TEST_RETIRED_OFX_IDS"

inline QStringList
testRetiredOfxIds()
{
    return QString::fromUtf8(qgetenv(NATRON_TEST_RETIRED_OFX_IDS_ENV_VAR)).split(QLatin1Char(','), Qt::SkipEmptyParts);
}

// Must run before AppManager::load(), which is when the OFX plug-ins are registered.
inline void
applyTestRetiredOfxIds()
{
    std::list<std::string> ids;

    for (const QString& id : testRetiredOfxIds()) {
        ids.push_back(id.trimmed().toStdString());
    }
    if (!ids.empty()) {
        NATRON_NAMESPACE::OfxHost::addRetiredPluginIdsForTesting(ids);
    }
}

#endif // TESTRETIREDOFXIDS_H
