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

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "Engine/ProcessLifetime.h"

NATRON_NAMESPACE_USING

namespace {

// The test binary's app manager owns the dispatcher and its signals, so each case runs in a
// forked child with a dispatcher of its own. The child reports through its exit status.
int
runInChild(const std::function<int()>& body)
{
    pid_t pid = ::fork();
    if (pid == 0) {
        ShutdownSignalDispatcher::resetInForkedChildForTesting();
        int code = 100;
        try {
            code = body();
        } catch (...) {
            code = 101;
        }
        ::_exit(code);
    }
    int status = 0;
    if ((pid < 0) || (::waitpid(pid, &status, 0) != pid)) {
        return -1;
    }

    return status;
}

bool
waitFor(const std::atomic<bool>& flag)
{
    for (int i = 0; i < 400 && !flag.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return flag.load();
}

} // anon namespace

TEST(ProcessLifetime, SignalReachesCallbackOffTheHandler)
{
    int status = runInChild([]() -> int {
        std::atomic<bool> called(false);
        std::atomic<int> received(0);
        std::thread::id callbackThread;
        if (!ShutdownSignalDispatcher::install({ SIGUSR1 }, [&](int signalId) {
                callbackThread = std::this_thread::get_id();
                received = signalId;
                called = true;
            })) {
            return 1;
        }
        ::raise(SIGUSR1);
        if (!waitFor(called)) {
            return 2;
        }
        if (received.load() != SIGUSR1) {
            return 3;
        }
        if (callbackThread == std::this_thread::get_id()) {
            return 4;
        }
        ShutdownSignalDispatcher::uninstall();
        if (ShutdownSignalDispatcher::isInstalled()) {
            return 5;
        }

        return 0;
    });

    ASSERT_TRUE(WIFEXITED(status)) << "status " << status;
    EXPECT_EQ(0, WEXITSTATUS(status));
}

TEST(ProcessLifetime, SecondSignalTakesTheDefaultAction)
{
    int status = runInChild([]() -> int {
        std::atomic<bool> called(false);
        // A stalled shutdown: the callback never ends the process itself.
        if (!ShutdownSignalDispatcher::install({ SIGUSR1 }, [&](int) { called = true; })) {
            return 1;
        }
        ::raise(SIGUSR1);
        if (!waitFor(called)) {
            return 2;
        }
        ::raise(SIGUSR1);
        std::this_thread::sleep_for(std::chrono::seconds(2));

        return 3;
    });

    ASSERT_TRUE(WIFSIGNALED(status)) << "the second signal must end the process, status " << status;
    EXPECT_EQ(SIGUSR1, WTERMSIG(status));
}

TEST(ProcessLifetime, UninstallRestoresHandlersAndAllowsReinstall)
{
    int status = runInChild([]() -> int {
        struct sigaction before;
        ::sigaction(SIGUSR2, nullptr, &before);
        std::atomic<int> calls(0);
        auto callback = [&](int) { ++calls; };
        if (!ShutdownSignalDispatcher::install({ SIGUSR2 }, callback)) {
            return 1;
        }
        if (ShutdownSignalDispatcher::install({ SIGUSR2 }, callback)) {
            return 2;
        }
        ShutdownSignalDispatcher::uninstall();
        struct sigaction after;
        ::sigaction(SIGUSR2, nullptr, &after);
        if (after.sa_handler != before.sa_handler) {
            return 3;
        }
        if (!ShutdownSignalDispatcher::install({ SIGUSR2 }, callback)) {
            return 4;
        }
        ::raise(SIGUSR2);
        for (int i = 0; i < 400 && calls.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ShutdownSignalDispatcher::uninstall();
        if (calls.load() != 1) {
            return 5;
        }

        return 0;
    });

    ASSERT_TRUE(WIFEXITED(status)) << "status " << status;
    EXPECT_EQ(0, WEXITSTATUS(status));
}

TEST(ProcessLifetime, InheritedDescriptorsAreClosedOnExec)
{
    int status = runInChild([]() -> int {
        int fds[2];
        if (::pipe(fds) != 0) {
            return 1;
        }
        const int stdinFlags = ::fcntl(0, F_GETFD);
        if ((::fcntl(fds[0], F_GETFD) & FD_CLOEXEC) || (::fcntl(fds[1], F_GETFD) & FD_CLOEXEC)) {
            return 2;
        }
        if (markInheritedDescriptorsCloseOnExec() < 2) {
            return 3;
        }
        if (!(::fcntl(fds[0], F_GETFD) & FD_CLOEXEC) || !(::fcntl(fds[1], F_GETFD) & FD_CLOEXEC)) {
            return 4;
        }
        if (::fcntl(0, F_GETFD) != stdinFlags) {
            return 5;
        }
        // An exec'd program does not see the descriptor.
        char command[64];
        std::snprintf(command, sizeof(command), "test ! -e /proc/self/fd/%d", fds[0]);
        int shellStatus = std::system(command);
        if (!WIFEXITED(shellStatus) || (WEXITSTATUS(shellStatus) != 0)) {
            return 6;
        }

        return 0;
    });

    ASSERT_TRUE(WIFEXITED(status)) << "status " << status;
    EXPECT_EQ(0, WEXITSTATUS(status));
}
