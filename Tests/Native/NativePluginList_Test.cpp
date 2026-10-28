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

#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "BaseTest.h"
#include "TestBuiltInPlugins.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Deep/DeepWrite.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Metadata/CopyMetadata.h"
#include "Engine/Nodes/Metadata/ModifyMetadata.h"
#include "Engine/Nodes/Metadata/RemoveMetadata.h"
#include "Engine/Nodes/Metadata/ViewMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/USDWrite.h"
#include "Engine/OfxRetired.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"

#define NATRON_NATIVE_REGISTRY_HEADERS
#include "NativeRegistryAll.inc"
#undef NATRON_NATIVE_REGISTRY_HEADERS

NATRON_NAMESPACE_USING

namespace {

struct FragmentEntry {
    std::string id;
    void (*builder)();
    bool internal;
    int major;
};

// The builder is compared by address with the one the registry stored for the same ID.
template <typename PLUGIN>
FragmentEntry
fragmentEntry()
{
    EffectInstancePtr effect(PLUGIN::BuildEffect(NodePtr()));
    const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(effect.get());

    if (!native) {
        ADD_FAILURE() << effect->getPluginID() << " has a registration fragment but is not a NativeEffectBase";

        return FragmentEntry { effect->getPluginID(), nullptr, false, 0 };
    }

    return FragmentEntry { std::string(native->nodeSpec()->id), (void (*)())&PLUGIN::BuildEffect, native->nodeSpec()->internal, native->nodeSpec()->majorVersion };
}

// Expands the generated aggregate of the registration fragments with a recording macro, so the
// fragments themselves are the source of truth rather than a copy of them kept here.
std::vector<FragmentEntry>
collectFragments()
{
    std::vector<FragmentEntry> entries;

#define NATRON_NATIVE_NODE(CLASS, HEADER) entries.push_back(fragmentEntry<CLASS>());
#include "NativeRegistryAll.inc"
#undef NATRON_NATIVE_NODE

    return entries;
}

const std::vector<FragmentEntry>&
fragments()
{
    static const std::vector<FragmentEntry> list = collectFragments();

    return list;
}

std::set<std::string>
fragmentIds()
{
    std::set<std::string> ids;

    for (const FragmentEntry& entry : fragments()) {
        ids.insert(entry.id);
    }

    return ids;
}

} // namespace

class NativePluginListTest
    : public BaseTest {
};

TEST_F(NativePluginListTest, EveryFragmentIsRegisteredAndEveryNativePluginHasAFragment)
{
    std::map<std::string, void (*)()> fragmentBuilders;

    for (const FragmentEntry& entry : fragments()) {
        fragmentBuilders[entry.id] = entry.builder;
    }
    EXPECT_FALSE(fragmentBuilders.empty());

    std::map<std::string, void (*)()> registeredBuilders;

    for (const auto& versions : appPTR->getPluginsList()) {
        for (const Plugin* plugin : versions.second) {
            const LibraryBinary* binary = plugin->getLibraryBinary();
            if (!binary || plugin->getOfxPlugin()) {
                continue;
            }
            const std::pair<bool, EffectBuilder> builder = binary->findFunction<EffectBuilder>("BuildEffect");
            if (!builder.first) {
                continue;
            }
            // A plug-in hand-registered outside the fragments is fine unless it is native.
            bool matchesFragment = false;
            for (const FragmentEntry& entry : fragments()) {
                if (entry.builder == (void (*)())builder.second) {
                    matchesFragment = true;
                    break;
                }
            }
            if (!matchesFragment) {
                if (testRegisteredPluginIds().count(versions.first)) {
                    continue;
                }
                EffectInstancePtr effect(builder.second(NodePtr()));
                EXPECT_FALSE(dynamic_cast<const NativeEffectBase*>(effect.get())) << versions.first << " is a native plug-in registered without a .reg.inc fragment";
                continue;
            }
            registeredBuilders[versions.first] = (void (*)())builder.second;
        }
    }

    for (const auto& entry : fragmentBuilders) {
        const std::map<std::string, void (*)()>::const_iterator it = registeredBuilders.find(entry.first);
        if (it == registeredBuilders.end()) {
            ADD_FAILURE() << entry.first << " has a registration fragment but is not registered under that ID";
        } else {
            EXPECT_TRUE(it->second == entry.second) << entry.first << " is registered with a different class than its fragment names";
        }
    }
    EXPECT_EQ(fragmentBuilders.size(), registeredBuilders.size());
}

