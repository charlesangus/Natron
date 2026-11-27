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

#include "MoonRayRuntime.h"

#include <cpuid.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
isFile(const std::string& path)
{
    struct stat info;

    return (::stat(path.c_str(), &info) == 0) && S_ISREG(info.st_mode);
}

void
prependToEnv(const char* name,
             const std::string& dir)
{
    const char* current = std::getenv(name);
    std::string value = dir;

    if (current && *current) {
        value += ":";
        value += current;
    }
    ::setenv(name, value.c_str(), 1);
}

// The unquoted value of one /etc/os-release field, or fallback when the file or field is missing.
std::string
osReleaseField(const std::string& field,
               const std::string& fallback)
{
    std::ifstream file("/etc/os-release");
    std::string line;
    const std::string key = field + "=";

    while (std::getline(file, line)) {
        if (line.compare(0, key.size(), key) != 0) {
            continue;
        }
        std::string value = line.substr(key.size());
        if ((value.size() >= 2) && ((value.front() == '"') || (value.front() == '\'')) && (value.back() == value.front())) {
            value = value.substr(1, value.size() - 2);
        }
        if (!value.empty()) {
            return value;
        }
    }

    return fallback;
}

void
setEnvIfEmpty(const char* name,
              const std::string& value)
{
    const char* current = std::getenv(name);

    if (!current || !*current) {
        ::setenv(name, value.c_str(), 1);
    }
}

const char* const kRenderProcessName = "execComp";

// Whether pid is a child of this process named kRenderProcessName that has not exited.
bool
isRunningRenderProcess(const std::string& pid)
{
    std::ifstream file("/proc/" + pid + "/stat");
    std::string stat;

    if (!std::getline(file, stat)) {
        return false;
    }
    // pid (comm) state ppid ...: comm may hold spaces and parentheses, so it ends at the last ')'.
    const std::string::size_type open = stat.find('(');
    const std::string::size_type close = stat.rfind(')');
    if ((open == std::string::npos) || (close == std::string::npos) || (close < open)) {
        return false;
    }
    std::istringstream rest(stat.substr(close + 1));
    char state = 0;
    long ppid = 0;
    if (!(rest >> state >> ppid) || (ppid != (long)::getpid()) || (state == 'Z') || (state == 'X')) {
        return false;
    }

    return stat.compare(open + 1, close - open - 1, kRenderProcessName) == 0;
}

std::vector<pid_t>
runningRenderProcesses()
{
    std::vector<pid_t> pids;
    DIR* proc = ::opendir("/proc");

    if (!proc) {
        return pids;
    }
    while (const dirent* entry = ::readdir(proc)) {
        char* end = nullptr;
        const long pid = std::strtol(entry->d_name, &end, 10);
        if ((pid > 0) && end && (*end == '\0') && isRunningRenderProcess(entry->d_name)) {
            pids.push_back((pid_t)pid);
        }
    }
    ::closedir(proc);

    return pids;
}

// How long execComp's group gets to exit after a SIGTERM before it is killed. Arras stops a session
// by asking execComp to stop, waiting STOP_WAIT (5 s) for it, then SIGTERMs the group and waits
// another 5 s before its SIGKILL. The grace before the SIGTERM covers Arras's STOP_WAIT; this final
// grace is kept short so a render process that ignores SIGTERM cannot hold up the exit.
const int kTermGraceMs = 500;

// Signals pid's process group, which Arras makes execComp lead, if pid is still an unreaped
// execComp child of this process. Arras reaps its processes on a thread of its own, after which the
// pid may be reused by an unrelated process: one is never signalled. False if pid was not signalled.
bool
signalRenderProcessGroup(pid_t pid,
                         int signum)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    const int pidfd = (int)::syscall(SYS_pidfd_open, pid, 0);
    if (pidfd >= 0) {
        // The pidfd pins the process checked: until it is reaped its pid, the group's id, cannot be
        // reused, and pidfd_send_signal fails once it has been.
        bool signalled = false;
        if (isRunningRenderProcess(std::to_string(pid))) {
            signalled = (::syscall(SYS_pidfd_send_signal, pidfd, signum, nullptr, 0) == 0);
            if (signalled) {
                (void)::kill(-pid, signum);
            }
        }
        ::close(pidfd);

        return signalled;
    }
    if (errno != ENOSYS) {
        // ESRCH among them: the process is gone and its pid may already be another's.
        return false;
    }
