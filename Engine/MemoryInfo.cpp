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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/MemoryInfo.h"

/*
 * Author:  David Robert Nadeau
 * Site:    http://NadeauSoftware.com/
 * License: Creative Commons Attribution 3.0 Unported License
 *          http://creativecommons.org/licenses/by/3.0/deed.en_US
 */
#include <algorithm> // min, max
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream> // stringstream
#include <stdexcept>
#include <vector>

#include <sys/sysinfo.h>
#include <unistd.h>

#include <QString>
#include <QLocale>
#include <QCoreApplication>
#include <QDebug>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

U64
getSystemTotalRAM()
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);

    return pages * page_size;
}

U64
getSystemTotalRAM_conditionnally()
{
    if ( isApplication32Bits() ) {
        return std::min( (U64)0x100000000ULL, getSystemTotalRAM() );
    } else {
        return getSystemTotalRAM();
    }
}

// prints RAM value as KB, MB or GB
QString
printAsRAM(U64 bytes)
{
    // According to the Si standard KB is 1000 bytes, KiB is 1024
    // but on windows sizes are calculated by dividing by 1024 so we do what they do.
    const U64 kb = 1024;
    const U64 mb = 1024 * kb;
    const U64 gb = 1024 * mb;
    const U64 tb = 1024 * gb;

    if (bytes >= tb) {
        return QCoreApplication::translate("MemoryInfo", "%1 TiB").arg( QLocale().toString(qreal(bytes) / tb, 'f', 3) );
    }
    if (bytes >= gb) {
        return QCoreApplication::translate("MemoryInfo", "%1 GiB").arg( QLocale().toString(qreal(bytes) / gb, 'f', 2) );
    }
    if (bytes >= mb) {
        return QCoreApplication::translate("MemoryInfo", "%1 MiB").arg( QLocale().toString(qreal(bytes) / mb, 'f', 1) );
    }
    if (bytes >= kb) {
        return QCoreApplication::translate("MemoryInfo", "%1 KiB").arg( QLocale().toString( (uint)(bytes / kb) ) );
    }

    return QCoreApplication::translate("MemoryInfo", "%1 byte(s)").arg( QLocale().toString( (uint)bytes ) );
}

bool
parseMemAvailableKB(const std::string& meminfoContents,
                    unsigned long long* outAvailableKB)
{
    static const std::string key("MemAvailable:");

    std::istringstream lines(meminfoContents);
    std::string line;

    while (std::getline(lines, line)) {
        if (line.compare(0, key.size(), key) != 0) {
            continue;
        }

        std::istringstream field(line.substr(key.size()));
        long long value = 0;
        std::string unit;
        std::string trailing;

        if (!(field >> value) || value < 0) {
            return false;
        }
        if (!(field >> unit) || unit != "kB") {
            return false;
        }
        if (field >> trailing) {
            return false;
        }

        const unsigned long long valueKB = (unsigned long long)value;
        if (valueKB > (std::numeric_limits<unsigned long long>::max)() / 1024ULL) {
            return false;
        }

        *outAvailableKB = valueKB;

        return true;
    }

    return false;
}

std::size_t
getAmountAvailablePhysicalRAM()
{
    std::ifstream meminfoFile("/proc/meminfo");

    if (meminfoFile) {
        std::stringstream contents;
        contents << meminfoFile.rdbuf();

        unsigned long long availableKB = 0;
        if (parseMemAvailableKB(contents.str(), &availableKB)) {
            return (std::size_t)(availableKB * 1024ULL);
        }
    }

    // MemAvailable has been present in /proc/meminfo since Linux 3.14; this is only
    // reached if that file couldn't be read or predates the field.
    struct sysinfo memInfo;
    sysinfo(&memInfo);
    unsigned long long totalFreeRAM = memInfo.freeram;
    totalFreeRAM *= memInfo.mem_unit;

    return (std::size_t)totalFreeRAM;
}

NATRON_NAMESPACE_EXIT
