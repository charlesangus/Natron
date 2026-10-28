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

#include <optional>
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
#include "Engine/Nodes/Metadata/ContributeMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Plugin.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const std::string kPrefix(kContributeMetadataKeyPrefix);

} // namespace

class ContributeMetadataTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        contribute = createNode(QString::fromUtf8(PLUGINID_NATRON_CONTRIBUTEMETADATA));
        ASSERT_TRUE(source && contribute);

        MetadataSourceTestEffect* sourceEffect = dynamic_cast<MetadataSourceTestEffect*>(source->getEffectInstance().get());
        ASSERT_TRUE(sourceEffect != NULL);
        ImageMetadata extra;
        extra.setString("ofx/sampletype", "float");
        extra.setDouble("ofx/framerate", 24.);
        sourceEffect->setExtraMetadata(extra);
        connectNodes(source, contribute, 0, true);
        effect = dynamic_cast<NativeEffectBase*>(contribute->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL);
    }

    ImageMetadata output()
    {
        return effect->getOutputMetadata(1., ViewIdx(0));
    }

    void setMode(ContributeMetadata::ModeEnum mode)
    {
        dynamic_cast<KnobChoice*>(contribute->getKnobByName("mode").get())->setValue((int)mode, ViewSpec::all(), 0);
    }

    void setString(const std::string& knob,
                   const std::string& value)
    {
        dynamic_cast<KnobString*>(contribute->getKnobByName(knob).get())->setValue(value, ViewSpec::all(), 0);
    }

    // The seven keys this node always publishes.
    static void expectOwnKeys(const ImageMetadata& m)
    {
        EXPECT_EQ(std::optional<std::string>(""), m.getString(kPrefix + "note"));
        EXPECT_EQ(std::optional<int>(1), m.getInt(kPrefix + "revision"));
        EXPECT_EQ(std::optional<double>(0.75), m.getDouble(kPrefix + "quality"));
        EXPECT_EQ(std::optional<std::string>("reviewed, approved"), m.getString(kPrefix + "tags"));
        EXPECT_EQ(std::optional<std::vector<int>>(std::vector<int>({ 0, 0, 1280, 720 })), m.getIntVector(kPrefix + "renderRegion"));
        EXPECT_EQ(std::optional<std::vector<double>>(std::vector<double>({ 1., 0.5, 0.25 })), m.getDoubleVector(kPrefix + "weights"));
        EXPECT_EQ(std::optional<double>(30.), m.getDouble("ofx/framerate"));
    }

    NodePtr source;
    NodePtr contribute;
    NativeEffectBase* effect;
};

TEST_F(ContributeMetadataTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_CONTRIBUTEMETADATA, 1, "ContributeMetadata");

    const PluginsMap& plugins = appPTR->getPluginsList();
    const PluginsMap::const_iterator it = plugins.find(PLUGINID_NATRON_CONTRIBUTEMETADATA);
    ASSERT_TRUE(it != plugins.end());
    const QStringList grouping = (*it->second.begin())->getGrouping();
    ASSERT_FALSE(grouping.isEmpty());
    EXPECT_EQ(QString::fromUtf8(PLUGIN_GROUP_METADATA), grouping.front());
}

TEST_F(ContributeMetadataTest, HasNoChannelSelector)
{
    EXPECT_FALSE(bool(contribute->getLayerKnob()));
}

TEST_F(ContributeMetadataTest, PublishesEveryKeyWithItsType)
{
    expectOwnKeys(output());
}

TEST_F(ContributeMetadataTest, FrameRateIsThirtyWhateverUpstreamSays)
{
    const ImageMetadata m = output();

    EXPECT_EQ(std::optional<double>(30.), m.getDouble("ofx/framerate"));
}

TEST_F(ContributeMetadataTest, NoteFollowsTheKnob)
{
    ASSERT_EQ(std::optional<std::string>(""), output().getString(kPrefix + "note"));

    setString("note", "hello");

    EXPECT_EQ(std::optional<std::string>("hello"), output().getString(kPrefix + "note"));
}

TEST_F(ContributeMetadataTest, InheritAllKeepsTheUpstreamKeys)
{
    setMode(ContributeMetadata::eModeInheritAll);
    const ImageMetadata m = output();

    expectOwnKeys(m);
    EXPECT_EQ(std::optional<std::string>("source"), m.getString("exr/origin"));
    EXPECT_EQ(std::optional<int>(1), m.getInt("exr/tag"));
    EXPECT_EQ(std::optional<int>(1), m.getInt("ofx/frame"));
    EXPECT_EQ(std::optional<std::string>("float"), m.getString("ofx/sampletype"));
    EXPECT_EQ(11u, m.size());
}

TEST_F(ContributeMetadataTest, DropOneKeyRemovesExactlyThatKey)
{
    setMode(ContributeMetadata::eModeDropOneKey);
    const ImageMetadata m = output();

    expectOwnKeys(m);
    EXPECT_FALSE(m.contains("ofx/sampletype"));
    EXPECT_TRUE(m.contains("exr/origin"));
    EXPECT_TRUE(m.contains("exr/tag"));
    EXPECT_TRUE(m.contains("ofx/frame"));
    EXPECT_EQ(10u, m.size());

    setString("dropKey", "exr/origin");
    const ImageMetadata other = output();

    EXPECT_FALSE(other.contains("exr/origin"));
    EXPECT_TRUE(other.contains("ofx/sampletype"));
    EXPECT_EQ(10u, other.size());
}

TEST_F(ContributeMetadataTest, DroppingAKeyUpstreamLacksIsANoOp)
{
    setMode(ContributeMetadata::eModeDropOneKey);
    setString("dropKey", "exr/absent");

    EXPECT_EQ(11u, output().size());
}

TEST_F(ContributeMetadataTest, InheritNothingLeavesOnlyTheOwnKeys)
{
    setMode(ContributeMetadata::eModeInheritNothing);
    const ImageMetadata m = output();

    expectOwnKeys(m);
    EXPECT_EQ(7u, m.size());
}
