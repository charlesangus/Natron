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

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Plugin.h"

NATRON_NAMESPACE_USING

typedef BaseTest ScenePrimPathKnobSpecTest;

// Every plug-in that declares a prim path knob makes a node with that knob, flagged as a prim
// path or pattern, which the tab menu's scene filter and the selection actions rely on.
TEST_F(ScenePrimPathKnobSpecTest, DeclaredKnobsAreHintedStringKnobs)
{
    int declared = 0;
    const PluginsMap& plugins = appPTR->getPluginsList();

    for (PluginsMap::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
        for (Plugin* plugin : it->second) {
            if (!plugin || plugin->getPrimPathKnob().empty()) {
                continue;
            }
            ++declared;
            NodePtr node = createNode(plugin->getPluginID(), plugin->getMajorVersion(), plugin->getMinorVersion());
            ASSERT_TRUE(bool(node)) << plugin->getPluginID().toStdString();
            KnobStringPtr knob = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(plugin->getPrimPathKnob()));
            ASSERT_TRUE(bool(knob)) << plugin->getPluginID().toStdString() << " has no string knob " << plugin->getPrimPathKnob();
            EXPECT_NE(KnobString::ePrimPathHintNone, knob->getPrimPathHint()) << plugin->getPluginID().toStdString();
        }
    }
    EXPECT_GE(declared, 2);
}

TEST_F(ScenePrimPathKnobSpecTest, PluginsKnowTheirFirstInputsKind)
{
    struct Expected {
        const char* id;
        bool hasInput;
        DataKindEnum kind;
        const char* primPathKnob;
    };
    const Expected expected[] = {
        { PLUGINID_NATRON_USDPRUNE, true, eDataKindScene, kUSDPruneParamPrimPattern },
        { PLUGINID_NATRON_USDEDITTRANSFORM, true, eDataKindScene, kUSDEditTransformParamPrimPattern },
        { PLUGINID_NATRON_USDXFORMPRIM, true, eDataKindScene, kSceneParamPrimPath },
        { PLUGINID_NATRON_USDMERGE, true, eDataKindScene, "" },
        { PLUGINID_NATRON_USDCAMERA, false, eDataKindImage, kSceneParamPrimPath },
    };

    for (const Expected& e : expected) {
        Plugin* plugin = appPTR->getPluginBinary(QString::fromUtf8(e.id), -1, -1, false);
        ASSERT_TRUE(plugin != nullptr) << e.id;
        DataKindEnum kind = eDataKindImage;
        EXPECT_EQ(e.hasInput, plugin->getFirstInputDataKind(&kind)) << e.id;
        if (e.hasInput) {
            EXPECT_EQ(e.kind, kind) << e.id;
        }
        EXPECT_EQ(std::string(e.primPathKnob), plugin->getPrimPathKnob()) << e.id;
    }
}