#endif
    // No pidfd on this kernel: rechecked right before the signal, which leaves the narrowest window.
    if (!isRunningRenderProcess(std::to_string(pid))) {
        return false;
    }
    (void)::kill(-pid, signum);

    return true;
}

// Waits until deadline for every pid in *running to have exited, keeping those still running.
void
waitForRenderProcesses(std::vector<pid_t>* running,
                       std::chrono::steady_clock::time_point deadline)
{
    for (;;) {
        running->erase(std::remove_if(running->begin(), running->end(),
                                      [](pid_t pid) { return !isRunningRenderProcess(std::to_string(pid)); }),
                       running->end());
        if (running->empty() || (std::chrono::steady_clock::now() >= deadline)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
} // anon namespace

namespace MoonRayRuntime {
// MoonRay is compiled with -march=core-avx2 throughout, so loading any of it on an older CPU
// faults on the first such instruction. Every feature that march implies is checked: SSE4.2,
// POPCNT, AVX, AVX2, FMA, F16C, BMI1 and BMI2 through GCC's CPU probe, which also checks that the OS
// saves the AVX register state, and LZCNT and MOVBE through CPUID, as the probe has no reliable
// name for them across the GCC versions the build images carry.
bool
cpuRunsMoonRay()
{
    __builtin_cpu_init();
    if (!(__builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt") && __builtin_cpu_supports("avx") && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("f16c") && __builtin_cpu_supports("bmi") && __builtin_cpu_supports("bmi2"))) {
        return false;
    }
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx & bit_MOVBE)) {
        return false;
    }
    if (!__get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx) || !(ecx & bit_LZCNT)) {
        return false;
    }

    return true;
}

std::string
setUp(const std::string& applicationDirPath)
{
    static const std::string installed = [&applicationDirPath]() -> std::string {
        const std::string pluginInfo = "/plugin/pxr/plugInfo.json";
        std::string prefix = applicationDirPath + "/../moonray";

        if (!isFile(prefix + pluginInfo)) {
#ifdef NATRON_MOONRAY_PREFIX
            prefix = NATRON_MOONRAY_PREFIX;
            if (!isFile(prefix + pluginInfo)) {
                return std::string();
            }
#else
            return std::string();
#endif
        }
        if (char* resolved = ::realpath(prefix.c_str(), nullptr)) {
            prefix = resolved;
            std::free(resolved);
        }
        if (!cpuRunsMoonRay()) {
            std::cerr << "MoonRay needs a CPU with AVX2, FMA, BMI1/2, F16C, LZCNT and MOVBE; its render delegate is not registered" << std::endl;

            return std::string();
        }

        // Ours first, so that execComp and the built-in DSOs match the delegate that loads them.
        prependToEnv("RDL2_DSO_PATH", prefix + "/rdl2dso");
        prependToEnv("ARRAS_SESSION_PATH", prefix + "/sessions");
        prependToEnv("PATH", prefix + "/bin");
        // Arras runs lsb_release, which many distributions do not install, unless both are set; it
        // only reports them in its session requests.
        setEnvIfEmpty("ARRAS_OS_DISTRIBUTION", osReleaseField("ID", "linux"));
        setEnvIfEmpty("ARRAS_OS_VERSION", osReleaseField("VERSION_ID", "unknown"));

        if (PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(prefix + pluginInfo).empty()) {
            std::cerr << "MoonRay at " << prefix << " registered no USD plug-ins" << std::endl;
        }

        return prefix;
    }();

    return installed;
}

int
endRenderProcesses(int timeoutMs)
{
    using Clock = std::chrono::steady_clock;
    const int totalMs = std::max(0, timeoutMs);
    const int termGraceMs = std::min(kTermGraceMs, totalMs / 4);
    const Clock::time_point start = Clock::now();
    std::vector<pid_t> running = runningRenderProcesses();

    waitForRenderProcesses(&running, start + std::chrono::milliseconds(totalMs - termGraceMs));
    if (running.empty()) {
        return 0;
    }
    const int stopped = (int)running.size();
    for (pid_t pid : running) {
        std::cerr << "MoonRay's render process " << pid << " did not exit; terminating it" << std::endl;
        (void)signalRenderProcessGroup(pid, SIGTERM);
    }
    waitForRenderProcesses(&running, start + std::chrono::milliseconds(totalMs));
    for (pid_t pid : running) {
        std::cerr << "MoonRay's render process " << pid << " did not terminate; killing it" << std::endl;
        (void)signalRenderProcessGroup(pid, SIGKILL);
    }

    return stopped;
}
} // namespace MoonRayRuntime

NATRON_NAMESPACE_EXIT
