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
//
// Every test process also gets, from a global environment set up once the main has loaded:
// - a handler that prints the phase and every thread's stack on a fatal signal, then dies of that
//   signal, so the exit status ctest sees is unchanged;
// - one stack dump, the process left running, when it is still going shortly before ctest's
//   timeout (NATRON_TEST_CTEST_TIMEOUT_S, default 300, 0 disables), since ctest kills a stuck
//   suite without a word;
// - a limit for the process to finish exiting once the last test is done, so a hang in process
//   teardown dumps stacks too (NATRON_TEST_EXIT_WATCHDOG_S, default 60, 0 disables).
class TestWatchdog {
public:
    static constexpr int kDefaultLimitSeconds = 150;
    // Matches the TIMEOUT every suite is registered with; SuiteProperties.cmake passes the longer
    // ones down.
    static constexpr int kDefaultCtestTimeoutSeconds = 300;
    // Room for eu-stack, then gdb, to finish before ctest's kill: each gets up to 30 s in run(),
    // and eu-stack takes a few seconds on a large process.
    static constexpr int kStallDumpLeadSeconds = 45;
    static constexpr int kDefaultExitLimitSeconds = 60;

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
        start(envSeconds("NATRON_TEST_WATCHDOG_S", kDefaultLimitSeconds));
    }

    void start(int limitSeconds)
    {
        stop();
        if (limitSeconds <= 0) {
            return;
        }
        allowStackDumpers();
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

    // eu-stack and gdb are not our ancestors, so Yama's default ptrace scope would refuse them;
    // this has to be set before the stall, when the process is still healthy.
    static void allowStackDumpers()
    {
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    }

    // Has to run after AppManager::load(), which installs a SIGSEGV and SIGFPE handler of its own
    // that prints the crashing thread only.
    static void installCrashHandler()
    {
        static std::atomic<bool> installed { false };
        if (installed.exchange(true)) {
            return;
        }
        // backtrace() loads libgcc on its first call, which allocates; the handler must not.
        void* frame = nullptr;
        backtrace(&frame, 1);
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        action.sa_handler = &onFatalSignal;
        sigemptyset(&action.sa_mask);
        // SA_ONSTACK uses the alternate stack AppManager gives the main thread, for a stack overflow.
        action.sa_flags = SA_RESETHAND | SA_NODEFER | SA_ONSTACK;
        for (const int sig : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT }) {
            sigaction(sig, &action, nullptr);
        }
    }

    // Seconds after the process started at which the one stall dump is taken: ctest's timeout for
    // this suite less kStallDumpLeadSeconds, or half of a short timeout. 0 disables the dump.
    static int stallDumpSeconds()
    {
        const int timeout = envSeconds("NATRON_TEST_CTEST_TIMEOUT_S", kDefaultCtestTimeoutSeconds);
        if (timeout <= 0) {
            return 0;
        }
        const int lead = timeout / 2 < kStallDumpLeadSeconds ? timeout / 2 : kStallDumpLeadSeconds;

        return timeout - lead > 0 ? timeout - lead : 1;
    }

    // Takes one dump of every stack, without stopping the process, once it has run secondsAfterStart.
    // A later call moves the deadline until the dump has been taken; after that it does nothing.
    static void armStallDump(int secondsAfterStart)
    {
        ProcessWatch& w = processWatch();
        std::lock_guard<std::mutex> lock(w.mutex);
        if (secondsAfterStart <= 0 || w.stallDumped) {
            w.stallArmed = false;
        } else {
            w.stallArmed = true;
            w.stallSeconds = secondsAfterStart;
            w.stallAt = processStart() + std::chrono::seconds(secondsAfterStart);
            startMonitor(w);
        }
        w.cv.notify_all();
    }

    static bool stallDumpTaken()
    {
        ProcessWatch& w = processWatch();
        std::lock_guard<std::mutex> lock(w.mutex);

        return w.stallDumped;
    }

    // Arms a limit for the process to finish exiting, for a hang in static or atexit teardown.
    static void armExitWatchdog(int limitSeconds)
    {
        ProcessWatch& w = processWatch();
        std::lock_guard<std::mutex> lock(w.mutex);
        if (limitSeconds <= 0) {
            w.exitArmed = false;
        } else {
            phase().store("process exit");
            w.exitArmed = true;
            w.exitSeconds = limitSeconds;
            w.exitAt = std::chrono::steady_clock::now() + std::chrono::seconds(limitSeconds);
            startMonitor(w);
        }
        w.cv.notify_all();
    }

    // gtest sets the environments up again for each --gtest_repeat iteration, after the previous
    // one's teardown armed the exit limit.
    static void disarmExitWatchdog()
    {
        ProcessWatch& w = processWatch();
        std::lock_guard<std::mutex> lock(w.mutex);
        w.exitArmed = false;
        w.cv.notify_all();
    }

    static int envSeconds(const char* name,
                          int fallback)
    {
        const char* env = std::getenv(name);

        return env && *env ? std::atoi(env) : fallback;
    }

    static std::chrono::steady_clock::time_point processStart()
    {
        return processStartTime;
    }

