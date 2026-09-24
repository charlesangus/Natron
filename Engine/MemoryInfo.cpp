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
#include <sstream> // stringstream
#include <stdexcept>
#include <vector>

// This fork targets Linux only (see README.md); the _WIN32, BSD, Apple, AIX and
// Solaris branches this file used to carry alongside these are unreachable and
// have been removed rather than kept up to date for platforms nothing builds.
#include <stdio.h>
#include <sys/resource.h>
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


#if 0 // not used for now
/**
 * Returns the peak (maximum so far) resident set size (physical
 * memory use) measured in bytes, or zero if the value cannot be
 * determined on this OS.
 */
std::size_t
getPeakRSS( )
{
#if defined(_WIN32)
    /* Windows -------------------------------------------------- */
    PROCESS_MEMORY_COUNTERS info;
    GetProcessMemoryInfo( GetCurrentProcess( ), &info, sizeof(info) );

    return (size_t)info.PeakWorkingSetSize;

#elif (defined(_AIX) || defined(__TOS__AIX__ ) ) || (defined(__sun__) || defined(__sun) || defined(sun) && (defined(__SVR4) || defined(__svr4__ )  ) )
    /* AIX and Solaris ------------------------------------------ */
    struct psinfo psinfo;
    int fd = -1;
    if ( ( fd = open( "/proc/self/psinfo", O_RDONLY ) ) == -1 ) {
        return (size_t)0L;      /* Can't open? */
    }
    if ( read( fd, &psinfo, sizeof(psinfo) ) != sizeof(psinfo) ) {
        close( fd );

        return (size_t)0L;      /* Can't read? */
    }
    close( fd );

    return (size_t)(psinfo.pr_rssize * 1024L);

#elif defined(__unix__) || defined(__unix) || defined(unix) || (defined(__APPLE__) && defined(__MACH__ ) )
    /* BSD, Linux, and OSX -------------------------------------- */
    struct rusage rusage;
    getrusage( RUSAGE_SELF, &rusage );
#if defined(__APPLE__) && defined(__MACH__)

    return (size_t)rusage.ru_maxrss;
#else

    return (size_t)(rusage.ru_maxrss * 1024L);
#endif

#else

    /* Unknown OS ----------------------------------------------- */
    return (size_t)0L;          /* Unsupported. */
#endif
}
#endif // 0

#if 0 // not used for now
/**
 * Returns the current resident set size (physical memory use) measured
 * in bytes, or zero if the value cannot be determined on this OS.
 */
std::size_t
getCurrentRSS( )
{
#if defined(_WIN32)
    /* Windows -------------------------------------------------- */
    PROCESS_MEMORY_COUNTERS info;
    GetProcessMemoryInfo( GetCurrentProcess( ), &info, sizeof(info) );

    return (size_t)info.WorkingSetSize;

#elif defined(__APPLE__) && defined(__MACH__)
    /* OSX ------------------------------------------------------ */
#  ifndef MACH_TASK_BASIC_INFO
    struct task_basic_info info;
    mach_msg_type_number_t infoCount = TASK_BASIC_INFO_COUNT;
    if (task_info( mach_task_self( ), TASK_BASIC_INFO,
                   (task_info_t)&info, &infoCount ) != KERN_SUCCESS) {
        return (size_t)0L;      /* Can't access? */
    }
#  else
    struct mach_task_basic_info info;
    mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info( mach_task_self( ), MACH_TASK_BASIC_INFO,
                   (task_info_t)&info, &infoCount ) != KERN_SUCCESS) {
        return (size_t)0L;      /* Can't access? */
    }
#  endif

    return (size_t)info.resident_size;

#elif defined(__linux__) || defined(__linux) || defined(linux) || defined(__gnu_linux__)
    /* Linux ---------------------------------------------------- */
    long rss = 0L;
    FILE* fp = NULL;
    if ( ( fp = fopen( "/proc/self/statm", "r" ) ) == NULL ) {
        return (size_t)0L;      /* Can't open? */
    }
    if (fscanf( fp, "%*s%ld", &rss ) != 1) {
        fclose( fp );

        return (size_t)0L;      /* Can't read? */
    }
    fclose( fp );

    return (size_t)rss * (size_t)sysconf( _SC_PAGESIZE);

#else

    /* AIX, BSD, Solaris, and Unknown OS ------------------------ */
    return (size_t)0L;          /* Unsupported. */
#endif
} // getCurrentRSS
#endif // 0

bool
parseMemAvailableKB(const std::string& meminfoContents,
                    unsigned long long* outAvailableKB)
{
    const std::string key("MemAvailable:");
    std::string::size_type pos = meminfoContents.find(key);

    if (pos == std::string::npos) {
        return false;
    }

    std::istringstream iss(meminfoContents.substr(pos + key.size()));
    unsigned long long value = 0;

    if (!(iss >> value)) {
        return false;
    }

    *outAvailableKB = value;

    return true;
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
