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

#include <cstring>
#include <string>

#include <lensfun.h>

#include <gtest/gtest.h>

#include "Engine/Nodes/Transform/LensfunRuntime.h"

NATRON_NAMESPACE_USING

TEST(LensfunLink, VersionNamesThePinnedCommit)
{
    EXPECT_EQ(40u, LensfunRuntime::version().size()) << LensfunRuntime::version();
}

// The build tree puts the database where the bundle does, beside the executables.
TEST(LensfunLink, DatabaseIsFoundAndLoads)
{
    ASSERT_FALSE(LensfunRuntime::databasePath().empty());

    std::string error;
    const lfDatabase* database = LensfunRuntime::database(&error);
    ASSERT_TRUE(database != nullptr) << error;
    EXPECT_EQ(database, LensfunRuntime::database());
}

TEST(LensfunLink, FindsAKnownCameraAndLens)
{
    const lfDatabase* database = LensfunRuntime::database();
    ASSERT_TRUE(database != nullptr);

    const lfCamera** cameras = database->FindCamerasExt("Canon", "Canon EOS 5D Mark II");
    ASSERT_TRUE(cameras != nullptr && cameras[0] != nullptr);
    const lfCamera* camera = cameras[0];
    EXPECT_STREQ("Canon EF", camera->Mount);
    EXPECT_FLOAT_EQ(1.f, camera->CropFactor);

    const lfLens** lenses = database->FindLenses(camera, nullptr, "Canon EF 24-105mm f/4L IS USM");
    ASSERT_TRUE(lenses != nullptr && lenses[0] != nullptr);
    EXPECT_FLOAT_EQ(24.f, lenses[0]->MinFocal);
    EXPECT_FLOAT_EQ(105.f, lenses[0]->MaxFocal);
    lf_free(lenses);
    lf_free(cameras);
}

TEST(LensfunLink, UnknownCameraIsNotFound)
{
    const lfDatabase* database = LensfunRuntime::database();
    ASSERT_TRUE(database != nullptr);

    const lfCamera** cameras = database->FindCamerasExt("No Such Maker", "No Such Camera");
    EXPECT_TRUE(cameras == nullptr);
    lf_free(cameras);
}
