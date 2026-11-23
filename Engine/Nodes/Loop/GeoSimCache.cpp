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
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <system_error>

#include <unistd.h>

#include "Engine/Geo/GeoFileFormat.h"

NATRON_NAMESPACE_ENTER

namespace {

std::atomic<U64> gNextCacheId { 0 };

void
removeFile(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path), ec);
}

} // anonymous namespace

GeoSimCache::GeoSimCache(std::size_t budget)
    : _budget(budget)
{
}

GeoSimCache::~GeoSimCache()
{
    std::lock_guard<std::mutex> lock(_mutex);

    for (std::map<GeoSimSeries, Frames>::iterator it = _series.begin(); it != _series.end(); ++it) {
        removeSpilledLocked(&it->second);
    }
    _series.clear();
    if (!_spillDirectory.empty()) {
        // Only when empty: never anything this cache did not write.
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(_spillDirectory), ec);
    }
}

void
GeoSimCache::setBudget(std::size_t budget)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _budget = budget;
    evictLocked(GeoSimSeries(), 0);
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
    std::lock_guard<std::mutex> lock(_mutex);
    const std::filesystem::path current(_spillDirectory);

    if (!_spillDirectory.empty() && (current.parent_path() == std::filesystem::path(parent))) {
        return;
    }
    for (std::map<GeoSimSeries, Frames>::iterator it = _series.begin(); it != _series.end();) {
        removeSpilledLocked(&it->second);
        it = it->second.ram.empty() ? _series.erase(it) : std::next(it);
    }
    if (!_spillDirectory.empty()) {
        std::error_code ec;
        std::filesystem::remove(current, ec);
    }
    if (parent.empty()) {
        _spillDirectory.clear();

        return;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "%ld-%llu", (long)::getpid(), (unsigned long long)gNextCacheId.fetch_add(1));
    _spillDirectory = (std::filesystem::path(parent) / name).string();
}

std::string
GeoSimCache::spillDirectory() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _spillDirectory;
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
    std::lock_guard<std::mutex> lock(_mutex);

    insertLocked(series, frame, detail);
    evictLocked(series, frame);
}

void
GeoSimCache::insertLocked(const GeoSimSeries& series,
                          int frame,
                          const GeoDetailConstPtr& detail)
{
    Frames& frames = _series[series];
    std::map<int, Entry>::iterator found = frames.ram.find(frame);

    if (found != frames.ram.end()) {
        _bytes -= found->second.bytes;
    }
    Entry& entry = frames.ram[frame];
    entry.detail = detail;
    entry.bytes = detail->getSizeInBytes();
    entry.lastUse = ++_clock;
    _bytes += entry.bytes;
}

void
GeoSimCache::forgetLocked(Frames* frames,
                          std::map<int, Entry>::iterator ram)
{
    _bytes -= ram->second.bytes;
    frames->ram.erase(ram);
}

void
GeoSimCache::removeSpilledLocked(Frames* frames)
{
    for (std::map<int, std::string>::const_iterator it = frames->spilled.begin(); it != frames->spilled.end(); ++it) {
        removeFile(it->second);
    }
    frames->spilled.clear();
}

void
GeoSimCache::evictLocked(const GeoSimSeries& kept,
                         int keptFrame)
{
    while (_bytes > _budget) {
        std::map<GeoSimSeries, Frames>::iterator oldestSeries = _series.end();
        std::map<int, Entry>::iterator oldest;
        for (std::map<GeoSimSeries, Frames>::iterator s = _series.begin(); s != _series.end(); ++s) {
            for (std::map<int, Entry>::iterator e = s->second.ram.begin(); e != s->second.ram.end(); ++e) {
                if ((s->first == kept) && (e->first == keptFrame)) {
                    continue;
                }
                if ((oldestSeries == _series.end()) || (e->second.lastUse < oldest->second.lastUse)) {
                    oldestSeries = s;
                    oldest = e;
                }
            }
        }
        if (oldestSeries == _series.end()) {
            return;
        }
        Frames& frames = oldestSeries->second;
        if (!_spillDirectory.empty() && (frames.spilled.find(oldest->first) == frames.spilled.end())) {
            const std::string path = spillPathLocked(oldestSeries->first, oldest->first);
            try {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(_spillDirectory), ec);
                geoFileWrite(*oldest->second.detail, path);
                frames.spilled[oldest->first] = path;
            } catch (const GeoFileError&) {
                // A frame that cannot be spilled (disk full, say) is solved again when asked for.
            }
        }
        forgetLocked(&frames, oldest);
        if (frames.ram.empty() && frames.spilled.empty()) {
            _series.erase(oldestSeries);
        }
    }
}