TEST_F(NativePluginListTest, NoTwoFragmentsRegisterTheSameId)
{
    std::set<std::string> seen;

    for (const FragmentEntry& entry : fragments()) {
        EXPECT_TRUE(seen.insert(entry.id).second) << entry.id << " is registered by more than one fragment";
    }
}

TEST_F(NativePluginListTest, EveryNativeIdHasASingleVersion)
{
    const PluginsMap& plugins = appPTR->getPluginsList();

    for (const FragmentEntry& entry : fragments()) {
        const PluginsMap::const_iterator it = plugins.find(entry.id);
        ASSERT_TRUE(it != plugins.end()) << entry.id;
        ASSERT_EQ(1u, it->second.size()) << entry.id << " is registered more than once";
    }
}

// Native nodes are named by their plain display name, never a reverse-domain OFX-style ID, and
// start at major 1. The listed nodes are still to be folded into another node.
TEST_F(NativePluginListTest, NativeIdsArePlainAtMajorOne)
{
    const std::set<std::string> pending = {
        PLUGINID_NATRON_ERODE,
        PLUGINID_NATRON_DILATE,
    };
    const PluginsMap& plugins = appPTR->getPluginsList();

    for (const FragmentEntry& entry : fragments()) {
        if (pending.count(entry.id)) {
            continue;
        }
        EXPECT_EQ(std::string::npos, entry.id.find('.')) << entry.id << " is a reverse-domain ID";
        EXPECT_EQ(1, entry.major) << entry.id;
        const PluginsMap::const_iterator it = plugins.find(entry.id);
        ASSERT_TRUE(it != plugins.end()) << entry.id;
        ASSERT_FALSE(it->second.empty()) << entry.id;
        EXPECT_EQ(1, (*it->second.begin())->getMajorVersion()) << entry.id;
    }
}

// A marker naming a native ID would retire the native node itself.
TEST_F(NativePluginListTest, NoRetiredOfxIdIsANativePluginId)
{
    const std::set<std::string> nativeIds = fragmentIds();

    for (const auto& retired : retiredOfxPlugins()) {
        EXPECT_EQ(0u, nativeIds.count(retired.first)) << "Engine/OfxRetired/" << retired.first << " retires an ID a native node registers";
    }
}

TEST_F(NativePluginListTest, EveryRetiredOfxReplacementIsANativePlugin)
{
    const std::set<std::string> nativeIds = fragmentIds();

    for (const auto& retired : retiredOfxPlugins()) {
        if (retired.second.empty()) {
            continue;
        }
        EXPECT_EQ(1u, nativeIds.count(retired.second)) << "Engine/OfxRetired/" << retired.first << " names " << retired.second << ", which is not a registered native plug-in";
    }
}

TEST_F(NativePluginListTest, CreatedNodesAreNative)
{
    for (const FragmentEntry& entry : fragments()) {
        // Internal-use-only plug-ins are not creatable through the user path.
        if (entry.internal) {
            continue;
        }
        NodePtr node = createNode(QString::fromStdString(entry.id));
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
                // USDWrite writes scenes, not images.
                EXPECT_TRUE(versions.first == PLUGINID_NATRON_WRITE || versions.first == PLUGINID_NATRON_DEEPWRITE || versions.first == PLUGINID_NATRON_USDWRITE) << versions.first;
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

// The native Merge is the only Merge: no net.sf.openfx.Merge* ID is registered, and the old
// node's ID builds nothing (an old project loads it as a missing plug-in).
TEST_F(NativePluginListTest, MergeIsTheOnlyMerge)
{
    bool found = false;
    for (const auto& versions : appPTR->getPluginsList()) {
        EXPECT_NE(0u, versions.first.rfind("net.sf.openfx.Merge", 0)) << versions.first;
        found = found || (versions.first == PLUGINID_NATRON_NATIVE_MERGE);
    }
    EXPECT_TRUE(found);

    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE));
    ASSERT_TRUE(bool(merge));
    EXPECT_TRUE(dynamic_cast<NativeEffectBase*>(merge->getEffectInstance().get()));

    CreateNodeArgs args("net.sf.openfx.MergePlugin", getApp()->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);

    EXPECT_FALSE(bool(getApp()->createNode(args)));
}
