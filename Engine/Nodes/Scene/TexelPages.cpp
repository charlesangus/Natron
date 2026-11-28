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

#include "TexelPages.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <system_error>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <OpenImageIO/imageio.h>

#include "Engine/MemoryInfo.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
const std::size_t kHalvesPerPage = (std::size_t)TexelPages::kPageTexels * 5;

bool
fail(std::string* error,
     const std::string& message)
{
    if (error) {
        *error = message;
    }

    return false;
}

void
toHalves(const TexelPages::PageData& page,
         std::vector<GfHalf>* halves)
{
    halves->resize(kHalvesPerPage);
    const std::size_t rgbaCount = (std::size_t)TexelPages::kPageTexels * 4;
    for (std::size_t i = 0; i < rgbaCount; ++i) {
        (*halves)[i] = GfHalf(page.rgba[i]);
    }
    for (std::size_t i = 0; i < (std::size_t)TexelPages::kPageTexels; ++i) {
        (*halves)[rgbaCount + i] = GfHalf(page.touched[i]);
    }
}

void
fromHalves(const GfHalf* halves,
           TexelPages::PageData* page)
{
    const std::size_t rgbaCount = (std::size_t)TexelPages::kPageTexels * 4;

    page->rgba.resize(rgbaCount);
    page->touched.resize((std::size_t)TexelPages::kPageTexels);
    for (std::size_t i = 0; i < rgbaCount; ++i) {
        page->rgba[i] = (float)halves[i];
    }
    for (std::size_t i = 0; i < (std::size_t)TexelPages::kPageTexels; ++i) {
        page->touched[i] = (float)halves[rgbaCount + i];
    }
}

// The channels of a decoded file as RGBA, as MaterialBake reads a base file.
void
expandChannels(const float* raw,
               int channels,
               std::size_t count,
               float* rgba)
{
    const bool grey = channels <= 2;

    for (std::size_t i = 0; i < count; ++i) {
        const float* in = raw + i * (std::size_t)channels;
        float* out = rgba + i * 4;
        out[0] = in[0];
        out[1] = grey ? in[0] : in[1];
        out[2] = grey ? in[0] : in[2];
        out[3] = (channels == 2) ? in[1] : ((channels == 4) ? in[3] : 1.f);
    }
}
} // anonymous namespace

struct TexelPages::Implementation {
    struct Entry {
        std::unique_ptr<std::vector<GfHalf>> texels;
        bool painted = false;
        // The slot of the spill file holding the page, and whether it holds the page as it is now.
        std::int64_t slot = -1;
        bool slotCurrent = false;
        std::list<PageKey>::iterator lru;
    };

    int size = 0;
    int pagesPerSide = 0;
    BaseFill base;
    std::size_t budgetPages = 1;
    std::string spillDirectory;

    mutable std::mutex mutex;
    std::map<PageKey, Entry> entries;
    // Resident pages, most recently used first.
    std::list<PageKey> lru;
    std::size_t spilled = 0;
    int spillFd = -1;
    std::int64_t nextSlot = 0;
    std::vector<std::int64_t> freeSlots;

    ~Implementation()
    {
        if (spillFd >= 0) {
            ::close(spillFd);
        }
    }

    bool openSpill(std::string* error)
    {
        if (spillFd >= 0) {
            return true;
        }
        std::string directory = spillDirectory;
        if (directory.empty()) {
            std::error_code ec;
            directory = std::filesystem::temp_directory_path(ec).string();
            if (ec || directory.empty()) {
                directory = "/tmp";
            }
        }
        std::string pattern = directory + "/NatronTexelPages-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        const int fd = ::mkstemp(name.data());
        if (fd < 0) {
            return fail(error, "Paint could not be spilled to " + directory + ": " + std::strerror(errno));
        }
        ::unlink(name.data());
        spillFd = fd;

        return true;
    }

