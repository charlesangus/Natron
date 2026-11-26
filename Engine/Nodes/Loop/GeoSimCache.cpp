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

#include "GeoSimCache.h"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <set>
#include <system_error>
#include <thread>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "Engine/Geo/GeoFileFormat.h"

NATRON_NAMESPACE_ENTER

namespace {

std::atomic<U64> gNextCacheId { 0 };

// One thread, in order: a directory queued after its files is removed once they are.
class DiskJobs {
public:
    static DiskJobs& instance()
    {
        // Never destroyed: the detached worker may still be running at exit.
        static DiskJobs* jobs = new DiskJobs();

        return *jobs;
    }

    void post(std::function<void()> job)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        _jobs.push_back(std::move(job));
        if (!_started) {
            _started = true;
            std::thread([this]() { run(); }).detach();
        }
        _wake.notify_one();
    }

    // True the first time parent is asked about.
    bool firstUseOf(const std::string& parent)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        return _parents.insert(parent).second;
    }

private:
    void run()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _wake.wait(lock, [this]() { return !_jobs.empty(); });
                job = std::move(_jobs.front());
                _jobs.pop_front();
            }
            try {
                job();
            } catch (...) {
            }
        }
    }

    std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<std::function<void()>> _jobs;
    std::set<std::string> _parents;
    bool _started = false;
};

// Removes files and empty directories, in order, in the background.
void
removeLater(std::vector<std::string> paths)
{
    if (paths.empty()) {
        return;
    }
    DiskJobs::instance().post([paths = std::move(paths)]() {
        for (const std::string& path : paths) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(path), ec);
        }
    });
}

bool
processIsRunning(long pid)
{
    if (pid <= 0) {
        return true;
    }

    return (::kill((pid_t)pid, 0) == 0) || (errno == EPERM);
}

} // anonymous namespace

GeoSimCache::GeoSimCache(std::size_t budget)
    : _budget(budget)
{
}

GeoSimCache::~GeoSimCache()
{
    std::vector<std::string> files;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        for (SeriesIt it = _series.begin(); it != _series.end(); ++it) {
            for (std::map<int, std::string>::const_iterator f = it->second.spilled.begin(); f != it->second.spilled.end(); ++f) {
                files.push_back(f->second);
            }
        }
        _series.clear();
        _lru.clear();
        if (!_spillDirectory.empty()) {
            // Only an empty directory goes: never anything this cache did not write.
            files.push_back(_spillDirectory);
        }
    }
    removeLater(std::move(files));
}

void
GeoSimCache::setBudget(std::size_t budget)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _budget = budget;
}

std::size_t
GeoSimCache::getBudget() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _budget;
}

void
GeoSimCache::setSpillParent(const std::string& parent)
{
    std::vector<std::string> files;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const std::filesystem::path current(_spillDirectory);

        if (!_spillDirectory.empty() && (current.parent_path() == std::filesystem::path(parent))) {
            return;
        }
        if (_spillDirectory.empty() && parent.empty()) {
            return;
        }
        for (SeriesIt it = _series.begin(); it != _series.end();) {
            Frames& frames = it->second;
            for (std::map<int, std::string>::const_iterator f = frames.spilled.begin(); f != frames.spilled.end(); ++f) {
                files.push_back(f->second);
            }
            frames.spilled.clear();
            // A write still running finds its frame gone and removes its file.
            frames.spilling.clear();
            const SeriesIt next = std::next(it);
            eraseIfEmptyLocked(it);
            it = next;
        }
        if (!_spillDirectory.empty()) {
            files.push_back(_spillDirectory);
        }
        if (parent.empty()) {
            _spillDirectory.clear();
        } else {
            char name[64];
            std::snprintf(name, sizeof(name), "%ld-%llu", (long)::getpid(), (unsigned long long)gNextCacheId.fetch_add(1));
            _spillDirectory = (std::filesystem::path(parent) / name).string();
        }
    }
    removeLater(std::move(files));
    if (!parent.empty() && DiskJobs::instance().firstUseOf(parent)) {
        DiskJobs::instance().post([parent]() { GeoSimCache::removeOrphanedSpills(parent); });
    }
}

