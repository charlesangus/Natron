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
#include <atomic>
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

// Slots of kPageBytes in an unlinked temporary file. Pages keep their file alive, so the
// pages a cleared TexelPages handed out stay readable.
struct SpillFile {
    std::string directory;
    std::mutex mutex;
    int fd = -1;
    std::int64_t nextSlot = 0;
    std::vector<std::int64_t> freeSlots;

    explicit SpillFile(const std::string& directory_)
        : directory(directory_)
    {
    }

    ~SpillFile()
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    SpillFile(const SpillFile&) = delete;
    SpillFile& operator=(const SpillFile&) = delete;

    bool take(std::int64_t* slot,
              std::string* error)
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (fd < 0) {
            std::string dir = directory;
            if (dir.empty()) {
                std::error_code ec;
                dir = std::filesystem::temp_directory_path(ec).string();
                if (ec || dir.empty()) {
                    dir = "/tmp";
                }
            }
            std::string pattern = dir + "/NatronTexelPages-XXXXXX";
            std::vector<char> name(pattern.begin(), pattern.end());
            name.push_back('\0');
            const int opened = ::mkstemp(name.data());
            if (opened < 0) {
                return fail(error, "Paint could not be spilled to " + dir + ": " + std::strerror(errno));
            }
            ::unlink(name.data());
            fd = opened;
        }
        if (!freeSlots.empty()) {
            *slot = freeSlots.back();
            freeSlots.pop_back();
        } else {
            *slot = nextSlot++;
        }

        return true;
    }

    void give(std::int64_t slot)
    {
        std::lock_guard<std::mutex> lock(mutex);

        freeSlots.push_back(slot);
    }

    // The descriptor is set once, before the first slot is handed out, and pread/pwrite need no lock.
    bool write(std::int64_t slot,
               const std::vector<GfHalf>& texels,
               std::string* error) const
    {
        const char* data = reinterpret_cast<const char*>(texels.data());
        std::size_t done = 0;
        const off_t offset = (off_t)slot * (off_t)TexelPages::kPageBytes;
        while (done < TexelPages::kPageBytes) {
            const ssize_t n = ::pwrite(fd, data + done, TexelPages::kPageBytes - done, offset + (off_t)done);
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

    bool read(std::int64_t slot,
              std::vector<GfHalf>* texels,
              std::string* error) const
    {
        texels->resize(kHalvesPerPage);
        char* data = reinterpret_cast<char*>(texels->data());
        std::size_t done = 0;
        const off_t offset = (off_t)slot * (off_t)TexelPages::kPageBytes;
        while (done < TexelPages::kPageBytes) {
            const ssize_t n = ::pread(fd, data + done, TexelPages::kPageBytes - done, offset + (off_t)done);
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
};
} // anonymous namespace

/*
 * Lock order: a TexelPages' mutex, then a page's, then the budget's. A page's destructor takes
 * the budget's mutex, so no strong reference to a page is dropped while it is held.
 */
class TexelPages::Budget {
public:
    explicit Budget(std::size_t bytes)
        : limit(std::max<std::size_t>(1, bytes / kPageBytes))
    {
    }

    std::mutex mutex;
    // Resident pages, most recently used first.
    std::list<std::weak_ptr<const Page>> lru;
    const std::size_t limit;

    // With the page's mutex held.
    void touch(const PagePtr& page);

    // Spills or drops the pages used least recently until the resident ones fit.
    bool balance(std::string* error);
};

struct TexelPages::Page {
    BudgetPtr budget;
    std::shared_ptr<SpillFile> spill;
    // Only a cache of the base, dropped rather than spilled.
    bool droppable = false;

    mutable std::mutex mutex;
    // Under mutex: the halves when resident, and the slot holding them once spilled. A page never
    // changes, so its slot stays good when it comes back into memory.
    mutable std::shared_ptr<const std::vector<GfHalf>> texels;
    mutable std::int64_t slot = -1;

    // Under the budget's mutex.
    mutable bool listed = false;
    mutable std::list<std::weak_ptr<const Page>>::iterator lru;

    ~Page()
    {
        {
            std::lock_guard<std::mutex> lock(budget->mutex);
            if (listed) {
                budget->lru.erase(lru);
            }
        }
        if (slot >= 0) {
            spill->give(slot);
        }
    }

    /**
     * @brief The page's texels; a spilled page comes back into memory when bringBack is set. Sets
     * *gone instead when the page was a cache of the base that has been dropped.
     **/
    bool load(const PagePtr& self,
              bool bringBack,
              PageData* data,
              bool* gone,
              std::string* error) const
    {
        *gone = false;
        std::lock_guard<std::mutex> lock(mutex);
        if (texels) {
            fromHalves(texels->data(), data);
            if (bringBack) {
                budget->touch(self);
            }

            return true;
        }
        if (slot < 0) {
            *gone = true;

            return true;
        }
        std::shared_ptr<std::vector<GfHalf>> halves = std::make_shared<std::vector<GfHalf>>();
        if (!spill->read(slot, halves.get(), error)) {
            return false;
        }
        fromHalves(halves->data(), data);
        if (bringBack) {
            texels = std::move(halves);
            budget->touch(self);
        }

        return true;
    }

    bool resident() const
    {
        std::lock_guard<std::mutex> lock(mutex);

        return bool(texels);
    }

    bool dropped() const
    {
        std::lock_guard<std::mutex> lock(mutex);

        return !texels && (slot < 0);
    }

    // Leaves memory unless it was used again since the budget chose it.
    bool evict(const PagePtr& self,
               std::string* error) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        {
            std::lock_guard<std::mutex> listLock(budget->mutex);
            if (listed) {
                return true;
            }
        }
        if (!texels) {
            return true;
        }
        if (!droppable && (slot < 0)) {
            std::int64_t taken = -1;
            if (!spill->take(&taken, error) || !spill->write(taken, *texels, error)) {
                if (taken >= 0) {
                    spill->give(taken);
                }
                budget->touch(self);

                return false;
            }
            slot = taken;
        }
        texels.reset();

        return true;
    }
};

void
TexelPages::Budget::touch(const PagePtr& page)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (page->listed) {
        lru.splice(lru.begin(), lru, page->lru);
    } else {
        lru.push_front(page);
        page->lru = lru.begin();
        page->listed = true;
    }
}