    bool writeSlot(std::int64_t slot,
                   const std::vector<GfHalf>& texels,
                   std::string* error) const
    {
        const char* data = reinterpret_cast<const char*>(texels.data());
        std::size_t done = 0;
        const off_t offset = (off_t)slot * (off_t)kPageBytes;
        while (done < kPageBytes) {
            const ssize_t n = ::pwrite(spillFd, data + done, kPageBytes - done, offset + (off_t)done);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }

                return fail(error, std::string("Paint could not be spilled to disk: ") + std::strerror(errno));
            }
            done += (std::size_t)n;
        }

        return true;
    }

    bool readSlot(std::int64_t slot,
                  std::vector<GfHalf>* texels,
                  std::string* error) const
    {
        texels->resize(kHalvesPerPage);
        char* data = reinterpret_cast<char*>(texels->data());
        std::size_t done = 0;
        const off_t offset = (off_t)slot * (off_t)kPageBytes;
        while (done < kPageBytes) {
            const ssize_t n = ::pread(spillFd, data + done, kPageBytes - done, offset + (off_t)done);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }

                return fail(error, std::string("Spilled paint could not be read back: ") + std::strerror(errno));
            }
            if (n == 0) {
                return fail(error, "Spilled paint could not be read back: the spill file is short.");
            }
            done += (std::size_t)n;
        }

        return true;
    }

    void freeSlot(Entry* entry)
    {
        if (entry->slot >= 0) {
            freeSlots.push_back(entry->slot);
            entry->slot = -1;
            entry->slotCurrent = false;
        }
    }

    void touch(const PageKey& key,
               Entry* entry)
    {
        if (entry->texels) {
            lru.erase(entry->lru);
        }
        lru.push_front(key);
        entry->lru = lru.begin();
    }

    void erase(std::map<PageKey, Entry>::iterator it)
    {
        Entry& entry = it->second;
        if (entry.texels) {
            lru.erase(entry.lru);
        } else {
            --spilled;
        }
        freeSlot(&entry);
        entries.erase(it);
    }

    bool unspill(const PageKey& key,
                 Entry* entry,
                 std::string* error)
    {
        std::unique_ptr<std::vector<GfHalf>> texels(new std::vector<GfHalf>());
        if (!readSlot(entry->slot, texels.get(), error)) {
            return false;
        }
        touch(key, entry);
        entry->texels = std::move(texels);
        --spilled;

        return true;
    }

    // Drops or spills the least recently used pages other than keep until the budget holds.
    bool evict(const PageKey& keep,
               std::string* error)
    {
        while (lru.size() > budgetPages) {
            std::list<PageKey>::iterator last = std::prev(lru.end());
            if (*last == keep) {
                if (lru.size() == 1) {
                    break;
                }
                last = std::prev(last);
            }
            const PageKey key = *last;
            std::map<PageKey, Entry>::iterator it = entries.find(key);
            Entry& entry = it->second;
            if (!entry.painted) {
                erase(it);
                continue;
            }
            if (!entry.slotCurrent) {
                if (!openSpill(error)) {
                    return false;
                }
                if (entry.slot < 0) {
                    if (!freeSlots.empty()) {
                        entry.slot = freeSlots.back();
                        freeSlots.pop_back();
                    } else {
                        entry.slot = nextSlot++;
                    }
                }
                if (!writeSlot(entry.slot, *entry.texels, error)) {
                    return false;
                }
                entry.slotCurrent = true;
            }
            lru.erase(entry.lru);
            entry.texels.reset();
            ++spilled;
        }

        return true;
    }

    bool fillBase(const PageKey& key,
                  PageData* page,
                  std::string* error) const
    {
        std::fill(page->rgba.begin(), page->rgba.end(), 0.f);
        std::fill(page->touched.begin(), page->touched.end(), 0.f);
        if (!base) {
            return true;
        }
        const RectI rect = texelsOf(key);
        if (rect.isNull()) {
            return true;
        }
        std::vector<float> region((std::size_t)rect.width() * (std::size_t)rect.height() * 4, 0.f);
        if (!base(key.tile, key.plane, rect, region.data(), error)) {
            return false;
        }
        for (int y = 0; y < rect.height(); ++y) {
            std::copy_n(region.data() + (std::size_t)y * (std::size_t)rect.width() * 4,
                        (std::size_t)rect.width() * 4,
                        page->rgba.data() + (std::size_t)y * (std::size_t)kPageSize * 4);
        }

        return true;
    }

    RectI texelsOf(const PageKey& key) const
    {
        const int x1 = key.px * kPageSize;
        const int y1 = key.py * kPageSize;
        if ((key.px < 0) || (key.py < 0) || (x1 >= size) || (y1 >= size)) {
            return RectI();
        }

        return RectI(x1, y1, std::min(size, x1 + kPageSize), std::min(size, y1 + kPageSize));
    }

    // The page's halves, read back from the spill file when it is not in memory, without bringing it back.
    bool peek(const Entry& entry,
              std::vector<GfHalf>* scratch,
              const std::vector<GfHalf>** texels,
              std::string* error) const
    {
        if (entry.texels) {
            *texels = entry.texels.get();

            return true;
        }
        if (!readSlot(entry.slot, scratch, error)) {
            return false;
        }
        *texels = scratch;

        return true;
    }
};