private:
    struct ProcessWatch {
        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;
        bool stallArmed = false;
        bool stallDumped = false;
        int stallSeconds = 0;
        std::chrono::steady_clock::time_point stallAt;
        bool exitArmed = false;
        int exitSeconds = 0;
        std::chrono::steady_clock::time_point exitAt;
    };

    // Never destroyed: the monitor thread is detached and may still be waiting during static teardown.
    static ProcessWatch& processWatch()
    {
        static ProcessWatch* w = new ProcessWatch;

        return *w;
    }

    static void startMonitor(ProcessWatch& w)
    {
        if (w.started) {
            return;
        }
        w.started = true;
        std::thread([&w]() { monitor(w); }).detach();
    }

    static void monitor(ProcessWatch& w)
    {
        std::unique_lock<std::mutex> lock(w.mutex);
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (w.stallArmed && now >= w.stallAt) {
                w.stallArmed = false;
                const int seconds = w.stallSeconds;
                lock.unlock();
                reportStall(seconds);
                lock.lock();
                w.stallDumped = true;
                w.cv.notify_all();
                continue;
            }
            if (w.exitArmed && now >= w.exitAt) {
                const int seconds = w.exitSeconds;
                lock.unlock();
                expire("process exit", seconds);
            }
            if (w.stallArmed && w.exitArmed) {
                w.cv.wait_until(lock, w.stallAt < w.exitAt ? w.stallAt : w.exitAt);
            } else if (w.stallArmed) {
                w.cv.wait_until(lock, w.stallAt);
            } else if (w.exitArmed) {
                w.cv.wait_until(lock, w.exitAt);
            } else {
                w.cv.wait(lock);
            }
        }
    }

    static void reportStall(int seconds)
    {
        char line[512];
        const int n = std::snprintf(line, sizeof(line), "watchdog: %s still running %d s after the process started, phase '%s'; stack dump follows, then the process goes on\n", currentTestName().c_str(), seconds, phase().load());
        writeStderr(line, n);
        dumpStacks();
        writeStderr("watchdog: stall dump done\n");
    }

    static inline const std::chrono::steady_clock::time_point processStartTime = std::chrono::steady_clock::now();

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

    static volatile sig_atomic_t& fatalSignal()
    {
        static volatile sig_atomic_t s = 0;

        return s;
    }

    static std::string currentTestName()
    {
        const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();

        return info ? std::string(info->test_case_name()) + "." + info->name() : std::string("(no test)");
    }

    static void writeStderr(const char* text,
                            int length = -1)
    {
        if (length < 0) {
            length = static_cast<int>(std::strlen(text));
        }
        while (length > 0) {
            const ssize_t written = write(STDERR_FILENO, text, static_cast<size_t>(length));
            if (written <= 0) {
                return;
            }
            text += written;
            length -= static_cast<int>(written);
        }
    }

    // Allocates nothing, as it also runs in the fatal-signal handler.
    static void dumpStacks()
    {
        std::fflush(stderr);
        char pid[24];
        std::snprintf(pid, sizeof(pid), "%d", static_cast<int>(getpid()));
        const char* const euStack[] = { "eu-stack", "-p", pid, nullptr };
        if (!run(euStack)) {
            writeStderr("watchdog: eu-stack unavailable or failed, trying gdb\n");
            const char* const gdb[] = { "gdb", "-p", pid, "-batch", "-ex", "thread apply all bt", nullptr };
            if (!run(gdb)) {
                writeStderr("watchdog: no stack dump tool worked\n");
            }
        }
    }

    // A crash inside malloc or stdio would deadlock the dump; the signal then ends the process anyway.
    static void onDumpTimeout(int)
    {
        const int sig = fatalSignal();
        signal(sig, SIG_DFL);
        raise(sig);
    }

    static void onFatalSignal(int sig)
    {
        if (!dumped().exchange(true)) {
            fatalSignal() = sig;
            struct sigaction guard;
            std::memset(&guard, 0, sizeof(guard));
            guard.sa_handler = &onDumpTimeout;
            sigemptyset(&guard.sa_mask);
            sigaction(SIGALRM, &guard, nullptr);
            alarm(90);

            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            char line[512];
            const int n = std::snprintf(line, sizeof(line), "watchdog: fatal signal %d in %s%s%s, phase '%s'; crashing thread then every thread follows\n", sig, info ? info->test_case_name() : "(no test)", info ? "." : "", info ? info->name() : "", phase().load());
            writeStderr(line, n);
            void* frames[128];
            const int count = backtrace(frames, 128);
            backtrace_symbols_fd(frames, count, STDERR_FILENO);
            dumpStacks();
            alarm(0);
        }
        // SA_RESETHAND restored the default action and SA_NODEFER left the signal unblocked, so the
        // process dies of it here, with the status ctest would have seen without the handler.
        raise(sig);
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

    [[noreturn]] static void expire(const char* what, int limit)
    {
        std::fprintf(stderr, "watchdog: %s stalled for %d s in phase '%s'; stack dump follows\n", what, limit, phase().load());
        std::fflush(stderr);
        dumped().store(true);
        dumpStacks();
        std::abort();
    }

    // True if the tool ran to completion and exited 0 within 30 s; its output goes to stderr.
    static bool run(const char* const* argv)
    {
        const pid_t child = fork();
        if (child < 0) {
            return false;
        }
        if (child == 0) {
            dup2(STDERR_FILENO, STDOUT_FILENO);
            execvp(argv[0], const_cast<char* const*>(argv));
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
class ProcessEnvironment : public ::testing::Environment {
public:
    virtual void SetUp() override
    {
        TestWatchdog::disarmExitWatchdog();
        TestWatchdog::allowStackDumpers();
        TestWatchdog::installCrashHandler();
        TestWatchdog::armStallDump(TestWatchdog::stallDumpSeconds());
    }

    virtual void TearDown() override
    {
        TestWatchdog::armExitWatchdog(TestWatchdog::envSeconds("NATRON_TEST_EXIT_WATCHDOG_S", TestWatchdog::kDefaultExitLimitSeconds));
    }
};

inline ::testing::Environment* const processEnvironment = ::testing::AddGlobalTestEnvironment(new ProcessEnvironment);
} // namespace TestWatchdogDetail

#endif // NATRON_TESTS_TESTWATCHDOG_H
