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
#include <QStringList>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"
#include "NativePluginIdentity.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/CompareMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Plugin.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

MetadataSourceTestEffect*
sourceEffectOf(const NodePtr& node)
{
    return dynamic_cast<MetadataSourceTestEffect*>(node->getEffectInstance().get());
}

} // namespace

class CompareMetadataTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        a = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        b = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        compare = createNode(QString::fromUtf8(PLUGINID_NATRON_COMPAREMETADATA));
        ASSERT_TRUE(a && b && compare);
        ASSERT_TRUE(sourceEffectOf(a) != NULL);
        ASSERT_TRUE(sourceEffectOf(b) != NULL);
        effect = dynamic_cast<CompareMetadata*>(compare->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL);
    }

    // A holds exr/aonly and exr/shared=x, B holds exr/bonly, exr/shared=y and a tag of 2.
    void makeSourcesDiffer()
    {
        ImageMetadata aExtra;
        aExtra.setInt("exr/aonly", 3);
        aExtra.setString("exr/shared", "x");
        sourceEffectOf(a)->setExtraMetadata(aExtra);

        ImageMetadata bExtra;
        bExtra.setInt("exr/bonly", 7);
        bExtra.setString("exr/shared", "y");
        sourceEffectOf(b)->setExtraMetadata(bExtra);
        dynamic_cast<KnobInt*>(b->getKnobByName("tag").get())->setValue(2, ViewSpec::all(), 0);
    }

    void connectBoth()
    {
        connectNodes(a, compare, 0, true);
        connectNodes(b, compare, 1, true);
    }

    std::string displayAt(double time)
    {
        effect->refreshDifferencesDisplay(time);

        return effect->getDifferencesDisplayText();
    }

    NodePtr a;
    NodePtr b;
    NodePtr compare;
    CompareMetadata* effect;
};

TEST_F(CompareMetadataTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_COMPAREMETADATA, 1, "CompareMetadata");

    const PluginsMap& plugins = appPTR->getPluginsList();
    const PluginsMap::const_iterator it = plugins.find(PLUGINID_NATRON_COMPAREMETADATA);
    ASSERT_TRUE(it != plugins.end());
    const QStringList grouping = (*it->second.begin())->getGrouping();
    ASSERT_FALSE(grouping.isEmpty());
    EXPECT_EQ(QString::fromUtf8(PLUGIN_GROUP_METADATA), grouping.front());
}

TEST_F(CompareMetadataTest, HasNoChannelSelector)
{
    EXPECT_FALSE(bool(compare->getLayerKnob()));
}

TEST_F(CompareMetadataTest, DifferencesListEachKindInKeyOrder)
{
    makeSourcesDiffer();
    connectBoth();

    EXPECT_EQ("A only: exr/aonly=3\n"
              "B only: exr/bonly=7\n"
              "differs: exr/shared: A=x B=y\n"
              "differs: exr/tag: A=1 B=2",
              displayAt(1.));
}

TEST_F(CompareMetadataTest, IdenticalMetadataGivesAnEmptyDisplay)
{
    connectBoth();

    EXPECT_EQ("", displayAt(1.));
}

TEST_F(CompareMetadataTest, DisconnectedBListsEveryKeyOfAAsAOnly)
{
    connectNodes(a, compare, 0, true);

    EXPECT_EQ("A only: exr/origin=source\n"
              "A only: exr/tag=1\n"
              "A only: ofx/frame=1",
              displayAt(1.));
}

TEST_F(CompareMetadataTest, DisplayFollowsTheFrame)
{
    connectNodes(a, compare, 0, true);

    EXPECT_NE(std::string::npos, displayAt(1.).find("A only: ofx/frame=1"));
    EXPECT_NE(std::string::npos, displayAt(2.).find("A only: ofx/frame=2"));
}

TEST_F(CompareMetadataTest, DisplayFollowsAKnobEditOnASource)
{
    connectBoth();
    ASSERT_EQ("", displayAt(1.));

    dynamic_cast<KnobInt*>(b->getKnobByName("tag").get())->setValue(5, ViewSpec::all(), 0);

    EXPECT_EQ("differs: exr/tag: A=1 B=5", displayAt(1.));
}

TEST_F(CompareMetadataTest, PassesTheMetadataOfAThrough)
{
    makeSourcesDiffer();
    connectBoth();

    EXPECT_EQ(sourceEffectOf(a)->getOutputMetadata(1., ViewIdx(0)), effect->getOutputMetadata(1., ViewIdx(0)));
}

TEST_F(CompareMetadataTest, VectorsAreCommaSeparated)
{
    ImageMetadata left;
    left.setIntVector("exr/size", std::vector<int>({ 1, 2 }));
    left.setDoubleVector("exr/chroma", std::vector<double>({ 0.5, 0.25 }));
    ImageMetadata right;
    right.setDoubleVector("exr/chroma", std::vector<double>({ 0.5, 0.75 }));

    EXPECT_EQ("differs: exr/chroma: A=0.5, 0.25 B=0.5, 0.75\n"
              "A only: exr/size=1, 2",
              CompareMetadata::differences(left, right));
}

TEST_F(CompareMetadataTest, RefreshDoesNotChangeTheNodeHash)
{
    makeSourcesDiffer();
    connectBoth();
    const U64 before = compare->getHashValue();

    EXPECT_FALSE(displayAt(1.).empty());
    EXPECT_FALSE(displayAt(2.).empty());

    EXPECT_EQ(before, compare->getHashValue());
    EXPECT_FALSE(effect->isEvaluationBlocked());
}
