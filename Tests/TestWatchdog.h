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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

// Aborts the process with every thread's stack on stderr when a test runs past its limit, so a
// stall under CI load fails fast and says where, instead of being killed by ctest with nothing.
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
        _limit = limitSeconds;
        _phase.store("start");
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

    // name must outlive the watchdog; use a string literal.
    void setPhase(const char* name)
    {
        _phase.store(name);
    }

private:
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
        const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string name = info ? std::string(info->test_case_name()) + "." + info->name() : std::string("(no test)");
        std::fprintf(stderr, "watchdog: %s stalled for %d s in phase '%s'; stack dump follows\n", name.c_str(), _limit, _phase.load());
        std::fflush(stderr);

        const std::string pid = std::to_string(getpid());
        if (!run({ "eu-stack", "-p", pid })) {
            std::fprintf(stderr, "watchdog: eu-stack unavailable or failed, trying gdb\n");
            if (!run({ "gdb", "-p", pid, "-batch", "-ex", "thread apply all bt" })) {
                std::fprintf(stderr, "watchdog: no stack dump tool worked\n");
            }
        }
        std::fflush(stderr);
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
    std::atomic<const char*> _phase { "start" };
};

#endif // NATRON_TESTS_TESTWATCHDOG_H
