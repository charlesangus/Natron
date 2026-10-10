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
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
bool
isExcluded(const std::string& format)
{
    for (const char* name : OiioReadSupport::kExcludedOiioFormats) {
        if (format == name) {
            return true;
        }
    }

    return false;
}

// The extensions of `format` in OIIO's own list, which is "format:ext,ext;format:ext".
std::set<std::string>
oiioExtensionsOf(const std::string& format)
{
    std::set<std::string> result;
    std::stringstream formats(OIIO::get_string_attribute("extension_list"));
    std::string entry;
    while (std::getline(formats, entry, ';')) {
        const std::size_t colon = entry.find(':');
        if (colon == std::string::npos || entry.substr(0, colon) != format) {
            continue;
        }
        std::stringstream exts(entry.substr(colon + 1));
        std::string ext;
        while (std::getline(exts, ext, ',')) {
            result.insert(ext);
        }
    }

    return result;
}

std::set<std::string>
readableSet()
{
    const std::vector<std::string>& v = OiioReadSupport::readableExtensions();

    return std::set<std::string>(v.begin(), v.end());
}
} // namespace

TEST(NativeReadFormats, ExtensionSetComesFromOiioAndHoldsTheCoreFormats)
{
    const std::set<std::string> readable = readableSet();
    std::string joined;
    for (const std::string& ext : readable) {
        joined += ext + " ";
    }
    std::cout << "native Read extensions: " << joined << std::endl;

    const char* const required[] = { "exr", "png", "jpg", "tif", "dpx", "cin", "tga", "hdr", "pfm", "bmp", "sgi", "webp" };
    for (const char* ext : required) {
        EXPECT_TRUE(readable.count(ext)) << ext;
    }
    EXPECT_EQ(OiioReadSupport::readableExtensions().size(), readable.size()) << "duplicates";
    EXPECT_TRUE(std::is_sorted(OiioReadSupport::readableExtensions().begin(), OiioReadSupport::readableExtensions().end()));
}

TEST(NativeReadFormats, ExcludedFormatsAreAbsentUnlessAKeptFormatClaimsTheExtension)
{
    const std::set<std::string> readable = readableSet();

    std::set<std::string> kept;
    std::stringstream formats(OIIO::get_string_attribute("extension_list"));
    std::string entry;
    while (std::getline(formats, entry, ';')) {
        const std::string name = entry.substr(0, entry.find(':'));
        if (!isExcluded(name)) {
            for (const std::string& ext : oiioExtensionsOf(name)) {
                kept.insert(ext);
            }
        }
    }

    for (const char* const* name = OiioReadSupport::kExcludedOiioFormats; name != OiioReadSupport::kExcludedOiioFormats + 5; ++name) {
        for (const std::string& ext : oiioExtensionsOf(*name)) {
            EXPECT_EQ(kept.count(ext) > 0, readable.count(ext) > 0) << *name << " extension " << ext;
        }
    }
    for (const char* ext : { "cr2", "nef", "dng", "arw", "null", "term", "psd" }) {
        if (!kept.count(ext)) {
            EXPECT_FALSE(readable.count(ext)) << ext;
        }
    }
}

TEST(NativeReadFormats, EveryMemberMapsToAKeptFormatCaseInsensitively)
{
    for (const std::string& ext : OiioReadSupport::readableExtensions()) {
        const std::string format = OiioReadSupport::formatNameForExtension(ext);
        EXPECT_FALSE(format.empty()) << ext;
        EXPECT_FALSE(isExcluded(format)) << ext << " -> " << format;
        EXPECT_TRUE(oiioExtensionsOf(format).count(ext)) << ext << " is not an extension of " << format;
    }
    EXPECT_EQ(OiioReadSupport::formatNameForExtension("exr"), OiioReadSupport::formatNameForExtension("EXR"));
    EXPECT_EQ(OiioReadSupport::formatNameForExtension("exr"), OiioReadSupport::formatNameForExtension(".Exr"));
    EXPECT_TRUE(OiioReadSupport::formatNameForExtension("cr2").empty());
    EXPECT_TRUE(OiioReadSupport::formatNameForExtension("").empty());
}

TEST(NativeReadFormats, PathCheckUsesTheLastExtensionOfTheFileName)
{
    EXPECT_TRUE(OiioReadSupport::isReadablePath("/a/b.c/img.0001.EXR"));
    EXPECT_TRUE(OiioReadSupport::isReadablePath("img.####.png"));
    EXPECT_FALSE(OiioReadSupport::isReadablePath("/a/b.exr/image"));
    EXPECT_FALSE(OiioReadSupport::isReadablePath("shot.cr2"));
    EXPECT_FALSE(OiioReadSupport::isReadablePath("shot."));
}

class NativeReadFormatsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createRead()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        EXPECT_TRUE(bool(node));

        return node;
    }

    static void setFile(const NodePtr& node,
                        const std::string& path)
    {
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(file != NULL);
        file->setValue(path);
    }

    static std::string messageOf(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }
};

TEST_F(NativeReadFormatsTest, AFileOutsideTheSetPostsAnUnsupportedFormatMessageAndFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/png-8bit.png");
    const QString raw = dir.path() + QString::fromUtf8("/shot.cr2");
    ASSERT_TRUE(QFile::copy(source, raw));

    NodePtr node = createRead();
    ASSERT_TRUE(bool(node));
    NativeRead* read = dynamic_cast<NativeRead*>(node->getEffectInstance().get());
    ASSERT_TRUE(read != NULL);

    setFile(node, raw.toStdString());
    RectD rod;
    EXPECT_EQ(eStatusFailed, read->getRegionOfDefinition(0, 1., RenderScale(), ViewIdx(0), &rod));
    EXPECT_TRUE(node->hasPersistentMessage());
    EXPECT_NE(std::string::npos, messageOf(node).find("Unsupported format")) << messageOf(node);

    setFile(node, std::string(NATRON_TESTS_FIXTURES_DIR "/png-8bit.png"));
    EXPECT_EQ(eStatusOK, read->getRegionOfDefinition(0, 1., RenderScale(), ViewIdx(0), &rod));
    EXPECT_FALSE(node->hasPersistentMessage()) << messageOf(node);
}
