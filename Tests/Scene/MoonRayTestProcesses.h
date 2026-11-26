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

#ifndef NATRON_TESTS_SCENE_MOONRAYTESTPROCESSES_H
#define NATRON_TESTS_SCENE_MOONRAYTESTPROCESSES_H

// Include after Python.h and UsdIncludes.h, as the tests that use it do.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <signal.h>
#include <unistd.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/HydraRenderImpl.h"

namespace MoonRayTestProcesses {
// The ids of this process's children named execComp: the processes Arras renders in.
inline std::vector<int>
execCompChildren()
{
    std::vector<int> children;
    DIR* proc = ::opendir("/proc");
    if (!proc) {
        return children;
    }
    const int self = (int)::getpid();
    while (struct dirent* entry = ::readdir(proc)) {
        const int pid = std::atoi(entry->d_name);
        if (pid <= 0) {
            continue;
        }
        std::ifstream stat(std::string("/proc/") + entry->d_name + "/stat");
        std::string line;
        if (!std::getline(stat, line)) {
            continue;
        }
        // pid (comm) state ppid ...
        const std::string::size_type open = line.find('(');
        const std::string::size_type close = line.rfind(')');
        if ((open == std::string::npos) || (close == std::string::npos)) {
            continue;
        }
        const std::string comm = line.substr(open + 1, close - open - 1);
        std::istringstream rest(line.substr(close + 1));
        std::string state;
        int ppid = 0;
        rest >> state >> ppid;
        if ((ppid == self) && (comm == "execComp") && (state != "Z")) {
            children.push_back(pid);
        }
    }
    ::closedir(proc);

    return children;
}

// Sets an environment variable for its lifetime, and puts back what it held before.
class ScopedEnv {
public:
    ScopedEnv(const char* name,
              const char* value)
        : _name(name)
    {
        const char* previous = std::getenv(name);
        _had = (previous != nullptr);
        if (_had) {
            _previous = previous;
        }
        ::setenv(name, value, 1);
    }

    ~ScopedEnv()
    {
        if (_had) {
            ::setenv(_name.c_str(), _previous.c_str(), 1);
        } else {
            ::unsetenv(_name.c_str());
        }
    }

private:
    std::string _name;
    std::string _previous;
    bool _had = false;
};

// Whether pid is a child of this process that Arras has not reaped yet, exited or not.
inline bool
isUnreapedChild(int pid)
{
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) {
        return false;
    }
    const std::string::size_type close = line.rfind(')');
    if (close == std::string::npos) {
        return false;
    }
    std::istringstream rest(line.substr(close + 1));
    std::string state;
    int ppid = 0;
    rest >> state >> ppid;

    return ppid == (int)::getpid();
}

/**
 * Stops each execComp this process starts once a render has read its session's progress past
 * zero, so never before its session is connected: one stopped while it connects fails the session's
 * making instead, and leaves Arras a process whose exit reaches a client already gone. On going it
 * continues the ones it stopped and waits for Arras to reap them, so that none ends its session
 * inside a later test.
 **/
class RenderProcessStopper {
public:
    explicit RenderProcessStopper(std::chrono::milliseconds reapBound)
        : _before(execCompChildren())
        , _reapBound(reapBound)
        , _thread([this]() { run(); })
    {
    }

    ~RenderProcessStopper()
    {
        (void)stopNoMore();
        (void)continueAndReap();
    }

    RenderProcessStopper(const RenderProcessStopper&) = delete;
    RenderProcessStopper& operator=(const RenderProcessStopper&) = delete;

    // The processes stopped, in the order they were.
    std::vector<int> stopNoMore()
    {
        _running = false;
        if (_thread.joinable()) {
            _thread.join();
        }
        std::lock_guard<std::mutex> lock(_mutex);

        return _stopped;
    }

    // False if one of the stopped processes was not reaped within the bound.
    bool continueAndReap()
    {
        const std::vector<int> stopped = stopNoMore();
        for (int pid : stopped) {
            if (isUnreapedChild(pid)) {
                (void)::kill(pid, SIGCONT);
            }
        }
        typedef std::chrono::steady_clock Clock;
        const Clock::time_point start = Clock::now();
        for (;;) {
            const bool anyLeft = std::any_of(stopped.begin(), stopped.end(), isUnreapedChild);
            if (!anyLeft) {
                return true;
            }
            if (Clock::now() - start >= _reapBound) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

private:
    void run()
    {
        std::map<int, std::uint64_t> readsWhenSeen;
        while (_running.load()) {
            const std::vector<int> children = execCompChildren();
            // Read after the listing: every read counted by then is of a session older than the
            // processes listed, whose own reads can only come after.
            const std::uint64_t reads = NATRON_NAMESPACE::HydraRender::getMoonRayProgressReads();
            for (int pid : children) {
                if (std::find(_before.begin(), _before.end(), pid) != _before.end()) {
                    continue;
                }
                const std::map<int, std::uint64_t>::iterator it = readsWhenSeen.insert(std::make_pair(pid, reads)).first;
                std::lock_guard<std::mutex> lock(_mutex);
                if ((reads > it->second) && (std::find(_stopped.begin(), _stopped.end(), pid) == _stopped.end())) {
                    if (::kill(pid, SIGSTOP) == 0) {
                        _stopped.push_back(pid);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    const std::vector<int> _before;
    const std::chrono::milliseconds _reapBound;
    std::atomic<bool> _running { true };
    std::mutex _mutex;
    std::vector<int> _stopped;
    std::thread _thread;
};
} // namespace MoonRayTestProcesses

#endif // NATRON_TESTS_SCENE_MOONRAYTESTPROCESSES_H
