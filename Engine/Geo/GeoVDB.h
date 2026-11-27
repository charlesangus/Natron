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

#ifndef Engine_Geo_GeoVDB_h
#define Engine_Geo_GeoVDB_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Only the version namespace macros: the grid types stay forward-declared, so including this
// header does not pull OpenVDB's trees (and oneTBB) into every geometry translation unit.
#include <openvdb/version.h>

#include "Global/GlobalDefines.h"

#include "Engine/Geo/GeoPacked.h"
#include "Engine/GeoDetail.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
    class GridBase;
}
}

NATRON_NAMESPACE_ENTER

/**
 * A VDB primitive is one vertex on one point, the volume's position (the world-space origin of
 * its index space), plus an immutable OpenVDB grid held in the owning detail's sharedGrids(),
 * so copies of a detail and details built from it share the grid until a builder replaces it.
 * Its name is the string primitive attribute kGeoNameAttr; its transform is the grid's own.
 *
 * A grid handed to these functions must not be modified afterwards by whoever else holds it:
 * its content hash is computed once and cached.
 *
 * Setters take storage offsets, like GeoDetailBuilder, and throw std::invalid_argument for a
 * dead primitive or one that is not a VDB primitive; getters do the same.
 **/
inline constexpr const char* kGeoNameAttr = "name";
// Index into sharedGrids() plus one; 0 means none.
inline constexpr const char* kGeoVDBGridAttr = "vdbGrid";
// World-space bounds of the active voxels, refreshed whenever the grid is set.
inline constexpr const char* kGeoVDBBoundsAttr = "vdbBounds";

typedef std::shared_ptr<const openvdb::GridBase> GeoVDBGridBaseConstPtr;

/** @brief Asked between pieces of long grid work, which stops once it returns true. Empty never stops. */
typedef std::function<bool()> GeoVDBAbortFunc;

/**
 * @brief One shared grid with its content hash: the value type, grid class, vector type, world
 * space flag, half-float saving, transform, and the tree: its background, root tiles, the child
 * and value masks of every internal node with those of its tiles that are active or not the
 * background, and every leaf (topology and values, active or not), nodes hashed in parallel. The
 * grid's name and other metadata do not enter it. Computed on first use and cached; wrappers
 * made by withGrid() over the same tree share the tree's hash and its measured memory.
 **/
class GeoVDBGrid {
public:
    /** @brief Throws std::invalid_argument for a null grid. */
    explicit GeoVDBGrid(GeoVDBGridBaseConstPtr grid);

    GeoVDBGrid(const GeoVDBGrid&) = delete;
    GeoVDBGrid& operator=(const GeoVDBGrid&) = delete;

    const GeoVDBGridBaseConstPtr& grid() const
    {
        return _grid;
    }

    /** @brief A new wrapper around @p grid, sharing this one's tree state when the tree is the same. */
    GeoVDBGridConstPtr withGrid(GeoVDBGridBaseConstPtr grid) const;

    /** @brief The tree this grid holds: equal for wrappers whose grids share one tree. */
    const void* treeIdentity() const;

    /** @brief Never 0. */
    U64 contentHash() const;

    U64 treeHash() const;

    /**
     * @brief Computes and caches both hashes unless @p aborted stops it first, in which case
     * nothing is cached and false is returned. The tree is hashed in blocks, asking between them.
     **/
    bool computeHashes(const GeoVDBAbortFunc& aborted) const;

    /** @brief The grid's tree memory, measured once per tree. */
    std::size_t getSizeInBytes() const
    {
        return _tree->bytes;
    }

private:
    struct TreeState {
        std::size_t bytes = 0;
        std::atomic<U64> hash { 0 };
    };

    GeoVDBGrid(GeoVDBGridBaseConstPtr grid,
               std::shared_ptr<TreeState> tree);

    bool computeTreeHash(const GeoVDBAbortFunc& aborted,
                         U64* hash) const;

    GeoVDBGridBaseConstPtr _grid;
    std::shared_ptr<TreeState> _tree;
    mutable std::atomic<U64> _hash { 0 };
};

/** @brief openvdb::initialize(), once per process; every grid I/O entry point calls it. */
void geoInitializeOpenVDB();

/**
 * @brief Appends a VDB primitive holding @p grid, named @p name, on @p point. Returns the new
 * primitive offset. Throws std::invalid_argument for a null grid or a dead point.
 **/
GeoOffset geoAppendVDB(GeoDetailBuilder& builder,
                       GeoOffset point,
                       GeoVDBGridBaseConstPtr grid,
                       std::string_view name);

/** @brief As above with an existing wrapper, whose cached hashes and measured memory carry over. */
GeoOffset geoAppendVDB(GeoDetailBuilder& builder,
                       GeoOffset point,
                       GeoVDBGridConstPtr grid,
                       std::string_view name);

/** @brief As above, on a new point placed at the grid's index-space origin. */
GeoOffset geoAppendVDB(GeoDetailBuilder& builder,
                       GeoVDBGridBaseConstPtr grid,
                       std::string_view name);

/**
 * @brief Points the primitive at @p grid, a new shared entry; the previous grid is left as it
 * was and dropped by finish() once nothing refers to it. Throws std::invalid_argument for a null
 * grid.
 **/
void geoReplaceVDBGrid(GeoDetailBuilder& builder,
                       GeoOffset primitive,
                       GeoVDBGridBaseConstPtr grid);

void geoSetVDBName(GeoDetailBuilder& builder,
                   GeoOffset primitive,
                   std::string_view name);

/**
 * @brief Composes @p transform after the grid's own index-to-world transform. The new grid
 * shares the old one's tree, so its tree hash is not recomputed.
 **/
