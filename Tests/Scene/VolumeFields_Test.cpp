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

#include "Global/Macros.h"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/VolumeFields.h"

NATRON_NAMESPACE_USING

namespace {
// make-volume-fixtures.py's frame 1: a sphere of radius 0.5 at x = -0.625 in voxels of 1/24.
const char* const kFrame1 = NATRON_TESTS_FIXTURES_DIR "/volume-sphere.0001.vdb";
const char* const kFrame2 = NATRON_TESTS_FIXTURES_DIR "/volume-sphere.0002.vdb";

const VolumeFields::Grid*
findGrid(const std::vector<VolumeFields::Grid>& grids,
         const std::string& name)
{
    const auto it = std::find_if(grids.begin(), grids.end(), [&name](const VolumeFields::Grid& grid) { return grid.name == name; });

    return (it == grids.end()) ? nullptr : &*it;
}
} // anon namespace

TEST(VolumeFields, ListsTheGridsOfAFileWithTheirTypes)
{
    std::vector<VolumeFields::Grid> grids;
    std::string error;
    ASSERT_TRUE(VolumeFields::readGrids(kFrame1, &grids, &error)) << error;
    EXPECT_TRUE(error.empty()) << error;
    ASSERT_EQ(3u, grids.size());
    const VolumeFields::Grid* density = findGrid(grids, "density");
    const VolumeFields::Grid* temperature = findGrid(grids, "temperature");
    const VolumeFields::Grid* velocity = findGrid(grids, "velocity");
    ASSERT_TRUE(density != nullptr);
    ASSERT_TRUE(temperature != nullptr);
    ASSERT_TRUE(velocity != nullptr);
    EXPECT_EQ(std::string("float"), density->valueType);
    EXPECT_EQ(std::string("float"), temperature->valueType);
    EXPECT_EQ(std::string("vec3s"), velocity->valueType);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(1. / 24., density->voxelSize[i], 1e-9) << "axis " << i;
    }
}

// The bounds reach the sphere's surface to within a voxel, on either side of x = 0 by frame.
TEST(VolumeFields, BoundsFollowTheActiveVoxels)
{
    const double voxel = 1. / 24.;
    const double centres[2] = { -0.625, 0.625 };
    const char* const files[2] = { kFrame1, kFrame2 };
    for (int frame = 0; frame < 2; ++frame) {
        std::vector<VolumeFields::Grid> grids;
        std::string error;
        ASSERT_TRUE(VolumeFields::readGrids(files[frame], &grids, &error)) << error;
        const VolumeFields::Grid* density = findGrid(grids, "density");
        ASSERT_TRUE(density != nullptr);
        ASSERT_TRUE(density->hasBounds);
        EXPECT_NEAR(centres[frame] - 0.5, density->boundsMin[0], 1.5 * voxel) << files[frame];
        EXPECT_NEAR(centres[frame] + 0.5, density->boundsMax[0], 1.5 * voxel) << files[frame];
        for (int i = 1; i < 3; ++i) {
            EXPECT_NEAR(-0.5, density->boundsMin[i], 1.5 * voxel) << files[frame] << " axis " << i;
            EXPECT_NEAR(0.5, density->boundsMax[i], 1.5 * voxel) << files[frame] << " axis " << i;
        }
    }
}

TEST(VolumeFields, AMissingFileIsAnError)
{
    std::vector<VolumeFields::Grid> grids(1);
    std::string error;
    EXPECT_FALSE(VolumeFields::readGrids(NATRON_TESTS_FIXTURES_DIR "/no-such-volume.vdb", &grids, &error));
    EXPECT_TRUE(grids.empty());
    EXPECT_NE(std::string::npos, error.find("no-such-volume.vdb")) << error;
}

TEST(VolumeFields, AFileThatIsNotOpenVdbIsAnError)
{
    std::vector<VolumeFields::Grid> grids(1);
    std::string error;
    EXPECT_FALSE(VolumeFields::readGrids(NATRON_TESTS_FIXTURES_DIR "/scene-volume.usda", &grids, &error));
    EXPECT_TRUE(grids.empty());
    EXPECT_NE(std::string::npos, error.find("scene-volume.usda")) << error;
}
