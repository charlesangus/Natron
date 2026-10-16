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

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Clamp.h"
#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Color/ColorLookup.h"
#include "Engine/Nodes/Color/ColorMathNode.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Color/Saturation.h"
#include "Engine/Nodes/Deep/DeepWrite.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/Keyer/ChromaKeyer.h"
#include "Engine/Nodes/Keyer/Keyer.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Metadata/CopyMetadata.h"
#include "Engine/Nodes/Metadata/ModifyMetadata.h"
#include "Engine/Nodes/Metadata/RemoveMetadata.h"
#include "Engine/Nodes/Metadata/ViewMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/Position.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

struct NativeEntry {
    const char* id;
    int major;
};

// Later families append their IDs here.
const std::vector<NativeEntry>&
nativeTable()
{
    static const std::vector<NativeEntry> table = {
        { PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE },
        { PLUGINID_NATRON_COLORCORRECT, PLUGIN_MAJOR_NATRON_COLORCORRECT },
        { PLUGINID_NATRON_SATURATION, PLUGIN_MAJOR_NATRON_SATURATION },
        { PLUGINID_NATRON_CLAMP, PLUGIN_MAJOR_NATRON_CLAMP },
        { PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT },
        { PLUGINID_NATRON_ADD, PLUGIN_MAJOR_NATRON_COLORMATH },
        { PLUGINID_NATRON_MULTIPLY, PLUGIN_MAJOR_NATRON_COLORMATH },
        { PLUGINID_NATRON_GAMMA, PLUGIN_MAJOR_NATRON_COLORMATH },
        { PLUGINID_NATRON_MERGE, PLUGIN_MAJOR_NATRON_MERGE },
        { PLUGINID_NATRON_MERGE_PREFIX "Plus", PLUGIN_MAJOR_NATRON_MERGE },
        { PLUGINID_NATRON_MERGE_PREFIX "Matte", PLUGIN_MAJOR_NATRON_MERGE },
        { PLUGINID_NATRON_MERGE_PREFIX "Multiply", PLUGIN_MAJOR_NATRON_MERGE },
        { PLUGINID_NATRON_MERGE_PREFIX "In", PLUGIN_MAJOR_NATRON_MERGE },
        { PLUGINID_NATRON_MERGE_PREFIX "Out", PLUGIN_MAJOR_NATRON_MERGE },
        { PLUGINID_NATRON_MERGE_PREFIX "Screen", PLUGIN_MAJOR_NATRON_MERGE },
        { PLUGINID_NATRON_MERGE_PREFIX "Max", PLUGIN_MAJOR_NATRON_MERGE },
        { PLUGINID_NATRON_MERGE_PREFIX "Min", PLUGIN_MAJOR_NATRON_MERGE },
        { PLUGINID_NATRON_MERGE_PREFIX "Difference", PLUGIN_MAJOR_NATRON_MERGE },
        { PLUGINID_NATRON_DISSOLVE, PLUGIN_MAJOR_NATRON_DISSOLVE },
        { PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT },
        { PLUGINID_NATRON_SOLID, PLUGIN_MAJOR_NATRON_CONSTANT },
        { PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD },
        { PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM },
        { PLUGINID_NATRON_TRANSFORMMASKED, PLUGIN_MAJOR_NATRON_TRANSFORMMASKED },
        { PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP },
        { PLUGINID_NATRON_POSITION, PLUGIN_MAJOR_NATRON_POSITION },
        { PLUGINID_NATRON_REFORMAT, PLUGIN_MAJOR_NATRON_REFORMAT },
        { PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR },
        { PLUGINID_NATRON_KEYER, PLUGIN_MAJOR_NATRON_KEYER },
        { PLUGINID_NATRON_CHROMAKEYER, PLUGIN_MAJOR_NATRON_CHROMAKEYER },
        { PLUGINID_NATRON_ERODE, PLUGIN_MAJOR_NATRON_ERODE },
        { PLUGINID_NATRON_DILATE, PLUGIN_MAJOR_NATRON_DILATE },
        { PLUGINID_NATRON_EDGEDETECT, PLUGIN_MAJOR_NATRON_EDGEDETECT },
        { PLUGINID_NATRON_COLORLOOKUP, PLUGIN_MAJOR_NATRON_COLORLOOKUP },
        { PLUGINID_NATRON_READ, PLUGIN_MAJOR_NATRON_READ },
        { PLUGINID_NATRON_WRITE, PLUGIN_MAJOR_NATRON_WRITE },
    };

    return table;
}

} // namespace