GeoDetailConstPtr
GeoSimCache::find(const GeoSimSeries& series,
                  int frame)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<GeoSimSeries, Frames>::iterator s = _series.find(series);

    if (s == _series.end()) {
        return GeoDetailConstPtr();
    }
    std::map<int, Entry>::iterator ram = s->second.ram.find(frame);
    if (ram != s->second.ram.end()) {
        ram->second.lastUse = ++_clock;

        return ram->second.detail;
    }
    std::map<int, std::string>::iterator spilled = s->second.spilled.find(frame);
    if (spilled == s->second.spilled.end()) {
        return GeoDetailConstPtr();
    }
    GeoDetailConstPtr detail;
    try {
        detail = std::make_shared<const GeoDetail>(geoFileRead(spilled->second));
    } catch (const GeoFileError&) {
        removeFile(spilled->second);
        s->second.spilled.erase(spilled);
        if (s->second.ram.empty() && s->second.spilled.empty()) {
            _series.erase(s);
        }

        return GeoDetailConstPtr();
    }
    insertLocked(series, frame, detail);
    evictLocked(series, frame);

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
    std::map<int, Entry>::const_iterator ram = s->second.ram.upper_bound(last);
    if (ram != s->second.ram.begin()) {
        --ram;
        if (ram->first >= first) {
            *frame = ram->first;
            found = true;
        }
    }
    std::map<int, std::string>::const_iterator spilled = s->second.spilled.upper_bound(last);
    if (spilled != s->second.spilled.begin()) {
        --spilled;
        if ((spilled->first >= first) && (!found || (spilled->first > *frame))) {
            *frame = spilled->first;
            found = true;
        }
    }

    return found;
}

void
GeoSimCache::retainOnly(const GeoSimSeries& series,
                        int frame)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<GeoSimSeries, Frames>::iterator s = _series.find(series);

    if (s == _series.end()) {
        return;
    }
    Frames& frames = s->second;
    for (std::map<int, Entry>::iterator it = frames.ram.begin(); it != frames.ram.end();) {
        if (it->first == frame) {
            ++it;
        } else {
            std::map<int, Entry>::iterator next = std::next(it);
            forgetLocked(&frames, it);
            it = next;
        }
    }
    for (std::map<int, std::string>::iterator it = frames.spilled.begin(); it != frames.spilled.end();) {
        if (it->first == frame) {
            ++it;
        } else {
            removeFile(it->second);
            it = frames.spilled.erase(it);
        }
    }
    if (frames.ram.empty() && frames.spilled.empty()) {
        _series.erase(s);
    }
}

void
GeoSimCache::retainHash(U64 hash)
{
    std::lock_guard<std::mutex> lock(_mutex);

    for (std::map<GeoSimSeries, Frames>::iterator it = _series.begin(); it != _series.end();) {
        if (it->first.hash == hash) {
            ++it;
            continue;
        }
        removeSpilledLocked(&it->second);
        for (std::map<int, Entry>::const_iterator e = it->second.ram.begin(); e != it->second.ram.end(); ++e) {
            _bytes -= e->second.bytes;
        }
        it = _series.erase(it);
    }
}

void
GeoSimCache::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);

    for (std::map<GeoSimSeries, Frames>::iterator it = _series.begin(); it != _series.end(); ++it) {
        removeSpilledLocked(&it->second);
    }
    _series.clear();
    _bytes = 0;
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
    std::size_t count = 0;

    for (std::map<GeoSimSeries, Frames>::const_iterator it = _series.begin(); it != _series.end(); ++it) {
        count += it->second.ram.size();
    }

    return count;
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
