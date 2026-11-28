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
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
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
//   timeout (NATRON_TEST_CTEST_TIMEOUT_S, which every registered suite gets from its TIMEOUT;
//   0 disables), since ctest kills a stuck suite without a word;
// - one stack dump, the process left running, when it has not finished exiting some time after
//   the last test, for a hang in process teardown (NATRON_TEST_EXIT_WATCHDOG_S, default 60,
//   0 disables);
// - a report of any non-zero exit status, through exit() or _exit() (TestExitReport.cpp), with the
//   calling thread and every thread's stack, since ctest shows only "Failed" for a suite whose
//   tests all passed and then something ended the process with a status of its own.
// A stall, its dump before ctest's timeout or a slow exit also dumps the stacks of this process's
// execComp children, MoonRay's render processes, within the same budget; where ptrace on them is
// refused, each of their threads' state and kernel wait channel instead.
// In a gtest death-test child only the crashing thread's backtrace is printed: the dump tools take
// seconds each, and the suites' death tests crash on purpose.
class TestWatchdog {
public:
    static constexpr int kDefaultLimitSeconds = 150;
    // For a binary run outside ctest, which sets NATRON_TEST_CTEST_TIMEOUT_S for every suite.
    static constexpr int kDefaultCtestTimeoutSeconds = 300;
    // One deadline for the whole dump: eu-stack, then gdb with whatever time eu-stack left.
    static constexpr int kDumpBudgetSeconds = 60;
    // The dump budget plus room for its output to reach ctest's log before the kill.
    static constexpr int kStallDumpLeadSeconds = kDumpBudgetSeconds + 15 > 75 ? kDumpBudgetSeconds + 15 : 75;
    // Past the dump budget, for a dump that deadlocks in fork inside a crashed malloc.
    static constexpr int kCrashDumpAlarmSeconds = kDumpBudgetSeconds + 30;
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
        // The handler cannot search PATH or allocate, so the tools are looked up now.
        findTool("eu-stack", toolPaths.euStack, sizeof(toolPaths.euStack));
        findTool("gdb", toolPaths.gdb, sizeof(toolPaths.gdb));
        // backtrace() loads libgcc on its first call, which allocates; the handler must not.
        void* frame = nullptr;
        backtrace(&frame, 1);
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        action.sa_handler = &onFatalSignal;
        sigemptyset(&action.sa_mask);
        // No SA_RESETHAND: a second thread faulting with the same signal during the dump has to
        // reach the handler and wait for it, not die of the default action. SA_NODEFER lets the
        // handler re-raise its own signal; SA_ONSTACK uses the alternate stack AppManager gives
        // the main thread, for a stack overflow.
        action.sa_flags = SA_NODEFER | SA_ONSTACK;
        for (const int sig : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT }) {
            sigaction(sig, &action, nullptr);
        }
    }

    // Has to run after AppManager::load(): on_exit handlers run last registered first, and the
    // manager's own ends the process with _exit() before the ones registered ahead of it.
    static void reportNonZeroExits()
    {
        reportingPid().store(getpid());
        static std::atomic<bool> registered { false };
        if (!registered.exchange(true)) {
            on_exit(&onExit, nullptr);
        }
    }

    // Called by the test binaries' _exit() before the process ends.
    static void reportDirectExit(int status)
    {
        reportExit("_exit", status);
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

    // Arms one stack dump, the process left running, for a hang in static or atexit teardown.
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

    // Lets the fatal-signal handler find the running test without taking gtest's mutex.
    class CurrentTestListener : public ::testing::EmptyTestEventListener {
    public:
        virtual void OnTestStart(const ::testing::TestInfo& info) override
        {
            currentTest().store(&info);
        }

        virtual void OnTestEnd(const ::testing::TestInfo&) override
        {
            currentTest().store(nullptr);
        }
    };

    static void listenForCurrentTest()
    {
        static std::atomic<bool> listening { false };
        if (!listening.exchange(true)) {
            ::testing::UnitTest::GetInstance()->listeners().Append(new CurrentTestListener);
        }
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

    struct ToolPaths {
        char euStack[4096];
        char gdb[4096];
    };

    // Text built in a fixed buffer, with no stdio and no allocation, for the fatal-signal handler.
    class Line {
    public:
        void addText(const char* text)
        {
            while (text && *text && _length < kCapacity) {
                _text[_length++] = *text++;
            }
        }

        void addNumber(long value)
        {
            char digits[24];
            int count = 0;
            unsigned long magnitude = value < 0 ? 0UL - static_cast<unsigned long>(value) : static_cast<unsigned long>(value);
            do {
                digits[count++] = static_cast<char>('0' + magnitude % 10);
                magnitude /= 10;
            } while (magnitude != 0);
            if (value < 0) {
                addText("-");
            }
            while (count > 0 && _length < kCapacity) {
                _text[_length++] = digits[--count];
            }
        }

        const char* c_str()
        {
            _text[_length] = '\0';

            return _text;
        }

        int length() const
        {
            return _length;
        }

    private:
        static constexpr int kCapacity = 511;
        char _text[kCapacity + 1];
        int _length = 0;
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
                w.exitArmed = false;
                const int seconds = w.exitSeconds;
                lock.unlock();
                reportSlowExit(seconds);
                lock.lock();
                continue;
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
        const long deadline = monotonicMilliseconds() + kDumpBudgetSeconds * 1000L;
        dumpStacks(deadline);
        dumpRenderProcessStacks(deadline);
        writeStderr("watchdog: stall dump done\n");
    }

    static void reportSlowExit(int seconds)
    {
        char line[512];
        const int n = std::snprintf(line, sizeof(line), "watchdog: process exit still running %d s after the last test, phase '%s'; stack dump follows, then the process goes on\n", seconds, phase().load());
        writeStderr(line, n);
        const long deadline = monotonicMilliseconds() + kDumpBudgetSeconds * 1000L;
        dumpStacks(deadline);
        dumpRenderProcessStacks(deadline);
        writeStderr("watchdog: exit dump done\n");
    }

    static void onExit(int status,
                       void*)
    {
        reportExit("exit", status);
    }

    // Only the process that asked: a forked child that never reached exec, or a vfork child,
    // ends through the same exit() and _exit() and must stay silent.
    static void reportExit(const char* how,
                           int status)
    {
        if (status == 0 || reportingPid().load() != getpid() || exitReported().exchange(true)) {
            return;
        }
        // gtest's own status for a run with failed tests, already explained in its summary.
        if (status == 1 && ::testing::UnitTest::GetInstance()->Failed()) {
            return;
        }
        const pid_t thread = static_cast<pid_t>(syscall(SYS_gettid));
        Line line;
        line.addText("watchdog: the process is exiting with status ");
        line.addNumber(status);
        line.addText(" through ");
        line.addText(how);
        line.addText("() on thread ");
        line.addNumber(thread);
        line.addText(thread == getpid() ? " (the main thread)" : " (not the main thread)");
        line.addText(", phase '");
        line.addText(phase().load());
        line.addText("'; its stack, then every thread's, follows\n");
        writeStderr(line.c_str(), line.length());
        void* frames[64];
        backtrace_symbols_fd(frames, backtrace(frames, 64), STDERR_FILENO);
        const long deadline = monotonicMilliseconds() + kDumpBudgetSeconds * 1000L;
        dumpStacks(deadline);
        dumpRenderProcessStacks(deadline);
        writeStderr("watchdog: exit report done\n");
    }

    static std::atomic<pid_t>& reportingPid()
    {
        static std::atomic<pid_t> p { 0 };

        return p;
    }

    static std::atomic<bool>& exitReported()
    {
        static std::atomic<bool> r { false };

        return r;
    }

    static inline const std::chrono::steady_clock::time_point processStartTime = std::chrono::steady_clock::now();
    static inline ToolPaths toolPaths = {};

    static std::atomic<const char*>& phase()
    {
        static std::atomic<const char*> p { "start" };

        return p;
    }

    static std::atomic<const ::testing::TestInfo*>& currentTest()
    {
        static std::atomic<const ::testing::TestInfo*> t { nullptr };

        return t;
    }

    // The thread taking the one fatal dump, 0 while none has started.
    static std::atomic<pid_t>& dumpingThread()
    {
        static std::atomic<pid_t> t { 0 };

        return t;
    }

    static std::atomic<bool>& dumpDone()
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

    static void findTool(const char* name,
                         char* out,
                         size_t size)
    {
        out[0] = '\0';
        const char* env = std::getenv("PATH");
        const std::string dirs = env && *env ? env : "/usr/local/bin:/usr/bin:/bin";
        size_t begin = 0;
        while (begin <= dirs.size()) {
            size_t end = dirs.find(':', begin);
            if (end == std::string::npos) {
                end = dirs.size();
            }
            const std::string dir = end > begin ? dirs.substr(begin, end - begin) : std::string(".");
            const std::string candidate = dir + "/" + name;
            if (candidate.size() < size && access(candidate.c_str(), X_OK) == 0) {
                std::memcpy(out, candidate.c_str(), candidate.size() + 1);

                return;
            }
            begin = end + 1;
        }
    }

    static long monotonicMilliseconds()
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        return static_cast<long>(now.tv_sec) * 1000L + now.tv_nsec / 1000000L;
    }

    static void sleepMilliseconds(long milliseconds)
    {
        struct timespec delay;
        delay.tv_sec = milliseconds / 1000;
        delay.tv_nsec = (milliseconds % 1000) * 1000000L;
        nanosleep(&delay, nullptr);
    }

    // Also runs in the fatal-signal handler, so it formats by hand and only calls write, fork,
    // execve, waitpid, kill, nanosleep and clock_gettime. All but fork are async-signal-safe; fork
    // runs glibc's atfork handlers, which take the malloc lock, so a crash inside malloc can still
    // hang the dump until kCrashDumpAlarmSeconds ends it.
    static void dumpStacks()
    {
        dumpStacks(monotonicMilliseconds() + kDumpBudgetSeconds * 1000L);
    }

    static void dumpStacks(long deadline)
    {
        if (::testing::internal::InDeathTestChild()) {
            writeStderr("watchdog: gtest death-test child, stack dump tools skipped\n");

            return;
        }
        Line pid;
        pid.addNumber(static_cast<long>(getpid()));
        (void)dumpStacksOf(pid.c_str(), deadline);
    }

    // True if eu-stack or gdb printed pidText's stacks before deadline.
    static bool dumpStacksOf(const char* pidText,
                             long deadline)
    {
        const char* const euStack[] = { "eu-stack", "-p", pidText, nullptr };
        if (run(toolPaths.euStack, euStack, deadline)) {
            return true;
        }
        writeStderr("watchdog: eu-stack unavailable or failed, trying gdb\n");
        const char* const gdb[] = { "gdb", "-p", pidText, "-batch", "-ex", "thread apply all bt", nullptr };
        if (run(toolPaths.gdb, gdb, deadline)) {
            return true;
        }
        writeStderr("watchdog: no stack dump tool worked\n");

        return false;
    }

    static std::string readFirstLine(const std::string& path)
    {
        std::ifstream file(path);
        std::string line;
        std::getline(file, line);

        return line;
    }

    // The pids of this process's live children named execComp: the render processes of MoonRay's
    // Arras sessions. Allocates, so never called from the fatal-signal handler.
    static std::vector<std::string> renderProcesses()
    {
        std::vector<std::string> pids;
        DIR* proc = opendir("/proc");
        if (!proc) {
            return pids;
        }
        const long self = static_cast<long>(getpid());
        while (const struct dirent* entry = readdir(proc)) {
            char* end = nullptr;
            const long pid = std::strtol(entry->d_name, &end, 10);
            if (pid <= 0 || !end || *end != '\0') {
                continue;
            }
            // pid (comm) state ppid ...: comm may hold spaces and parentheses.
            const std::string stat = readFirstLine(std::string("/proc/") + entry->d_name + "/stat");
            const std::string::size_type open = stat.find('(');
            const std::string::size_type close = stat.rfind(')');
            if (open == std::string::npos || close == std::string::npos || close < open) {
                continue;
            }
            char state = 0;
            long ppid = 0;
            if (std::sscanf(stat.c_str() + close + 1, " %c %ld", &state, &ppid) != 2) {
                continue;
            }
            if (ppid == self && state != 'Z' && state != 'X' && stat.compare(open + 1, close - open - 1, "execComp") == 0) {
                pids.push_back(entry->d_name);
            }
        }
        closedir(proc);

        return pids;
    }

    // Each thread's name, state and kernel wait channel, which need no ptrace.
    static void dumpThreadStates(const std::string& pid)
    {
        const std::string tasks = "/proc/" + pid + "/task";
        DIR* dir = opendir(tasks.c_str());
        if (!dir) {
            writeStderr("watchdog: its threads cannot be listed\n");

            return;
        }
        while (const struct dirent* entry = readdir(dir)) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            const std::string task = tasks + "/" + entry->d_name;
            const std::string stat = readFirstLine(task + "/stat");
            const std::string::size_type close = stat.rfind(')');
            char state = '?';
            if (close != std::string::npos) {
                (void)std::sscanf(stat.c_str() + close + 1, " %c", &state);
            }
            const std::string text = "TID " + std::string(entry->d_name) + " (" + readFirstLine(task + "/comm") + ") state " + state + " wchan " + readFirstLine(task + "/wchan") + "\n";
            writeStderr(text.c_str(), static_cast<int>(text.size()));
        }
        closedir(dir);
    }

    // A stalled MoonRay render may be waiting on its render process: its stacks, in whatever of
    // the dump's budget the test process's own left. Under Yama's default ptrace scope a sibling
    // tool cannot attach to them without CAP_SYS_PTRACE, so their thread states stand in.
    static void dumpRenderProcessStacks(long deadline)
    {
        if (::testing::internal::InDeathTestChild()) {
            return;
        }
        for (const std::string& pid : renderProcesses()) {
            const std::string state = readFirstLine("/proc/" + pid + "/stat");
            const std::string::size_type close = state.rfind(')');
            char code = '?';
            if (close != std::string::npos) {
                (void)std::sscanf(state.c_str() + close + 1, " %c", &code);
            }
            const std::string header = "watchdog: execComp render process " + pid + ", state " + code + "; stack dump follows\n";
            writeStderr(header.c_str(), static_cast<int>(header.size()));
            if (monotonicMilliseconds() >= deadline) {
                writeStderr("watchdog: the dump budget is spent; thread states only\n");
                dumpThreadStates(pid);
                continue;
            }
            if (!dumpStacksOf(pid.c_str(), deadline)) {
                writeStderr("watchdog: no stacks for it (ptrace refused, or no tool); thread states instead\n");
                dumpThreadStates(pid);
            }
        }
    }

    // A crash inside malloc or stdio would deadlock the dump; the signal then ends the process anyway.
    static void onDumpTimeout(int)
    {
        dieOf(fatalSignal());
    }

    static void dieOf(int sig)
    {
        struct sigaction fallback;
        std::memset(&fallback, 0, sizeof(fallback));
        fallback.sa_handler = SIG_DFL;
        sigemptyset(&fallback.sa_mask);
        sigaction(sig, &fallback, nullptr);
        raise(sig);
    }

    // True if this thread is the one to take the dump; false if another thread took it or is
    // taking it, or if this thread is already taking it and faulted again.
    static bool claimDump()
    {
        pid_t expected = 0;
        const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
        if (dumpingThread().compare_exchange_strong(expected, self)) {
            return true;
        }
        if (expected != self) {
            const long deadline = monotonicMilliseconds() + (kCrashDumpAlarmSeconds + 5) * 1000L;
            while (!dumpDone().load() && monotonicMilliseconds() < deadline) {
                sleepMilliseconds(50);
            }
        }

        return false;
    }

    static void onFatalSignal(int sig)
    {
        if (claimDump()) {
            fatalSignal() = sig;
            struct sigaction guard;
            std::memset(&guard, 0, sizeof(guard));
            guard.sa_handler = &onDumpTimeout;
            sigemptyset(&guard.sa_mask);
            struct sigaction previousAlarm;
            sigaction(SIGALRM, &guard, &previousAlarm);
            const unsigned previousRemaining = alarm(kCrashDumpAlarmSeconds);
            const long startedAt = monotonicMilliseconds();

            const ::testing::TestInfo* info = currentTest().load();
            if (!info) {
                // gtest forwards no test events in a death-test child. Asking it takes its mutex,
                // which it holds only around its own bookkeeping.
                info = ::testing::UnitTest::GetInstance()->current_test_info();
            }
            Line line;
            line.addText("watchdog: fatal signal ");
            line.addNumber(sig);
            line.addText(" in ");
            if (info) {
                line.addText(info->test_case_name());
                line.addText(".");
                line.addText(info->name());
            } else {
                line.addText("(no test)");
            }
            line.addText(", phase '");
            line.addText(phase().load());
            line.addText("'; crashing thread then every thread follows\n");
            writeStderr(line.c_str(), line.length());
            // Not async-signal-safe, but installCrashHandler() has already loaded what it needs.
            void* frames[128];
            const int count = backtrace(frames, 128);
            backtrace_symbols_fd(frames, count, STDERR_FILENO);
            dumpStacks();

            alarm(0);
            sigaction(SIGALRM, &previousAlarm, nullptr);
            if (previousRemaining > 0) {
                const long elapsed = (monotonicMilliseconds() - startedAt) / 1000L;
                alarm(static_cast<long>(previousRemaining) > elapsed ? previousRemaining - static_cast<unsigned>(elapsed) : 1);
            }
            dumpDone().store(true);
        }
        // SA_NODEFER left the signal unblocked, so the process dies of it here, with the status
        // ctest would have seen without the handler.
        dieOf(sig);
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

    // Claims the dump first, so the abort that follows does not dump a second time.
    [[noreturn]] static void expire(const char* what, int limit)
    {
        std::fprintf(stderr, "watchdog: %s stalled for %d s in phase '%s'; stack dump follows\n", what, limit, phase().load());
        if (claimDump()) {
            const long deadline = monotonicMilliseconds() + kDumpBudgetSeconds * 1000L;
            dumpStacks(deadline);
            dumpRenderProcessStacks(deadline);
            dumpDone().store(true);
        }
        std::abort();
    }

    // True if the tool ran to completion and exited 0 before deadline, a monotonicMilliseconds()
    // time; its output goes to stderr.
    static bool run(const char* path,
                    const char* const* argv,
                    long deadline)
    {
        if (!path[0]) {
            return false;
        }
        const pid_t child = fork();
        if (child < 0) {
            return false;
        }
        if (child == 0) {
            dup2(STDERR_FILENO, STDOUT_FILENO);
            execve(path, const_cast<char* const*>(argv), environ);
            _exit(127);
        }
        while (monotonicMilliseconds() < deadline) {
            int status = 0;
            const pid_t done = waitpid(child, &status, WNOHANG);
            if (done == child) {
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            if (done < 0) {
                return false;
            }
            sleepMilliseconds(50);
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
        TestWatchdog::listenForCurrentTest();
        TestWatchdog::armStallDump(TestWatchdog::stallDumpSeconds());
        TestWatchdog::reportNonZeroExits();
    }

    virtual void TearDown() override
    {
        TestWatchdog::armExitWatchdog(TestWatchdog::envSeconds("NATRON_TEST_EXIT_WATCHDOG_S", TestWatchdog::kDefaultExitLimitSeconds));
    }
};

inline ::testing::Environment* const processEnvironment = ::testing::AddGlobalTestEnvironment(new ProcessEnvironment);
} // namespace TestWatchdogDetail

#endif // NATRON_TESTS_TESTWATCHDOG_H
