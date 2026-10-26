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

#include "ProcessLifetime.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

NATRON_NAMESPACE_ENTER

namespace {

// Signal handlers only see these globals, and they must not take a lock.
int g_pipe[2] = { -1, -1 };
std::atomic<int> g_signalsReceived(0);
static_assert(std::atomic<int>::is_always_lock_free, "the signal handler needs a lock-free counter");

struct DispatcherState {
    std::mutex mutex;
    bool installed = false;
    std::unique_ptr<std::thread> reader;
    std::vector<int> signalIds;
    std::vector<struct sigaction> previous;
};

DispatcherState&
state()
{
    static DispatcherState s;

    return s;
}

// The byte that tells the reader thread to stop; signal numbers are never 0.
const unsigned char kStopByte = 0;

void
onShutdownSignal(int signalId)
{
    const int savedErrno = errno;

    if (g_signalsReceived.fetch_add(1) > 0) {
        // The signal stays blocked until this handler returns, so the re-raised one is
        // delivered with the default action right after.
        ::signal(signalId, SIG_DFL);
        ::raise(signalId);
    } else {
        const unsigned char byte = (unsigned char)signalId;
        ssize_t written = ::write(g_pipe[1], &byte, 1);
        (void)written;
    }
    errno = savedErrno;
}

void
readerLoop(int readFd,
           std::function<void(int)> onSignal)
{
    for (;;) {
        unsigned char byte = 0;
        ssize_t n = ::read(readFd, &byte, 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0 || byte == kStopByte) {
            return;
        }
        if (onSignal) {
            onSignal((int)byte);
        }
    }
}

void
closePipe()
{
    for (int i = 0; i < 2; ++i) {
        if (g_pipe[i] != -1) {
            ::close(g_pipe[i]);
            g_pipe[i] = -1;
        }
    }
}

} // anon namespace

bool
ShutdownSignalDispatcher::install(const std::vector<int>& signalIds,
                                  const std::function<void(int)>& onSignal)
{
    DispatcherState& s = state();
    std::lock_guard<std::mutex> k(s.mutex);

    if (s.installed) {
        return false;
    }
    // Non-blocking so a handler never waits on a full pipe; one byte is all that matters.
    if (::pipe2(g_pipe, O_CLOEXEC) != 0) {
        return false;
    }
    ::fcntl(g_pipe[1], F_SETFL, ::fcntl(g_pipe[1], F_GETFL) | O_NONBLOCK);
    g_signalsReceived.store(0);

    s.reader.reset(new std::thread(readerLoop, g_pipe[0], onSignal));

    s.signalIds.clear();
    s.previous.clear();
    for (int signalId : signalIds) {
        struct sigaction sa;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = onShutdownSignal;
        struct sigaction old;
        if (::sigaction(signalId, &sa, &old) == 0) {
            s.signalIds.push_back(signalId);
            s.previous.push_back(old);
        }
    }
    s.installed = true;

    return s.signalIds.size() == signalIds.size();
}

void
ShutdownSignalDispatcher::uninstall()
{
    DispatcherState& s = state();
    std::lock_guard<std::mutex> k(s.mutex);

    if (!s.installed) {
        return;
    }
    for (std::size_t i = 0; i < s.signalIds.size(); ++i) {
        ::sigaction(s.signalIds[i], &s.previous[i], nullptr);
    }
    s.signalIds.clear();
    s.previous.clear();

    ssize_t written = ::write(g_pipe[1], &kStopByte, 1);
    (void)written;
    if (s.reader && s.reader->joinable()) {
        if (s.reader->get_id() == std::this_thread::get_id()) {
            s.reader->detach();
        } else {
            s.reader->join();
        }
    }
    s.reader.reset();
    closePipe();
    s.installed = false;
}

void
ShutdownSignalDispatcher::resetInForkedChildForTesting()
{
    DispatcherState& s = state();

    // The reader thread exists only in the parent. Its object is leaked: it can be neither
    // joined nor destroyed while joinable.
    for (std::size_t i = 0; i < s.signalIds.size(); ++i) {
        ::sigaction(s.signalIds[i], &s.previous[i], nullptr);
    }
    s.signalIds.clear();
    s.previous.clear();
    (void)s.reader.release();
    closePipe();
    s.installed = false;
}

bool
ShutdownSignalDispatcher::isInstalled()
{
    DispatcherState& s = state();
    std::lock_guard<std::mutex> k(s.mutex);

    return s.installed;
}

int
markInheritedDescriptorsCloseOnExec(int firstFd)
{
    DIR* dir = ::opendir("/proc/self/fd");

    if (!dir) {
        return 0;
    }
    const int ownFd = ::dirfd(dir);
    int changed = 0;
    while (struct dirent* entry = ::readdir(dir)) {
        char* end = nullptr;
        long fd = std::strtol(entry->d_name, &end, 10);
        if ((end == entry->d_name) || (*end != '\0') || (fd < firstFd) || (fd == ownFd)) {
            continue;
        }
        int flags = ::fcntl((int)fd, F_GETFD);
        if ((flags != -1) && !(flags & FD_CLOEXEC)) {
            if (::fcntl((int)fd, F_SETFD, flags | FD_CLOEXEC) == 0) {
                ++changed;
            }
        }
    }
    ::closedir(dir);

    return changed;
}

NATRON_NAMESPACE_EXIT