std::string
GeoSimCache::spillDirectory() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _spillDirectory;
}

void
GeoSimCache::removeOrphanedSpills(const std::string& parent)
{
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(parent), ec);

    if (ec) {
        return;
    }
    const long self = (long)::getpid();
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            return;
        }
        std::error_code typeError;
        if (!it->is_directory(typeError)) {
            continue;
        }
        const std::string name = it->path().filename().string();
        char* rest = NULL;
        errno = 0;
        const long pid = std::strtol(name.c_str(), &rest, 10);
        if ((errno != 0) || (rest == name.c_str()) || (*rest != '-') || (pid == self) || processIsRunning(pid)) {
            continue;
        }
        std::error_code removeError;
        std::filesystem::remove_all(it->path(), removeError);
    }
}

std::string
GeoSimCache::spillPathLocked(const GeoSimSeries& series,
                             int frame) const
{
    char name[128];
    std::snprintf(name, sizeof(name), "%016llx-%016llx-v%d-f%d.%s", (unsigned long long)series.hash, (unsigned long long)series.contextKey,
                  series.view, frame, kGeoFileExtension);

    return (std::filesystem::path(_spillDirectory) / name).string();
}

void
GeoSimCache::insert(const GeoSimSeries& series,
                    int frame,
                    const GeoDetailConstPtr& detail)
{
    if (!detail) {
        return;
    }
    std::vector<SpillWrite> writes;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        insertLocked(series, frame, detail);
        evictLocked(Key(series, frame), &writes);
    }
    writeSpills(writes);
}

void
GeoSimCache::insertLocked(const GeoSimSeries& series,
                          int frame,
                          const GeoDetailConstPtr& detail)
{
    Frames& frames = _series[series];
    std::map<int, Entry>::iterator found = frames.ram.find(frame);

    if (found == frames.ram.end()) {
        found = frames.ram.insert(std::make_pair(frame, Entry())).first;
        found->second.lru = _lru.insert(_lru.end(), Key(series, frame));
    } else {
        _bytes -= found->second.bytes;
        _lru.splice(_lru.end(), _lru, found->second.lru);
    }
    found->second.detail = detail;
    found->second.bytes = detail->getSizeInBytes();
    _bytes += found->second.bytes;
}

void
GeoSimCache::forgetLocked(Frames* frames,
                          std::map<int, Entry>::iterator ram)
{
    _bytes -= ram->second.bytes;
    _lru.erase(ram->second.lru);
    frames->ram.erase(ram);
}

void
GeoSimCache::dropSeriesLocked(SeriesIt series,
                              std::vector<std::string>* files)
{
    Frames& frames = series->second;

    for (std::map<int, Entry>::iterator e = frames.ram.begin(); e != frames.ram.end(); ++e) {
        _bytes -= e->second.bytes;
        _lru.erase(e->second.lru);
    }
    for (std::map<int, std::string>::const_iterator f = frames.spilled.begin(); f != frames.spilled.end(); ++f) {
        files->push_back(f->second);
    }
    _series.erase(series);
}

void
GeoSimCache::eraseIfEmptyLocked(SeriesIt series)
{
    const Frames& frames = series->second;

    if (frames.ram.empty() && frames.spilling.empty() && frames.spilled.empty()) {
        _series.erase(series);
    }
}