TexelPages::TexelPages(int size,
                       BaseFill base,
                       std::size_t budgetBytes,
                       const std::string& spillDirectory)
    : _imp(new Implementation())
{
    _imp->size = std::max(0, size);
    _imp->pagesPerSide = (_imp->size + kPageSize - 1) / kPageSize;
    _imp->base = std::move(base);
    if (budgetBytes == 0) {
        budgetBytes = defaultBudget();
    }
    _imp->budgetPages = std::max<std::size_t>(1, budgetBytes / kPageBytes);
    _imp->spillDirectory = spillDirectory;
}

TexelPages::~TexelPages()
{
}

std::size_t
TexelPages::defaultBudget()
{
    const U64 ram = getSystemTotalRAM();

    return std::max<std::size_t>(kPageBytes * 64, (std::size_t)(ram / 4));
}

int
TexelPages::tileSize() const
{
    return _imp->size;
}

int
TexelPages::pagesPerSide() const
{
    return _imp->pagesPerSide;
}

TexelPages::PageKey
TexelPages::pageOf(int tile,
                   int plane,
                   int x,
                   int y)
{
    PageKey key;

    key.tile = tile;
    key.plane = plane;
    key.px = (x >= 0) ? (x / kPageSize) : -1;
    key.py = (y >= 0) ? (y / kPageSize) : -1;

    return key;
}

RectI
TexelPages::texelsOf(const PageKey& key) const
{
    return _imp->texelsOf(key);
}

bool
TexelPages::read(const PageKey& key,
                 PageData* page,
                 std::string* error)
{
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        std::map<PageKey, Implementation::Entry>::iterator it = _imp->entries.find(key);
        if (it != _imp->entries.end()) {
            Implementation::Entry& entry = it->second;
            if (entry.texels) {
                _imp->touch(key, &entry);
            } else if (!_imp->unspill(key, &entry, error)) {
                return false;
            }
            fromHalves(entry.texels->data(), page);

            return _imp->evict(key, error);
        }
    }

    // The base is read unlocked; a thread that loses the race to insert it reads the winner's.
    PageData filled;
    if (!_imp->fillBase(key, &filled, error)) {
        return false;
    }
    std::unique_ptr<std::vector<GfHalf>> texels(new std::vector<GfHalf>());
    toHalves(filled, texels.get());

    std::lock_guard<std::mutex> lock(_imp->mutex);
    std::map<PageKey, Implementation::Entry>::iterator it = _imp->entries.find(key);
    if (it == _imp->entries.end()) {
        it = _imp->entries.insert(std::make_pair(key, Implementation::Entry())).first;
        Implementation::Entry& entry = it->second;
        _imp->touch(key, &entry);
        entry.texels = std::move(texels);
    } else if (it->second.texels) {
        _imp->touch(key, &it->second);
    } else if (!_imp->unspill(key, &it->second, error)) {
        return false;
    }
    fromHalves(it->second.texels->data(), page);

    return _imp->evict(key, error);
} // TexelPages::read

bool
TexelPages::write(const PageKey& key,
                  const PageData& page,
                  std::string* error)
{
    if ((page.rgba.size() != (std::size_t)kPageTexels * 4) || (page.touched.size() != (std::size_t)kPageTexels)) {
        return fail(error, "A paint page is malformed.");
    }
    if (_imp->texelsOf(key).isNull()) {
        return fail(error, "A paint page lies outside its tile.");
    }
    std::unique_ptr<std::vector<GfHalf>> texels(new std::vector<GfHalf>());
    toHalves(page, texels.get());

    std::lock_guard<std::mutex> lock(_imp->mutex);
    Implementation::Entry& entry = _imp->entries[key];
    if (!entry.texels && (entry.slot >= 0)) {
        --_imp->spilled;
    }
    _imp->touch(key, &entry);
    entry.texels = std::move(texels);
    entry.painted = true;
    entry.slotCurrent = false;

    return _imp->evict(key, error);
}