class NativePluginListTest
    : public BaseTest {
};

TEST_F(NativePluginListTest, RetiredOfxIdsResolveToASingleNativeVersion)
{
    const PluginsMap& plugins = appPTR->getPluginsList();

    for (const NativeEntry& entry : nativeTable()) {
        const PluginsMap::const_iterator it = plugins.find(entry.id);
        ASSERT_TRUE(it != plugins.end()) << entry.id;
        ASSERT_EQ(1u, it->second.size()) << entry.id << " must have no OFX version left";
        EXPECT_EQ(entry.major, (*it->second.begin())->getMajorVersion()) << entry.id;
    }
}

TEST_F(NativePluginListTest, CreatedNodesAreNative)
{
    for (const NativeEntry& entry : nativeTable()) {
        NodePtr node = createNode(QString::fromUtf8(entry.id));
        ASSERT_TRUE(bool(node)) << entry.id;
        EXPECT_TRUE(dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get())) << entry.id;
    }
}

TEST_F(NativePluginListTest, MetadataNodesAreGroupedAndDescribed)
{
    const char* const ids[] = {
        PLUGINID_NATRON_MODIFYMETADATA,
        PLUGINID_NATRON_COPYMETADATA,
        PLUGINID_NATRON_REMOVEMETADATA,
        PLUGINID_NATRON_VIEWMETADATA,
    };
    const PluginsMap& plugins = appPTR->getPluginsList();

    for (const char* id : ids) {
        const PluginsMap::const_iterator it = plugins.find(id);
        ASSERT_TRUE(it != plugins.end()) << id;
        ASSERT_FALSE(it->second.empty()) << id;

        const QStringList grouping = (*it->second.begin())->getGrouping();
        ASSERT_FALSE(grouping.isEmpty()) << id;
        EXPECT_EQ(QString::fromUtf8(PLUGIN_GROUP_METADATA), grouping.front()) << id;

        NodePtr node = createNode(QString::fromUtf8(id));
        ASSERT_TRUE(bool(node)) << id;
        EXPECT_FALSE(node->getPluginDescription().empty()) << id;
    }
}

TEST_F(NativePluginListTest, OfxMetadataNodesAreGroupedWithTheNativeOnes)
{
    const char* const ids[] = {
        "org.openfx.examples.metadataCompare",
        "org.openfx.examples.metadataContribute",
        "org.openfx.examples.metadataTimeCode",
    };
    const PluginsMap& plugins = appPTR->getPluginsList();

    for (const char* id : ids) {
        const PluginsMap::const_iterator it = plugins.find(id);
        if (it == plugins.end()) {
            ADD_FAILURE() << id << " is not loaded; it is bundled in build/assets/Plugins";
            continue;
        }
        ASSERT_FALSE(it->second.empty()) << id;
        const QStringList grouping = (*it->second.begin())->getGrouping();
        ASSERT_EQ(1, grouping.size()) << id;
        EXPECT_EQ(QString::fromUtf8(PLUGIN_GROUP_METADATA), grouping.front()) << id;
    }
}

TEST_F(NativePluginListTest, NativeReadIsTheOnlyReader)
{
    for (const auto& versions : appPTR->getPluginsList()) {
        for (const Plugin* plugin : versions.second) {
            if (plugin->isReader()) {
                EXPECT_EQ(std::string(PLUGINID_NATRON_READ), versions.first);
            }
        }
    }
}

TEST_F(NativePluginListTest, OfxReaderIDBuildsNothing)
{
    CreateNodeArgs args(PLUGINID_OFX_READOIIO, getApp()->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);

    EXPECT_FALSE(bool(getApp()->createNode(args)));
}

TEST_F(NativePluginListTest, NativeWriteIsTheOnlyImageWriter)
{
    for (const auto& versions : appPTR->getPluginsList()) {
        for (const Plugin* plugin : versions.second) {
            if (plugin->isWriter()) {
                EXPECT_TRUE(versions.first == PLUGINID_NATRON_WRITE || versions.first == PLUGINID_NATRON_DEEPWRITE) << versions.first;
            }
        }
    }
}

TEST_F(NativePluginListTest, OfxWriterIDBuildsNothing)
{
    CreateNodeArgs args(PLUGINID_OFX_WRITEOIIO, getApp()->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);

    EXPECT_FALSE(bool(getApp()->createNode(args)));
}