void
GeoSimCache::evictLocked(const Key& kept,
                         std::vector<SpillWrite>* writes)
{
    std::list<Key>::iterator it = _lru.begin();

    while ((_bytes > _budget) && (it != _lru.end())) {
        if (*it == kept) {
            ++it;
            continue;
        }
        const Key key = *it;
        ++it;
        const SeriesIt s = _series.find(key.first);
        Frames& frames = s->second;
        const std::map<int, Entry>::iterator e = frames.ram.find(key.second);
        const GeoDetailConstPtr detail = e->second.detail;
        forgetLocked(&frames, e);
        const bool onDisk = (frames.spilled.find(key.second) != frames.spilled.end()) || (frames.spilling.find(key.second) != frames.spilling.end());
        if (!_spillDirectory.empty() && !onDisk) {
            SpillWrite write;
            write.key = key;
            write.detail = detail;
            write.token = ++_nextToken;
            write.directory = _spillDirectory;
            write.path = spillPathLocked(key.first, key.second);
            Spilling& spilling = frames.spilling[key.second];
            spilling.detail = detail;
            spilling.token = write.token;
            writes->push_back(std::move(write));
        } else {
            eraseIfEmptyLocked(s);
        }
    }
}

void
GeoSimCache::writeSpills(const std::vector<SpillWrite>& writes)
{
    GeoFileWriteOptions options;
    // Scratch data: a frame lost to a crash is solved again.
    options.durable = false;

    for (const SpillWrite& write : writes) {
        bool written = false;
        try {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(write.directory), ec);
            geoFileWrite(*write.detail, write.path, options);
            written = true;
        } catch (const std::exception&) {
            // A frame that cannot be spilled (disk full, say) is solved again when asked for.
        }
        bool kept = false;
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            const SeriesIt s = _series.find(write.key.first);
            if (s != _series.end()) {
                Frames& frames = s->second;
                std::map<int, Spilling>::iterator spilling = frames.spilling.find(write.key.second);
                if ((spilling != frames.spilling.end()) && (spilling->second.token == write.token)) {
                    frames.spilling.erase(spilling);
                    if (written && (write.directory == _spillDirectory)) {
                        frames.spilled[write.key.second] = write.path;
                        kept = true;
                    }
                    eraseIfEmptyLocked(s);
                }
            }
            stale = (write.directory != _spillDirectory);
        }
        if (written && !kept) {
            std::vector<std::string> files(1, write.path);
            if (stale) {
                files.push_back(write.directory);
            }
            removeLater(std::move(files));
        }
    }
}

GeoDetailConstPtr
GeoSimCache::find(const GeoSimSeries& series,
                  int frame)
{
    std::string path;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const SeriesIt s = _series.find(series);

        if (s == _series.end()) {
            return GeoDetailConstPtr();
        }
        Frames& frames = s->second;
        const std::map<int, Entry>::iterator ram = frames.ram.find(frame);
        if (ram != frames.ram.end()) {
            _lru.splice(_lru.end(), _lru, ram->second.lru);

            return ram->second.detail;
        }
        const std::map<int, Spilling>::const_iterator spilling = frames.spilling.find(frame);
        if (spilling != frames.spilling.end()) {
            return spilling->second.detail;
        }
        const std::map<int, std::string>::const_iterator spilled = frames.spilled.find(frame);
        if (spilled == frames.spilled.end()) {
            return GeoDetailConstPtr();
        }
        path = spilled->second;
    }

    GeoDetailConstPtr detail;
    try {
        detail = std::make_shared<const GeoDetail>(geoFileRead(path));
    } catch (const GeoFileError&) {
    }

    std::vector<SpillWrite> writes;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const SeriesIt s = _series.find(series);

        if (s == _series.end()) {
            return GeoDetailConstPtr();
        }
        Frames& frames = s->second;
        const std::map<int, Entry>::iterator ram = frames.ram.find(frame);
        if (ram != frames.ram.end()) {
            _lru.splice(_lru.end(), _lru, ram->second.lru);

            return ram->second.detail;
        }
        const std::map<int, std::string>::iterator spilled = frames.spilled.find(frame);
        if ((spilled == frames.spilled.end()) || (spilled->second != path)) {
            // Forgotten while it was read.
            return GeoDetailConstPtr();
        }
        if (!detail) {
            frames.spilled.erase(spilled);
            eraseIfEmptyLocked(s);
        } else {
            insertLocked(series, frame, detail);
            evictLocked(Key(series, frame), &writes);
        }
    }
    if (!detail) {
        removeLater(std::vector<std::string>(1, path));

        return GeoDetailConstPtr();
    }
    writeSpills(writes);

    return detail;
}