void geoTransformVDB(GeoDetailBuilder& builder,
                     GeoOffset primitive,
                     const GeoMatrix4& transform);

const GeoVDBGridConstPtr& geoVDB(const GeoDetail& detail,
                                 GeoOffset primitive);

const GeoVDBGridBaseConstPtr& geoVDBGrid(const GeoDetail& detail,
                                         GeoOffset primitive);

/** @brief Empty when the detail has no name attribute. */
std::string_view geoVDBName(const GeoDetail& detail,
                            GeoOffset primitive);

/** @brief World-space bounds of the active voxels; zero for an empty grid. */
GeoBounds geoVDBBounds(const GeoDetail& detail,
                       GeoOffset primitive);

/** @brief World-space bounds of @p grid's active voxels, voxel extents included; zero when empty. */
GeoBounds geoVDBGridBounds(const openvdb::GridBase& grid);

/** @brief The name stored on @p grid, empty when it has none. */
std::string geoVDBGridName(const openvdb::GridBase& grid);

/** @brief A grid described with the tokens of a USD OpenVDB field asset. */
struct GeoVDBFieldInfo {
    // Empty when no fieldDataType token names the value type.
    std::string dataType;
    // vectorDataRoleHint: None, Point, Normal or Vector.
    std::string roleHint = "None";
    // fieldClass: levelSet, fogVolume, staggered or unknown.
    std::string gridClass = "unknown";
};
GeoVDBFieldInfo geoVDBFieldInfo(const openvdb::GridBase& grid);

/** @brief The names of the grids in the .vdb file at @p path, in file order. */
bool geoVDBFileGridNames(const std::string& path,
                         std::vector<std::string>* names,
                         std::string* errorMessage);

/**
 * @brief Reads the grids of the .vdb file at @p path whose names match @p pattern
 * (geoNameMatchesPattern()), in file order; the others are not read. @p aborted is asked
 * before each grid; once it says so, nothing is returned and the read fails.
 **/
bool geoReadVDBFile(const std::string& path,
                    std::string_view pattern,
                    std::vector<GeoVDBGridBaseConstPtr>* grids,
                    std::string* errorMessage,
                    const GeoVDBAbortFunc& aborted = GeoVDBAbortFunc());

/**
 * @brief Reads the grid named @p name from the .vdb file at @p path: the first one, or with
 * @p index the one after @p index others of that name, in file order. @p aborted is asked before
 * the grid is read; once it says so, nothing is returned and the read fails.
 **/
bool geoReadVDBGrid(const std::string& path,
                    const std::string& name,
                    GeoVDBGridBaseConstPtr* grid,
                    std::string* errorMessage,
                    int index = 0,
                    const GeoVDBAbortFunc& aborted = GeoVDBAbortFunc());

/** @brief A grid to write to a file and the name it is stored under there. */
struct GeoVDBNamedGrid {
    const openvdb::GridBase* grid = nullptr;
    std::string name;
};

/**
 * @brief Writes @p grids, in order and each under its name, as the grids of the .vdb file at
 * @p path; same-named grids keep their order. The file is written next to its final name under a
 * dot-prefixed name and renamed into place, so a reader never sees half of it. @p aborted is asked
 * every few megabytes written; once it says so, the partial file is removed and the write fails.
 **/
bool geoWriteVDBFile(const std::vector<GeoVDBNamedGrid>& grids,
                     const std::string& path,
                     std::string* errorMessage,
                     const GeoVDBAbortFunc& aborted = GeoVDBAbortFunc());

/** @brief As above, @p grid named @p name the only grid of the file. */
bool geoWriteVDBFile(const openvdb::GridBase& grid,
                     std::string_view name,
                     const std::string& path,
                     std::string* errorMessage,
                     const GeoVDBAbortFunc& aborted = GeoVDBAbortFunc());

/** @brief A shared grid to store in a content-addressed file and the name it is stored under. */
struct GeoVDBStoredGrid {
    const GeoVDBGrid* grid = nullptr;
    std::string name;
};

/**
 * @brief The content-addressed file of @p grids, <directory>/<hash>.vdb, written when absent with
 * the grids in order (geoWriteVDBFile()). The hash covers, in order, every grid's content hash
 * (value type, class, transform and tree) and the name it is stored under. In the disk tier's
 * files section, a file found is marked used and a file written is charged to the tier. Hashing
 * and writing ask @p aborted as GeoVDBGrid::computeHashes() and geoWriteVDBFile() do.
 **/
bool geoEnsureVDBFile(const std::vector<GeoVDBStoredGrid>& grids,
                      const std::string& directory,
                      std::string* path,
                      std::string* errorMessage,
                      const GeoVDBAbortFunc& aborted = GeoVDBAbortFunc());

/** @brief As above, @p grid stored as @p name the only grid of the file. */
bool geoEnsureVDBFile(const GeoVDBGrid& grid,
                      std::string_view name,
                      const std::string& directory,
                      std::string* path,
                      std::string* errorMessage,
                      const GeoVDBAbortFunc& aborted = GeoVDBAbortFunc());

/**
 * @brief Where volumes handed to renderers are written: the node cache's disk tier files section,
 * which evicts them by last use under the tier's budget, or <disk cache location>/vdb without a tier.
 **/
std::string geoVDBCacheDirectory();

/**
 * @brief Removes the files of geoVDBCacheDirectory(), as clearing the disk cache does: the tier's
 * files section, or without a tier only the names geoEnsureVDBFile() writes.
 **/
void geoClearVDBCacheDirectory();

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoVDB_h
