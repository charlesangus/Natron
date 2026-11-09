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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneFileSource.h"

NATRON_NAMESPACE_USING

namespace {
// Two files, each sublayering one of its own, in a directory removed with the fixture.
class TwoFiles {
public:
    TwoFiles()
    {
        static std::atomic<int> counter(0);

        _dir = std::filesystem::temp_directory_path() / ("scene_file_source_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(_dir);
        for (const char* name : { "a", "b" }) {
            std::ofstream((_dir / (std::string(name) + "_sub.usda")).string()) << "#usda 1.0\ndef Xform \"Sub\"\n{\n}\n";
            std::ofstream((_dir / (std::string(name) + ".usda")).string()) << "#usda 1.0\n(\n    subLayers = [@./" << name << "_sub.usda@]\n)\ndef Xform \"World\"\n{\n}\n";
        }
    }

    ~TwoFiles()
    {
        std::error_code ignored;
        std::filesystem::remove_all(_dir, ignored);
    }

    std::string path(const std::string& name) const
    {
        return (_dir / (name + ".usda")).string();
    }

private:
    std::filesystem::path _dir;
};

U64
walkedDigest(const std::string& path)
{
    SceneFileSource source;

    source.load(path);
    source.walk(path);

    return source.getDigest();
}
} // anonymous namespace

TEST(SceneFileSourceTest, AWalkOvertakenByALoadLeavesTheNewFilesDigest)
{
    const TwoFiles files;
    const std::string a = files.path("a");
    const std::string b = files.path("b");
    const U64 walkedA = walkedDigest(a);
    const U64 walkedB = walkedDigest(b);
    ASSERT_NE(walkedA, walkedB);

    for (int i = 0; i < 200; ++i) {
        SceneFileSource source;
        source.load(a);
        std::thread walker([&source, &a]() { source.walk(a); });
        source.load(b);
        walker.join();

        // Whichever came first, the digest is not that of the file no longer named.
        ASSERT_NE(source.getDigest(), walkedA) << "iteration " << i;
        source.walk(b);
        ASSERT_EQ(source.getDigest(), walkedB) << "iteration " << i;
    }
}

TEST(SceneFileSourceTest, AWalkTakesTheDigestOfItsClosure)
{
    const TwoFiles files;
    SceneFileSource source;
    const std::string a = files.path("a");

    source.load(a);
    const U64 unwalked = source.getDigest();
    EXPECT_NE(unwalked, U64(0));
    EXPECT_TRUE(source.walk(a));
    EXPECT_EQ(source.getDigest(), walkedDigest(a));
    // Nothing was reloaded: the second walk has nothing to do.
    EXPECT_FALSE(source.walk(a));
}