bool
GeoSimCache::latest(const GeoSimSeries& series,
                    int first,
                    int last,
                    int* frame) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<GeoSimSeries, Frames>::const_iterator s = _series.find(series);

    if ((s == _series.end()) || (last < first)) {
        return false;
    }
    bool found = false;
    const auto consider = [&](const auto& frames) {
        auto it = frames.upper_bound(last);
        if (it == frames.begin()) {
            return;
        }
        --it;
        if ((it->first >= first) && (!found || (it->first > *frame))) {
            *frame = it->first;
            found = true;
        }
    };
    consider(s->second.ram);
    consider(s->second.spilling);
    consider(s->second.spilled);

    return found;
}

std::map<int, bool>
GeoSimCache::frames(const GeoSimSeries& series) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<GeoSimSeries, Frames>::const_iterator s = _series.find(series);
    std::map<int, bool> kept;

    if (s == _series.end()) {
        return kept;
    }
    for (const auto& frame : s->second.spilled) {
        kept[frame.first] = false;
    }
    // A frame being written to disk is still in RAM.
    for (const auto& frame : s->second.spilling) {
        kept[frame.first] = true;
    }
    for (const auto& frame : s->second.ram) {
        kept[frame.first] = true;
    }

    return kept;
}

void
GeoSimCache::retainOnly(const GeoSimSeries& series,
                        int frame)
{
    std::vector<std::string> files;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const SeriesIt s = _series.find(series);

        if (s == _series.end()) {
            return;
        }
        Frames& frames = s->second;
        for (std::map<int, Entry>::iterator it = frames.ram.begin(); it != frames.ram.end();) {
            if (it->first == frame) {
                ++it;
            } else {
                const std::map<int, Entry>::iterator next = std::next(it);
                forgetLocked(&frames, it);
                it = next;
            }
        }
        for (std::map<int, Spilling>::iterator it = frames.spilling.begin(); it != frames.spilling.end();) {
            it = (it->first == frame) ? std::next(it) : frames.spilling.erase(it);
        }
        for (std::map<int, std::string>::iterator it = frames.spilled.begin(); it != frames.spilled.end();) {
            if (it->first == frame) {
                ++it;
            } else {
                files.push_back(it->second);
                it = frames.spilled.erase(it);
            }
        }
        eraseIfEmptyLocked(s);
    }
    removeLater(std::move(files));
}

void
GeoSimCache::retainHash(U64 hash)
{
    std::vector<std::string> files;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        for (SeriesIt it = _series.begin(); it != _series.end();) {
            const SeriesIt next = std::next(it);
            if (it->first.hash != hash) {
                dropSeriesLocked(it, &files);
            }
            it = next;
        }
    }
    removeLater(std::move(files));
}

void
GeoSimCache::clear()
{
    std::vector<std::string> files;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        for (SeriesIt it = _series.begin(); it != _series.end(); ++it) {
            for (std::map<int, std::string>::const_iterator f = it->second.spilled.begin(); f != it->second.spilled.end(); ++f) {
                files.push_back(f->second);
            }
        }
        _series.clear();
        _lru.clear();
        _bytes = 0;
    }
    removeLater(std::move(files));
}

std::size_t
GeoSimCache::ramBytes() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _bytes;
}

std::size_t
GeoSimCache::ramFrameCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _lru.size();
}

std::size_t
GeoSimCache::spilledFrameCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::size_t count = 0;

    for (std::map<GeoSimSeries, Frames>::const_iterator it = _series.begin(); it != _series.end(); ++it) {
        count += it->second.spilled.size();
    }

    return count;
}

NATRON_NAMESPACE_EXIT
