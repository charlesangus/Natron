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
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/IO/OiioWriteSupport.h"

NATRON_NAMESPACE_USING

namespace {
bool
has(const std::vector<std::string>& v,
    const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool
hasChoice(const std::vector<OiioWriteSupport::CompressionChoice>& v,
          const std::string& id)
{
    for (const OiioWriteSupport::CompressionChoice& c : v) {
        if (id == c.id) {
            return true;
        }
    }

    return false;
}
} // namespace

TEST(OiioWriteSupport, WritableSet)
{
    const std::vector<std::string>& exts = OiioWriteSupport::writableExtensions();
    std::cout << "writable:";
    for (const std::string& e : exts) {
        std::cout << ' ' << e;
    }
    std::cout << std::endl;

    for (const char* e : { "exr", "png", "jpg", "tif", "dpx", "tga", "hdr", "pfm", "bmp", "sgi", "webp" }) {
        EXPECT_TRUE(has(exts, e)) << e;
    }
    // Read-only or excluded formats: raw, null, term, psd, ffmpeg, plus cineon and dds, which OIIO only reads.
    for (const char* e : { "cr2", "nef", "dng", "null", "term", "psd", "mov", "mp4", "cin", "dds", "ptex", "vdb" }) {
        EXPECT_FALSE(has(exts, e)) << e;
    }

    std::set<std::string> excluded;
    for (const char* name : OiioReadSupport::kExcludedOiioFormats) {
        excluded.insert(name);
    }
    EXPECT_TRUE(std::is_sorted(exts.begin(), exts.end()));
    for (const std::string& e : exts) {
        const std::string format = OiioWriteSupport::formatNameForWritableExtension(e);
        EXPECT_FALSE(format.empty()) << e;
        EXPECT_EQ(0u, excluded.count(format)) << e;
        EXPECT_TRUE(OiioWriteSupport::capabilities(format).known) << e;
    }
}

TEST(OiioWriteSupport, Lookups)
{
    EXPECT_EQ("openexr", OiioWriteSupport::formatNameForWritableExtension("EXR"));
    EXPECT_EQ("openexr", OiioWriteSupport::formatNameForWritableExtension(".exr"));
    EXPECT_EQ("png", OiioWriteSupport::formatNameForWritableExtension("png"));
    EXPECT_EQ("", OiioWriteSupport::formatNameForWritableExtension("nope"));
    EXPECT_TRUE(OiioWriteSupport::isWritablePath("/tmp/a.b/shot.0001.EXR"));
    EXPECT_TRUE(OiioWriteSupport::isWritablePath("out.Jpg"));
    EXPECT_FALSE(OiioWriteSupport::isWritablePath("/tmp/a.exr/shot"));
    EXPECT_FALSE(OiioWriteSupport::isWritablePath("shot.cr2"));
    EXPECT_FALSE(OiioWriteSupport::isWritablePath("shot"));
}

TEST(OiioWriteSupport, AutoDataType)
{
    using OiioWriteSupport::DataType;
    EXPECT_EQ(DataType::Half, OiioWriteSupport::autoDataType("openexr"));
    EXPECT_EQ(DataType::UInt8, OiioWriteSupport::autoDataType("png"));
    EXPECT_EQ(DataType::UInt8, OiioWriteSupport::autoDataType("jpeg"));
    EXPECT_EQ(DataType::Float, OiioWriteSupport::autoDataType("hdr"));
    EXPECT_EQ(DataType::Float, OiioWriteSupport::autoDataType("pnm", "pfm"));
    EXPECT_EQ(DataType::UInt8, OiioWriteSupport::autoDataType("pnm", "ppm"));
    EXPECT_EQ(DataType::UInt16, OiioWriteSupport::autoDataType("tiff"));
    EXPECT_EQ(DataType::UInt16, OiioWriteSupport::autoDataType("dpx"));

    for (const std::string& e : OiioWriteSupport::writableExtensions()) {
        const std::string format = OiioWriteSupport::formatNameForWritableExtension(e);
        const DataType a = OiioWriteSupport::autoDataType(format, e);
        const std::vector<DataType>& valid = OiioWriteSupport::validDataTypes(format);
        EXPECT_FALSE(valid.empty()) << format;
        EXPECT_TRUE(std::find(valid.begin(), valid.end(), a) != valid.end()) << e << " " << OiioWriteSupport::dataTypeId(a);
    }

    DataType t;
    EXPECT_TRUE(OiioWriteSupport::dataTypeFromId("16f", &t));
    EXPECT_EQ(DataType::Half, t);
    EXPECT_FALSE(OiioWriteSupport::dataTypeFromId("auto", &t));
    EXPECT_EQ(OIIO::TypeDesc::UINT16, OiioWriteSupport::dataTypeOiio(DataType::UInt10));
    EXPECT_EQ(10, OiioWriteSupport::dataTypeBitsPerSample(DataType::UInt10));
}

TEST(OiioWriteSupport, Capabilities)
{
    const OiioWriteSupport::FormatCaps exr = OiioWriteSupport::capabilities("openexr");
    EXPECT_TRUE(exr.known);
    EXPECT_TRUE(exr.tiles);
    EXPECT_TRUE(exr.displayWindow);
    EXPECT_TRUE(exr.multiImage);

    const OiioWriteSupport::FormatCaps jpeg = OiioWriteSupport::capabilities("jpeg");
    EXPECT_TRUE(jpeg.known);
    EXPECT_FALSE(jpeg.tiles);
    EXPECT_FALSE(jpeg.multiImage);

    EXPECT_FALSE(OiioWriteSupport::capabilities("no-such-format").known);
}

TEST(OiioWriteSupport, OptionTable)
{
    const std::vector<OiioWriteSupport::CompressionChoice>& exr = OiioWriteSupport::compressionChoices("openexr");
    ASSERT_FALSE(exr.empty());
    EXPECT_STREQ("default", exr[0].id);
    for (const char* id : { "none", "zip", "zips", "rle", "piz", "pxr24", "b44", "b44a", "dwaa", "dwab" }) {
        EXPECT_TRUE(hasChoice(exr, id)) << id;
    }
    EXPECT_FALSE(hasChoice(exr, "lzw"));
    EXPECT_TRUE(OiioWriteSupport::compressionChoices("jpeg").empty());

    OiioWriteSupport::ParamVisibility v = OiioWriteSupport::paramVisibility("openexr", "default");
    EXPECT_TRUE(v.compression);
    EXPECT_TRUE(v.tileSize);
    EXPECT_TRUE(v.partSplitting);
    EXPECT_TRUE(v.views);
    EXPECT_FALSE(v.quality);
    EXPECT_FALSE(v.dwaCompressionLevel);
    EXPECT_FALSE(v.zipCompressionLevel);

    EXPECT_TRUE(OiioWriteSupport::paramVisibility("openexr", "dwaa").dwaCompressionLevel);
    EXPECT_TRUE(OiioWriteSupport::paramVisibility("openexr", "zips").zipCompressionLevel);
    EXPECT_FALSE(OiioWriteSupport::paramVisibility("openexr", "zips").dwaCompressionLevel);
    EXPECT_TRUE(OiioWriteSupport::paramVisibility("tiff", "jpeg").quality);
    EXPECT_FALSE(OiioWriteSupport::paramVisibility("tiff", "lzw").quality);

    v = OiioWriteSupport::paramVisibility("jpeg", "default");
    EXPECT_FALSE(v.compression);
    EXPECT_TRUE(v.quality);
    EXPECT_FALSE(v.tileSize);
    EXPECT_FALSE(v.partSplitting);
    EXPECT_FALSE(v.views);
    EXPECT_FALSE(v.dwaCompressionLevel);

    EXPECT_FALSE(OiioWriteSupport::paramVisibility("png", "default").quality);
    EXPECT_FALSE(OiioWriteSupport::paramVisibility("no-such-format", "default").compression);
}

TEST(OiioWriteSupport, EmptyAndUnknownNamesLeaveNoPendingOiioError)
{
    OIIO::geterror();

    EXPECT_FALSE(OiioWriteSupport::capabilities("").known);
    EXPECT_FALSE(OiioWriteSupport::capabilities("no-such-format").known);
    EXPECT_TRUE(OiioWriteSupport::formatNameForWritableExtension("").empty());
    EXPECT_FALSE(OiioWriteSupport::isWritablePath(""));
    OiioWriteSupport::autoDataType("");
    OiioWriteSupport::validDataTypes("");
    OiioWriteSupport::compressionChoices("");
    OiioWriteSupport::paramVisibility("", "default");

    EXPECT_TRUE(OIIO::geterror().empty());
}