bool
TexelPages::Budget::balance(std::string* error)
{
    for (;;) {
        std::vector<PagePtr> victims;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (lru.size() <= limit) {
                return true;
            }
            std::size_t excess = lru.size() - limit;
            std::list<std::weak_ptr<const Page>>::iterator it = lru.end();
            while ((excess > 0) && (it != lru.begin())) {
                --it;
                PagePtr page = it->lock();
                if (!page) {
                    // Dying: its destructor takes it off the list.
                    continue;
                }
                page->listed = false;
                it = lru.erase(it);
                --excess;
                victims.push_back(std::move(page));
            }
        }
        if (victims.empty()) {
            return true;
        }
        // Every victim is either evicted or listed again, even past a failure.
        bool evicted = true;
        for (const PagePtr& victim : victims) {
            std::string evictError;
            if (!victim->evict(victim, &evictError) && evicted) {
                evicted = false;
                if (error) {
                    *error = evictError;
                }
            }
        }
        if (!evicted) {
            return false;
        }
    }
}

namespace {
TexelPages::PagePtr
makePage(const TexelPages::BudgetPtr& budget,
         const std::shared_ptr<SpillFile>& spill,
         bool droppable,
         std::vector<GfHalf>&& halves)
{
    std::shared_ptr<TexelPages::Page> page = std::make_shared<TexelPages::Page>();

    page->budget = budget;
    page->spill = spill;
    page->droppable = droppable;
    page->texels = std::make_shared<const std::vector<GfHalf>>(std::move(halves));
    TexelPages::PagePtr made = page;
    std::lock_guard<std::mutex> lock(made->mutex);
    budget->touch(made);

    return made;
}
} // anonymous namespace

struct TexelPages::Implementation {
    struct Entry {
        PagePtr page;
        bool painted = false;
    };

    int size = 0;
    int pagesPerSide = 0;
    BaseFill base;
    BudgetPtr budget;
    std::string spillDirectory;

