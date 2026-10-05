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

#ifndef NATRON_ENGINE_OPENMPTHREADS_H
#define NATRON_ENGINE_OPENMPTHREADS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <chrono>

#ifdef __linux__
#include <dlfcn.h>
#endif

NATRON_NAMESPACE_ENTER

namespace OpenMPThreadsDetail {
#ifdef __linux__
typedef void (*SetNumThreadsFn)(int);
typedef int (*GetMaxThreadsFn)();

// libgomp is looked up among the loaded objects only. Graphs without an OpenMP plug-in never load it, so a failed
// lookup is retried at most once a second, by one thread, rather than on every call.
inline bool
lookup(SetNumThreadsFn* setNumThreads,
       GetMaxThreadsFn* getMaxThreads)
{
    static std::atomic<SetNumThreadsFn> cachedSet(nullptr);
    static std::atomic<GetMaxThreadsFn> cachedGet(nullptr);
    static std::atomic<long long> nextRetryNs(0);
    SetNumThreadsFn set = cachedSet.load(std::memory_order_acquire);

    if (!set) {
        const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        long long due = nextRetryNs.load(std::memory_order_relaxed);
        if (now < due || !nextRetryNs.compare_exchange_strong(due, now + 1000000000LL, std::memory_order_relaxed)) {
            return false;
        }
        void* lib = dlopen("libgomp.so.1", RTLD_LAZY | RTLD_NOLOAD);
        if (!lib) {
            return false;
        }
        set = reinterpret_cast<SetNumThreadsFn>(dlsym(lib, "omp_set_num_threads"));
        GetMaxThreadsFn get = reinterpret_cast<GetMaxThreadsFn>(dlsym(lib, "omp_get_max_threads"));
        if (!set || !get) {
            dlclose(lib);

            return false;
        }
        cachedGet.store(get, std::memory_order_relaxed);
        cachedSet.store(set, std::memory_order_release);
    }
    *setNumThreads = set;
    *getMaxThreads = cachedGet.load(std::memory_order_relaxed);

    return true;
}
#endif
} // namespace OpenMPThreadsDetail

/**
 * @brief Sets the nthreads ICV of the calling thread, from which OpenMP plug-ins size the teams they start on it, and
 * returns its previous value; returns 0 and does nothing when libgomp is not loaded.
 **/
inline int
setOpenMPThreadsOfCurrentThread(int threads)
{
#ifdef __linux__
    OpenMPThreadsDetail::SetNumThreadsFn set = nullptr;
    OpenMPThreadsDetail::GetMaxThreadsFn get = nullptr;
    if (!OpenMPThreadsDetail::lookup(&set, &get)) {
        return 0;
    }
    const int previous = get();
    set(threads);

    return previous;
#else
    (void)threads;

    return 0;
#endif
}

/**
 * @brief Sets the nthreads ICV of the calling thread for the lifetime of the scope, since a pool thread keeps it from
 * one task to the next.
 **/
class OpenMPThreadsScope {
public:
    explicit OpenMPThreadsScope(int threads)
        : _previous(setOpenMPThreadsOfCurrentThread(threads))
    {
    }

    ~OpenMPThreadsScope()
    {
        if (_previous > 0) {
            setOpenMPThreadsOfCurrentThread(_previous);
        }
    }

    OpenMPThreadsScope(const OpenMPThreadsScope&) = delete;
    OpenMPThreadsScope& operator=(const OpenMPThreadsScope&) = delete;

private:
    int _previous;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_OPENMPTHREADS_H
