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

#include <string>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/ViewMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

bool
contains(const std::string& text,
         const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

class ViewMetadataTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        view = createNode(QString::fromUtf8(PLUGINID_NATRON_VIEWMETADATA));
        ASSERT_TRUE(source && view);

        MetadataSourceTestEffect* sourceEffect = dynamic_cast<MetadataSourceTestEffect*>(source->getEffectInstance().get());

        ASSERT_TRUE(sourceEffect != NULL);
        ImageMetadata extra;
        extra.setString("exif/Note", "<b>\"a\" & b</b>");
        extra.setDouble("exr/gain", 0.1);
        extra.setIntVector("dpx/size", std::vector<int>(1, 4));
        extra.setDoubleVector("exr/chroma", std::vector<double>(2, 0.5));
        sourceEffect->setExtraMetadata(extra);
        connectNodes(source, view, 0, true);
        effect = dynamic_cast<ViewMetadata*>(view->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL);
    }

    NodePtr source;
    NodePtr view;
    ViewMetadata* effect;
};

TEST_F(ViewMetadataTest, PassesMetadataThrough)
{
    EXPECT_EQ(dynamic_cast<NativeEffectBase*>(source->getEffectInstance().get())->getOutputMetadata(1., ViewIdx(0)),
              effect->getOutputMetadata(1., ViewIdx(0)));
}

TEST_F(ViewMetadataTest, DisplayListsEveryKeyAndValue)
{
    effect->refreshMetadataDisplay(1.);

    const std::string text = effect->getMetadataDisplayText();

    EXPECT_TRUE(contains(text, "exr/tag"));
    EXPECT_TRUE(contains(text, "exr/origin"));
    EXPECT_TRUE(contains(text, "source"));
    EXPECT_TRUE(contains(text, "ofx/frame"));
    EXPECT_TRUE(contains(text, "exr/gain"));
    EXPECT_TRUE(contains(text, "0.1"));
    EXPECT_TRUE(contains(text, "dpx/size"));
    EXPECT_TRUE(contains(text, "exr/chroma"));
    EXPECT_TRUE(contains(text, "0.5, 0.5"));
}

TEST_F(ViewMetadataTest, KeysAreSorted)
{
    effect->refreshMetadataDisplay(1.);

    const std::string text = effect->getMetadataDisplayText();

    EXPECT_LT(text.find("dpx/size"), text.find("exr/chroma"));
    EXPECT_LT(text.find("exr/chroma"), text.find("exr/tag"));
    EXPECT_LT(text.find("exr/tag"), text.find("ofx/frame"));
}

TEST_F(ViewMetadataTest, DisplayFollowsTheFrame)
{
    effect->refreshMetadataDisplay(1.);
    const std::string first = effect->getMetadataDisplayText();

    effect->refreshMetadataDisplay(2.);
    const std::string second = effect->getMetadataDisplayText();

    EXPECT_NE(first, second);
    EXPECT_TRUE(contains(first, "<td><font color=#c8c8c8>1</font></td>"));
    EXPECT_TRUE(contains(second, "<td><font color=#c8c8c8>2</font></td>"));
}

TEST_F(ViewMetadataTest, SpecialCharactersAreEscaped)
{
    effect->refreshMetadataDisplay(1.);

    const std::string text = effect->getMetadataDisplayText();

    EXPECT_TRUE(contains(text, "&lt;b&gt;&quot;a&quot; &amp; b&lt;/b&gt;"));
    EXPECT_FALSE(contains(text, "<b>\"a\""));
}

TEST_F(ViewMetadataTest, RefreshDoesNotChangeTheNodeHash)
{
    const U64 before = view->getHashValue();

    effect->refreshMetadataDisplay(1.);
    effect->refreshMetadataDisplay(2.);

    EXPECT_EQ(before, view->getHashValue());
}

TEST_F(ViewMetadataTest, UnconnectedNodeShowsAnEmptyNotice)
{
    NodePtr lone = createNode(QString::fromUtf8(PLUGINID_NATRON_VIEWMETADATA));
    ASSERT_TRUE(lone);
    ViewMetadata* loneEffect = dynamic_cast<ViewMetadata*>(lone->getEffectInstance().get());

    ASSERT_TRUE(loneEffect != NULL);
    loneEffect->refreshMetadataDisplay(1.);

    EXPECT_TRUE(contains(loneEffect->getMetadataDisplayText(), "No metadata"));
}
