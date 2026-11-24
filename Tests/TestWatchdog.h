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

#ifndef NATRON_TESTS_TESTWATCHDOG_H
#define NATRON_TESTS_TESTWATCHDOG_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

// Aborts the process with every thread's stack on stderr when a test runs past its limit, so a
// stall under CI load fails fast and says where, instead of being killed by ctest with nothing.
// start() also installs a handler that prints the phase and every thread's stack on a fatal signal,
// and a global environment arms a second limit once the last test has finished, so a hang in
// process teardown dumps stacks too (NATRON_TEST_EXIT_WATCHDOG_S, default 60, 0 disables).
class TestWatchdog {
public:
    static constexpr int kDefaultLimitSeconds = 150;

    TestWatchdog() = default;
    TestWatchdog(const TestWatchdog&) = delete;
    TestWatchdog& operator=(const TestWatchdog&) = delete;

    ~TestWatchdog()
    {
        stop();
    }

    // NATRON_TEST_WATCHDOG_S overrides the limit, and 0 disables the watchdog.
    void start()
    {
        int limit = kDefaultLimitSeconds;
        const char* env = std::getenv("NATRON_TEST_WATCHDOG_S");
        if (env && *env) {
            limit = std::atoi(env);
        }
        start(limit);
    }

    void start(int limitSeconds)
    {
        stop();
        if (limitSeconds <= 0) {
            return;
        }
        // eu-stack and gdb are not our ancestors, so Yama's default ptrace scope would refuse
        // them; this has to be set before the stall, when the process is still healthy.
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
        installCrashHandler();
        _limit = limitSeconds;
        phase().store("start");
        _stopped = false;
        _thread = std::thread([this]() { watch(); });
    }

    void stop()
    {
        if (!_thread.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stopped = true;
        }
        _cv.notify_all();
        _thread.join();
    }

    // name must outlive the watchdog; use a string literal. Static so that helpers without a
    // watchdog at hand can report the phase they are in.
    static void setPhase(const char* name)
    {
        phase().store(name);
    }

    static void installCrashHandler()
    {
        static std::atomic<bool> installed { false };
        if (installed.exchange(true)) {
            return;
        }
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        action.sa_handler = &onFatalSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESETHAND | SA_NODEFER;
        for (const int sig : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT }) {
            sigaction(sig, &action, nullptr);
        }
    }

    // Arms a limit for the process to finish exiting, for a hang in static or atexit teardown.
    static void armExitWatchdog(int limitSeconds)
    {
        if (limitSeconds <= 0) {
            return;
        }
        phase().store("process exit");
        std::thread([limitSeconds]() {
            std::this_thread::sleep_for(std::chrono::seconds(limitSeconds));
            expire("process exit", limitSeconds);
        }).detach();
    }

private:
    static std::atomic<const char*>& phase()
    {
        static std::atomic<const char*> p { "start" };

        return p;
    }

    static std::atomic<bool>& dumped()
    {
        static std::atomic<bool> d { false };

        return d;
    }

    static std::string currentTestName()
    {
        const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();

        return info ? std::string(info->test_case_name()) + "." + info->name() : std::string("(no test)");
    }

    static void dumpStacks()
    {
        const std::string pid = std::to_string(getpid());
        if (!run({ "eu-stack", "-p", pid })) {
            std::fprintf(stderr, "watchdog: eu-stack unavailable or failed, trying gdb\n");
            if (!run({ "gdb", "-p", pid, "-batch", "-ex", "thread apply all bt" })) {
                std::fprintf(stderr, "watchdog: no stack dump tool worked\n");
            }
        }
        std::fflush(stderr);
    }

    static void onFatalSignal(int sig)
    {
        if (dumped().exchange(true)) {
            return;
        }
        std::fprintf(stderr, "watchdog: fatal signal %d in %s, phase '%s'; crashing thread then every thread follows\n", sig, currentTestName().c_str(), phase().load());
        void* frames[128];
        const int count = backtrace(frames, 128);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
        std::fflush(stderr);
        dumpStacks();
    }

    void watch()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        if (_cv.wait_for(lock, std::chrono::seconds(_limit), [this]() { return _stopped; })) {
            return;
        }
        expire();
    }

    void expire()
    {
        expire(currentTestName().c_str(), _limit);
    }

    static void expire(const char* what, int limit)
    {
        std::fprintf(stderr, "watchdog: %s stalled for %d s in phase '%s'; stack dump follows\n", what, limit, phase().load());
        std::fflush(stderr);
        dumped().store(true);
        dumpStacks();
        std::abort();
    }

    // True if the tool ran to completion and exited 0 within 30 s; its output goes to stderr.
    static bool run(const std::vector<std::string>& args)
    {
        std::vector<char*> argv;
        for (const std::string& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        const pid_t child = fork();
        if (child < 0) {
            return false;
        }
        if (child == 0) {
            dup2(STDERR_FILENO, STDOUT_FILENO);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        for (int i = 0; i < 600; ++i) {
            int status = 0;
            const pid_t done = waitpid(child, &status, WNOHANG);
            if (done == child) {
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            if (done < 0) {
                return false;
            }
            usleep(50 * 1000);
        }
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);

        return false;
    }

    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _stopped = false;
    int _limit = kDefaultLimitSeconds;
};

namespace TestWatchdogDetail {
class ExitEnvironment : public ::testing::Environment {
public:
    virtual void TearDown() override
    {
        int limit = 60;
        const char* env = std::getenv("NATRON_TEST_EXIT_WATCHDOG_S");
        if (env && *env) {
            limit = std::atoi(env);
        }
        TestWatchdog::armExitWatchdog(limit);
    }
};

inline ::testing::Environment* const exitEnvironment = ::testing::AddGlobalTestEnvironment(new ExitEnvironment);
} // namespace TestWatchdogDetail

#endif // NATRON_TESTS_TESTWATCHDOG_H