bool
TexelPages::snapshot(const std::vector<PageKey>& keys,
                     std::vector<Snapshot>* snapshots,
                     std::string* error)
{
    snapshots->clear();
    snapshots->reserve(keys.size());

    std::lock_guard<std::mutex> lock(_imp->mutex);
    std::vector<GfHalf> scratch;
    for (const PageKey& key : keys) {
        Snapshot snap;
        snap.key = key;
        std::map<PageKey, Implementation::Entry>::const_iterator it = _imp->entries.find(key);
        if ((it != _imp->entries.end()) && it->second.painted) {
            const std::vector<GfHalf>* texels = nullptr;
            if (!_imp->peek(it->second, &scratch, &texels, error)) {
                return false;
            }
            snap.painted = true;
            snap.texels = *texels;
        }
        snapshots->push_back(std::move(snap));
    }

    return true;
}

bool
TexelPages::restore(const std::vector<Snapshot>& snapshots,
                    std::string* error)
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    for (const Snapshot& snap : snapshots) {
        std::map<PageKey, Implementation::Entry>::iterator it = _imp->entries.find(snap.key);
        if (!snap.painted) {
            if (it != _imp->entries.end()) {
                _imp->erase(it);
            }
            continue;
        }
        if (snap.texels.size() != kHalvesPerPage) {
            return fail(error, "A paint page snapshot is malformed.");
        }
        Implementation::Entry& entry = _imp->entries[snap.key];
        if (!entry.texels && (entry.slot >= 0)) {
            --_imp->spilled;
        }
        _imp->touch(snap.key, &entry);
        entry.texels.reset(new std::vector<GfHalf>(snap.texels));
        entry.painted = true;
        entry.slotCurrent = false;
        if (!_imp->evict(snap.key, error)) {
            return false;
        }
    }

    return true;
}

bool
TexelPages::exportTile(int tile,
                       int plane,
                       float* rgba,
                       float* touched,
                       std::string* error) const
{
    const std::size_t size = (std::size_t)_imp->size;
    std::vector<GfHalf> scratch;
    PageData page;

    std::lock_guard<std::mutex> lock(_imp->mutex);
    PageKey first;
    first.tile = tile;
    first.plane = plane;
    first.px = std::numeric_limits<std::int32_t>::min();
    first.py = std::numeric_limits<std::int32_t>::min();
    for (std::map<PageKey, Implementation::Entry>::const_iterator it = _imp->entries.lower_bound(first);
         (it != _imp->entries.end()) && (it->first.tile == tile) && (it->first.plane == plane); ++it) {
        if (!it->second.painted) {
            continue;
        }
        const std::vector<GfHalf>* texels = nullptr;
        if (!_imp->peek(it->second, &scratch, &texels, error)) {
            return false;
        }
        fromHalves(texels->data(), &page);
        const RectI rect = _imp->texelsOf(it->first);
        for (int y = rect.y1; y < rect.y2; ++y) {
            const std::size_t from = (std::size_t)(y - rect.y1) * (std::size_t)kPageSize;
            const std::size_t to = (std::size_t)rect.x1 + (std::size_t)y * size;
            const std::size_t count = (std::size_t)rect.width();
            std::copy_n(page.rgba.data() + from * 4, count * 4, rgba + to * 4);
            if (touched) {
                std::copy_n(page.touched.data() + from, count, touched + to);
            }
        }
    }

    return true;
} // TexelPages::exportTile

std::vector<TexelPages::PageKey>
TexelPages::paintedPages() const
{
    std::vector<PageKey> keys;
    std::lock_guard<std::mutex> lock(_imp->mutex);

    for (const std::pair<const PageKey, Implementation::Entry>& entry : _imp->entries) {
        if (entry.second.painted) {
            keys.push_back(entry.first);
        }
    }

    return keys;
}

std::size_t
TexelPages::pageCount() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return _imp->entries.size();
}

std::size_t
TexelPages::residentPageCount() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return _imp->lru.size();
}

std::size_t
TexelPages::spilledPageCount() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return _imp->spilled;
}

void
TexelPages::clear()
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    _imp->entries.clear();
    _imp->lru.clear();
    _imp->spilled = 0;
    _imp->freeSlots.clear();
    _imp->nextSlot = 0;
    if (_imp->spillFd >= 0) {
        ::close(_imp->spillFd);
        _imp->spillFd = -1;
    }
}

