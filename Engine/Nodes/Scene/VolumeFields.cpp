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

#include "VolumeFields.h"

#include <sys/stat.h>

#include <exception>
#include <mutex>

#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>

NATRON_NAMESPACE_ENTER

namespace VolumeFields {
namespace {
    // Reading a grid's metadata makes the grid, which needs its type registered first.
    void
    initializeOpenVdb()
    {
        static std::once_flag once;

        std::call_once(once, [] { openvdb::initialize(); });
    }

    Grid
    describe(const openvdb::GridBase& grid)
    {
        Grid described;
        described.name = grid.getName();
        described.valueType = grid.valueType();
        const openvdb::Vec3d voxel = grid.voxelSize();
        for (int i = 0; i < 3; ++i) {
            described.voxelSize[i] = voxel[i];
        }
        const openvdb::Vec3IMetadata::ConstPtr lo = grid.getMetadata<openvdb::Vec3IMetadata>(openvdb::GridBase::META_FILE_BBOX_MIN);
        const openvdb::Vec3IMetadata::ConstPtr hi = grid.getMetadata<openvdb::Vec3IMetadata>(openvdb::GridBase::META_FILE_BBOX_MAX);
        if (lo && hi) {
            const openvdb::Vec3i& min = lo->value();
            const openvdb::Vec3i& max = hi->value();
            if ((min[0] <= max[0]) && (min[1] <= max[1]) && (min[2] <= max[2])) {
                // Voxel centres sit at integer index coordinates, so the extents reach half a voxel past them.
                const openvdb::BBoxd index(openvdb::Vec3d(min[0], min[1], min[2]) - openvdb::Vec3d(0.5),
                                           openvdb::Vec3d(max[0], max[1], max[2]) + openvdb::Vec3d(0.5));
                const openvdb::BBoxd world = grid.transform().indexToWorld(index);
                for (int i = 0; i < 3; ++i) {
                    described.boundsMin[i] = world.min()[i];
                    described.boundsMax[i] = world.max()[i];
                }
                described.hasBounds = true;
            }
        }

        return described;
    }
} // anon namespace

bool
readGrids(const std::string& path,
          std::vector<Grid>* grids,
          std::string* error)
{
    grids->clear();
    error->clear();
    struct stat info;
    if ((::stat(path.c_str(), &info) != 0) || !S_ISREG(info.st_mode)) {
        *error = "no file at " + path;

        return false;
    }
    initializeOpenVdb();
    try {
        openvdb::io::File file(path);
        file.open(false);
        const openvdb::GridPtrVecPtr metadata = file.readAllGridMetadata();
        file.close();
        if (metadata) {
            for (const openvdb::GridBase::Ptr& grid : *metadata) {
                if (grid) {
                    grids->push_back(describe(*grid));
                }
            }
        }
    } catch (const std::exception& e) {
        grids->clear();
        *error = path + " is not a readable OpenVDB file: " + e.what();

        return false;
    } catch (...) {
        grids->clear();
        *error = path + " is not a readable OpenVDB file";

        return false;
    }

    return true;
}
} // namespace VolumeFields

NATRON_NAMESPACE_EXIT
