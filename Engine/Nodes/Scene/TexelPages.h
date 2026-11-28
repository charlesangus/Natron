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

#ifndef Engine_Nodes_Scene_TexelPages_h
#define Engine_Nodes_Scene_TexelPages_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The texels paint is laid into directly, held as pages of kPageSize x kPageSize
 * half-float texels: premultiplied RGBA plus how much paint each texel has taken, 0 to 1.
 *
 * A page exists only once it is read or written. A page read before it is written holds the base,
 * which the BaseFill gives; a written page is painted. Past the memory budget the pages used
 * least recently leave memory: a base page is dropped and filled again when read, a painted page
 * is spilled to an unlinked file and read back from it, so no amount of paint is ever refused.
 *
 * Texels are addressed as SceneRaster lays a UDIM tile out: texel (x, y) of a size x size tile
 * has its centre at u = tileU + (x + 0.5) / size, v = tileV + (y + 0.5) / size, rows bottom-up.
 * Pages are copied in and out whole, so any number of threads may read and write at once.
 **/
class TexelPages {
public:
    static constexpr int kPageSize = 64;
    static constexpr int kPageTexels = kPageSize * kPageSize;
    static constexpr std::size_t kPageBytes = (std::size_t)kPageTexels * 5 * sizeof(PXR_NS::GfHalf);
    // How far paint grows past the edge of a UV island into the texels no face covers, so that
    // filtering across the edge reads the paint rather than what lies outside the island.
    static constexpr int kSeamGutter = 2;

    struct PageKey {
        std::int32_t tile = 1001;
        // A plane of the painted texture, numbered as its owner numbers them.
        std::int32_t plane = 0;
        // The page's column and row within its tile.
        std::int32_t px = 0;
        std::int32_t py = 0;

        bool operator==(const PageKey& other) const
        {
            return (tile == other.tile) && (plane == other.plane) && (px == other.px) && (py == other.py);
        }

        bool operator!=(const PageKey& other) const
        {
            return !(*this == other);
        }

        bool operator<(const PageKey& other) const
        {
            if (tile != other.tile) {
                return tile < other.tile;
            }
            if (plane != other.plane) {
                return plane < other.plane;
            }
            if (py != other.py) {
                return py < other.py;
            }

            return px < other.px;
        }
    };

    // A page's texels as floats, element x + y * kPageSize; texels past the tile's edge are unused.
    struct PageData {
        std::vector<float> rgba = std::vector<float>((std::size_t)kPageTexels * 4, 0.f);
        std::vector<float> touched = std::vector<float>((std::size_t)kPageTexels, 0.f);
    };

    /**
     * @brief Writes the base of texels, a rectangle within one tile, into rgba: premultiplied RGBA,
     * row-major and bottom-up with a stride of texels.width(). False with *error set when the base
     * cannot be read.
     **/
    typedef std::function<bool(int tile, int plane, const RectI& texels, float* rgba, std::string* error)> BaseFill;

    // A page as halves: the rgba of each texel, then the touched value of each texel.
    static constexpr std::size_t kPageHalves = (std::size_t)kPageTexels * 5;

    static void pack(const PageData& page, std::vector<PXR_NS::GfHalf>* halves);
    static void unpack(const PXR_NS::GfHalf* halves, PageData* page);

    /**
     * @brief A painted page, never changed once made, so the working pages, published snapshots
     * and undo journals all hold the same one. It lives in memory or in a spill file as the
     * memory budget decides.
     **/
    struct Page;
    typedef std::shared_ptr<const Page> PagePtr;

    // A page's texels. A spilled page is read from its file and stays spilled.
    static bool readPage(const PagePtr& page, PageData* data, std::string* error) WARN_UNUSED_RETURN;

    // The painted pages of one row of pages of a tile's plane, and those rows keyed by their
    // first page (px 0).
    typedef std::map<PageKey, PagePtr> PageRow;
    typedef std::map<PageKey, std::shared_ptr<const PageRow>> PaintedRows;
    typedef std::shared_ptr<const PaintedRows> Painted;

    // A page as it was, to put it back: a null page was not painted and goes back to the base.
    struct Snapshot {
        PageKey key;
        PagePtr page;
    };

    /**
     * @brief Resident pages held to a byte limit across every TexelPages using it. Past the limit,
     * the pages used least recently are spilled, or dropped when they only cache the base.
     **/
    class Budget;
    typedef std::shared_ptr<Budget> BudgetPtr;

    // The process' budget: a quarter of the physical memory.
    static BudgetPtr sharedBudget();

    /**
     * @brief Pages of size x size tiles over base, a null base being transparent black. budgetBytes
     * of 0 shares sharedBudget(), else the pages have a budget of their own; at least one page is
     * always held. Spilled pages go to a file in spillDirectory, the system's temporary directory
     * when empty.
     **/
    TexelPages(int size,
               BaseFill base,
               std::size_t budgetBytes = 0,
               const std::string& spillDirectory = std::string());
    ~TexelPages();

    TexelPages(const TexelPages&) = delete;
    TexelPages& operator=(const TexelPages&) = delete;

    // A quarter of the physical memory.
    static std::size_t defaultBudget();

    int tileSize() const;
    int pagesPerSide() const;

    // The page holding texel (x, y) of a tile.
    static PageKey pageOf(int tile,
                          int plane,
                          int x,
                          int y);

    // The texels of the tile a page covers.
    RectI texelsOf(const PageKey& key) const;

    bool read(const PageKey& key, PageData* page, std::string* error) WARN_UNUSED_RETURN;

    bool write(const PageKey& key, const PageData& page, std::string* error) WARN_UNUSED_RETURN;

    bool snapshot(const std::vector<PageKey>& keys, std::vector<Snapshot>* snapshots, std::string* error) WARN_UNUSED_RETURN;

    bool restore(const std::vector<Snapshot>& snapshots, std::string* error) WARN_UNUSED_RETURN;

    /**
     * @brief Writes the painted pages of a tile's plane over rgba, size * size premultiplied RGBA
     * texels, and their touched values over touched when it is not null (size * size floats);
     * texels of pages never painted are left as they are.
     **/
    bool exportTile(int tile, int plane, float* rgba, float* touched, std::string* error) const WARN_UNUSED_RETURN;

    std::vector<PageKey> paintedPages() const;

    /**
     * @brief Every painted page, without copying any: the rows are shared until a write changes
     * them, so this costs nothing and what it hands out stays as it was.
     **/
    Painted sharePainted() const;

    // Pages that exist, in memory or spilled.
    std::size_t pageCount() const;
    // Pages this holds that are in memory, and painted pages it holds that are spilled.
    std::size_t residentPageCount() const;
    std::size_t spilledPageCount() const;

    void clear();

    /**
     * @brief The base an image file gives texels, a rectangle within a size x size tile, as
     * MaterialBake::readBaseTile() reads the whole tile: the file's pixels when it is that size,
     * else sampled bilinearly at each texel's centre with its edges clamped. Only the part of the
     * file the texels need is decoded.
     **/
    static bool readFileRegion(const std::string& path,
                               int size,
                               const RectI& texels,
                               float* rgba,
                               std::string* error) WARN_UNUSED_RETURN;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TexelPages_h