    mutable std::mutex mutex;
    std::shared_ptr<SpillFile> spill;
    std::map<PageKey, Entry> entries;
    // The painted pages again, by row, shared with what sharePainted() handed out until a write
    // copies the index and the row it changes.
    std::shared_ptr<const PaintedRows> painted = std::make_shared<PaintedRows>();

    // With mutex held; a null page unpaints the key.
    void setPainted(const PageKey& key,
                    const PagePtr& page)
    {
        PageKey rowKey = key;
        rowKey.px = 0;
        // A count of one means nothing else holds it, and nothing can take it without the mutex.
        if (painted.use_count() > 1) {
            painted = std::make_shared<PaintedRows>(*painted);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        // Made here as non-const objects, and held by nothing else when changed.
        PaintedRows& rows = const_cast<PaintedRows&>(*painted);
        PaintedRows::iterator it = rows.find(rowKey);
        if (it == rows.end()) {
            if (!page) {
                return;
            }
            it = rows.insert(std::make_pair(rowKey, std::make_shared<PageRow>())).first;
        } else if (it->second.use_count() > 1) {
            it->second = std::make_shared<PageRow>(*it->second);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        PageRow& row = const_cast<PageRow&>(*it->second);
        if (page) {
            row[key] = page;
        } else {
            row.erase(key);
            if (row.empty()) {
                rows.erase(it);
            }
        }
    }

    // With mutex held.
    void erase(std::map<PageKey, Entry>::iterator it)
    {
        if (it->second.painted) {
            setPainted(it->first, PagePtr());
        }
        entries.erase(it);
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
    _imp->budget = (budgetBytes == 0) ? sharedBudget() : std::make_shared<Budget>(budgetBytes);
    _imp->spillDirectory = spillDirectory;
    _imp->spill = std::make_shared<SpillFile>(spillDirectory);
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

TexelPages::BudgetPtr
TexelPages::sharedBudget()
{
    static const BudgetPtr budget = std::make_shared<Budget>(defaultBudget());

    return budget;
}

bool
TexelPages::readPage(const PagePtr& page,
                     PageData* data,
                     std::string* error)
{
    if (!page) {
        return fail(error, "A paint page is missing.");
    }
    bool gone = false;
    if (!page->load(page, false, data, &gone, error)) {
        return false;
    }
    if (gone) {
        return fail(error, "A paint page is missing.");
    }

    return true;
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
    PagePtr found;
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        const std::map<PageKey, Implementation::Entry>::const_iterator it = _imp->entries.find(key);
        if (it != _imp->entries.end()) {
            found = it->second.page;
        }
    }
    if (found) {
        bool gone = false;
        if (!found->load(found, true, page, &gone, error)) {
            return false;
        }
        if (!gone) {
            return _imp->budget->balance(error);
        }
        std::lock_guard<std::mutex> lock(_imp->mutex);
        const std::map<PageKey, Implementation::Entry>::iterator it = _imp->entries.find(key);
        if ((it != _imp->entries.end()) && (it->second.page == found)) {
            _imp->entries.erase(it);
        }
    }

    // The base is read unlocked; a thread that loses the race to insert it reads the winner's.
    PageData filled;
    if (!_imp->fillBase(key, &filled, error)) {
        return false;
    }
    std::vector<GfHalf> halves;
    toHalves(filled, &halves);
    // Every later read comes from the cached halves, so the first must be rounded the same way
    // or a texel's base depends on how often its page was read.
    fromHalves(halves.data(), &filled);
    PagePtr winner;
    {
        PagePtr made = makePage(_imp->budget, _imp->spill, true, std::move(halves));
        std::lock_guard<std::mutex> lock(_imp->mutex);
        const std::map<PageKey, Implementation::Entry>::const_iterator it = _imp->entries.find(key);
        if (it == _imp->entries.end()) {
            Implementation::Entry entry;
            entry.page = std::move(made);
            _imp->entries.insert(std::make_pair(key, std::move(entry)));
        } else {
            winner = it->second.page;
        }
    }
    bool gone = true;
    if (winner && !winner->load(winner, true, page, &gone, error)) {
        return false;
    }
    if (gone) {
        *page = std::move(filled);
    }

    return _imp->budget->balance(error);
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
    std::vector<GfHalf> halves;
    toHalves(page, &halves);
    PagePtr made = makePage(_imp->budget, _imp->spill, false, std::move(halves));
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        Implementation::Entry& entry = _imp->entries[key];
        entry.page = made;
        entry.painted = true;
        _imp->setPainted(key, made);
    }

    return _imp->budget->balance(error);
}

bool
TexelPages::snapshot(const std::vector<PageKey>& keys,
                     std::vector<Snapshot>* snapshots,
                     std::string* /*error*/)
{
    snapshots->clear();
    snapshots->reserve(keys.size());

    std::lock_guard<std::mutex> lock(_imp->mutex);
    for (const PageKey& key : keys) {
        Snapshot snap;
        snap.key = key;
        const std::map<PageKey, Implementation::Entry>::const_iterator it = _imp->entries.find(key);
        if ((it != _imp->entries.end()) && it->second.painted) {
            snap.page = it->second.page;
        }
        snapshots->push_back(std::move(snap));
    }

    return true;
}

bool
TexelPages::restore(const std::vector<Snapshot>& snapshots,
                    std::string* error)
{
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        for (const Snapshot& snap : snapshots) {
            if (!snap.page) {
                const std::map<PageKey, Implementation::Entry>::iterator it = _imp->entries.find(snap.key);
                if (it != _imp->entries.end()) {
                    _imp->erase(it);
                }
                continue;
            }
            if (snap.page->budget != _imp->budget) {
                return fail(error, "A paint page snapshot belongs to other pages.");
            }
            Implementation::Entry& entry = _imp->entries[snap.key];
            entry.page = snap.page;
            entry.painted = true;
            _imp->setPainted(snap.key, snap.page);
        }
    }

    return _imp->budget->balance(error);
}

bool
TexelPages::exportTile(int tile,
                       int plane,
                       float* rgba,
                       float* touched,
                       std::string* error) const
{
    const std::size_t size = (std::size_t)_imp->size;
    std::vector<std::pair<PageKey, PagePtr>> pages;
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        PageKey first;
        first.tile = tile;
        first.plane = plane;
        first.px = std::numeric_limits<std::int32_t>::min();
        first.py = std::numeric_limits<std::int32_t>::min();
        for (std::map<PageKey, Implementation::Entry>::const_iterator it = _imp->entries.lower_bound(first);
             (it != _imp->entries.end()) && (it->first.tile == tile) && (it->first.plane == plane); ++it) {
            if (it->second.painted) {
                pages.push_back(std::make_pair(it->first, it->second.page));
            }
        }
    }

