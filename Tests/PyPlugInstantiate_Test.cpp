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

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QtGlobal>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

// AppManager::loadAllPlugins() (and with it, the PyPlug scan of NATRON_PLUGIN_PATH) runs once,
// synchronously, inside wmain.cpp's manager.load() call. Reaching it with the bundled PyPlugs
// directory already on that path means setting the environment variable before main() runs,
// which a dynamic-initializer at namespace scope in any translation unit of this binary
// guarantees regardless of link order.
struct AddPyPlugsDirToNatronPluginPath {
    AddPyPlugsDirToNatronPluginPath()
    {
#ifdef __NATRON_WIN32__
        const char pathSep = ';';
#else
        const char pathSep = ':';
#endif
        const QByteArray existing = qgetenv(NATRON_PLUGIN_PATH_ENV_VAR);
        QByteArray path(NATRON_TESTS_PYPLUGS_DIR);
        if (!existing.isEmpty()) {
            path = existing + pathSep + path;
        }
        qputenv(NATRON_PLUGIN_PATH_ENV_VAR, path);
    }
};
const AddPyPlugsDirToNatronPluginPath addPyPlugsDirToNatronPluginPath;

// fr.inria.SplitAndJoin is excluded: it does "from NatronGui import *", so
// AppManager::loadPythonGroups() deliberately never registers it in this background test
// process (no QApplication, no NatronGui bindings to import), the same as a real headless
// Natron run. It is not a plugin this harness can instantiate.
const char* const kAllBundledPyPlugs[] = {
    "fr.inria.AngleBlur",
    "fr.inria.DropShadow",
    "fr.inria.EdgeBlur",
    "fr.inria.Fill",
    "fr.inria.Glow",
    "fr.inria.LightWrap",
    "fr.inria.PIKColor",
    "fr.inria.ZMask",
    "fr.inria.ZRemap",
};

// The subset of kAllBundledPyPlugs whose exported script pins at least one channel-agnostic
// inner node (one whose EffectInstance::defaultProcessesAllLayers() is true, e.g. a Blur, a
// Transform or a Switch) to the Color layer, rather than leaving it on its own All default.
const char* const kPyPlugsWithColorPinnedInnerNodes[] = {
    "fr.inria.AngleBlur",
    "fr.inria.DropShadow",
    "fr.inria.EdgeBlur",
    "fr.inria.Glow",
    "fr.inria.LightWrap",
    "fr.inria.PIKColor",
    "fr.inria.ZMask",
    "fr.inria.ZRemap",
};

bool
isPluginLoaded(const char* pluginID)
{
    try {
        return appPTR->getPluginBinary(QString::fromUtf8(pluginID), -1, -1, false) != NULL;
    } catch (const std::exception&) {
        return false;
    }
}

KnobChannelSetPtr
channelSetOf(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
}

bool
isColorRow(const ChannelSetRow& row)
{
    return row.mode == ChannelSetRow::eModeLayer && row.layerOrPattern == kNatronColorLayerID;
}

} // namespace

class PyPlugInstantiateTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

TEST_F(PyPlugInstantiateTest, EveryBundledPyPlugInstantiatesWithoutAPythonException)
{
    if (!isPluginLoaded(kAllBundledPyPlugs[0])) {
        // Not practical here: the PyPlugs never made it into this process's plugin registry
        // (see AddPyPlugsDirToNatronPluginPath above), so there is nothing to instantiate.
        std::cerr << "Skipping: PyPlugs under " NATRON_TESTS_PYPLUGS_DIR " were not registered as plugins." << std::endl;
        return;
    }

    for (std::size_t i = 0; i < sizeof(kAllBundledPyPlugs) / sizeof(kAllBundledPyPlugs[0]); ++i) {
        const char* pluginID = kAllBundledPyPlugs[i];
        EXPECT_TRUE(isPluginLoaded(pluginID)) << pluginID;

        // createNodeFromPythonModule() runs createInstance() and, on a Python exception,
        // reports the error and returns a null node instead of the partially built one.
        NodePtr node = createNode(QString::fromUtf8(pluginID));
        EXPECT_TRUE(bool(node)) << pluginID << " failed to instantiate";
    }
}

TEST_F(PyPlugInstantiateTest, ChannelAgnosticInnerNodesOfPyPlugsArePinnedToColor)
{
    if (!isPluginLoaded(kPyPlugsWithColorPinnedInnerNodes[0])) {
        std::cerr << "Skipping: PyPlugs under " NATRON_TESTS_PYPLUGS_DIR " were not registered as plugins." << std::endl;
        return;
    }

    for (std::size_t i = 0; i < sizeof(kPyPlugsWithColorPinnedInnerNodes) / sizeof(kPyPlugsWithColorPinnedInnerNodes[0]); ++i) {
        const char* pluginID = kPyPlugsWithColorPinnedInnerNodes[i];
        NodePtr node = createNode(QString::fromUtf8(pluginID));
        ASSERT_TRUE(bool(node)) << pluginID;

        NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(node->getEffectInstance());
        ASSERT_TRUE(bool(group)) << pluginID;
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group);
        ASSERT_TRUE(bool(collection)) << pluginID;

        NodesList innerNodes;
        collection->getNodes_recursive(innerNodes, true);

        int channelAgnosticNodesChecked = 0;
        for (NodesList::const_iterator it = innerNodes.begin(); it != innerNodes.end(); ++it) {
            const NodePtr& inner = *it;
            if (!inner->getEffectInstance()->defaultProcessesAllLayers()) {
                continue;
            }
            KnobChannelSetPtr channels = channelSetOf(inner);
            if (!channels) {
                continue;
            }
            std::vector<ChannelSetRow> rows = channels->getRows();
            ASSERT_FALSE(rows.empty()) << pluginID << "/" << inner->getScriptName();
            EXPECT_TRUE(isColorRow(rows[0])) << pluginID << "/" << inner->getScriptName();
            ++channelAgnosticNodesChecked;
        }
        EXPECT_GT(channelAgnosticNodesChecked, 0) << pluginID << " has no channel-agnostic inner node to check";
    }
}