bool
TexelPages::readFileRegion(const std::string& path,
                           int size,
                           const RectI& texels,
                           float* rgba,
                           std::string* error)
{
    if ((size <= 0) || texels.isNull()) {
        return fail(error, "The tile is empty.");
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return fail(error, "The file " + path + " is missing.");
    }
    std::string readError;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &readError, FilePin::eDisk);
    if (!header || header->subimages.empty()) {
        return fail(error, "The file " + path + " could not be read: " + readError);
    }
    const OIIO::ImageSpec& spec = header->subimages.front();
    const RectI window = OiioReadSupport::dataWindowOf(spec);
    const int channels = std::min(spec.nchannels, 4);
    if (window.isNull() || (channels <= 0)) {
        return fail(error, "The file " + path + " holds no pixels.");
    }
    const int width = window.width();
    const int height = window.height();

    // The file's pixels, relative to its data window, a region of them is decoded into.
    RectI source;
    if ((width == size) && (height == size)) {
        source = texels;
    } else {
        int sx1 = width;
        int sx2 = 0;
        int sy1 = height;
        int sy2 = 0;
        for (int x : { texels.x1, texels.x2 - 1 }) {
            const double fx = std::floor((double)((x + 0.5f) / size) * width - 0.5);
            sx1 = std::min(sx1, std::max(0, std::min(width - 1, (int)fx)));
            sx2 = std::max(sx2, std::max(0, std::min(width - 1, (int)fx + 1)) + 1);
        }
        for (int y : { texels.y1, texels.y2 - 1 }) {
            const double fy = std::floor((double)((y + 0.5f) / size) * height - 0.5);
            sy1 = std::min(sy1, std::max(0, std::min(height - 1, (int)fy)));
            sy2 = std::max(sy2, std::max(0, std::min(height - 1, (int)fy + 1)) + 1);
        }
        source = RectI(sx1, sy1, sx2, sy2);
    }
    if ((source.x1 < 0) || (source.y1 < 0) || (source.x2 > width) || (source.y2 > height)) {
        return fail(error, "The texels lie outside " + path + ".");
    }
    const std::size_t sw = (std::size_t)source.width();
    const std::size_t sh = (std::size_t)source.height();
    std::vector<float> raw(sw * sh * (std::size_t)channels);
    const RectI decodeWindow(window.x1 + source.x1, window.y1 + source.y1, window.x1 + source.x2, window.y1 + source.y2);
    if (!OiioReadSupport::decode(path, 0, 0, channels, decodeWindow, raw.data(), sw * (std::size_t)channels, &readError, FilePin::eDisk)) {
        return fail(error, "The file " + path + " could not be read: " + readError);
    }
    const std::size_t outWidth = (std::size_t)texels.width();
    if ((width == size) && (height == size)) {
        expandChannels(raw.data(), channels, sw * sh, rgba);

        return true;
    }
    std::vector<float> pixels(sw * sh * 4);
    expandChannels(raw.data(), channels, sw * sh, pixels.data());
    for (int y = texels.y1; y < texels.y2; ++y) {
        const double fy = (double)((y + 0.5f) / size) * height - 0.5;
        const double fy0 = std::floor(fy);
        const int y0 = (int)fy0;
        const float ay = (float)(fy - fy0);
        const float wy[2] = { 1.f - ay, ay };
        for (int x = texels.x1; x < texels.x2; ++x) {
            const double fx = (double)((x + 0.5f) / size) * width - 0.5;
            const double fx0 = std::floor(fx);
            const int x0 = (int)fx0;
            const float ax = (float)(fx - fx0);
            const float wx[2] = { 1.f - ax, ax };
            float sum[4] = { 0.f, 0.f, 0.f, 0.f };
            for (int j = 0; j < 2; ++j) {
                if (wy[j] == 0.f) {
                    continue;
                }
                const int row = std::min(std::max(y0 + j, 0), height - 1) - source.y1;
                for (int i = 0; i < 2; ++i) {
                    if (wx[i] == 0.f) {
                        continue;
                    }
                    const int column = std::min(std::max(x0 + i, 0), width - 1) - source.x1;
                    const float* texel = pixels.data() + ((std::size_t)column + (std::size_t)row * sw) * 4;
                    const float weight = wx[i] * wy[j];
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += texel[c] * weight;
                    }
                }
            }
            float* out = rgba + ((std::size_t)(x - texels.x1) + (std::size_t)(y - texels.y1) * outWidth) * 4;
            std::copy(sum, sum + 4, out);
        }
    }

    return true;
} // TexelPages::readFileRegion

NATRON_NAMESPACE_EXIT