    PageData page;
    for (const std::pair<PageKey, PagePtr>& entry : pages) {
        if (!readPage(entry.second, &page, error)) {
            return false;
        }
        const RectI rect = _imp->texelsOf(entry.first);
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

void
TexelPages::pack(const PageData& page,
                 std::vector<GfHalf>* halves)
{
    toHalves(page, halves);
}

void
TexelPages::unpack(const GfHalf* halves,
                   PageData* page)
{
    fromHalves(halves, page);
}

TexelPages::Painted
TexelPages::sharePainted() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return _imp->painted;
}

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
    std::size_t count = 0;
    std::lock_guard<std::mutex> lock(_imp->mutex);

    for (const std::pair<const PageKey, Implementation::Entry>& entry : _imp->entries) {
        if (entry.second.painted || !entry.second.page->dropped()) {
            ++count;
        }
    }

    return count;
}

std::size_t
TexelPages::residentPageCount() const
{
    std::size_t count = 0;
    std::lock_guard<std::mutex> lock(_imp->mutex);

    for (const std::pair<const PageKey, Implementation::Entry>& entry : _imp->entries) {
        if (entry.second.page->resident()) {
            ++count;
        }
    }

    return count;
}

std::size_t
TexelPages::spilledPageCount() const
{
    std::size_t count = 0;
    std::lock_guard<std::mutex> lock(_imp->mutex);

    for (const std::pair<const PageKey, Implementation::Entry>& entry : _imp->entries) {
        if (entry.second.painted && !entry.second.page->resident()) {
            ++count;
        }
    }

    return count;
}

void
TexelPages::clear()
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    _imp->entries.clear();
    _imp->painted = std::make_shared<PaintedRows>();
    _imp->spill = std::make_shared<SpillFile>(_imp->spillDirectory);
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
