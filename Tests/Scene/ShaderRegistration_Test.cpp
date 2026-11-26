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
#include "Engine/Node.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"

#include "Scene/ShaderManifestTestUtils.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

struct RendererGroup {
    const char* renderer;
    const char* grouping;
};

const RendererGroup kRenderers[] = {
    { "cycles", "Shader/Cycles" },
    { "moonray", "Shader/MoonRay" },
};

class ShaderRegistrationTest
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

} // namespace

TEST_F(ShaderRegistrationTest, EveryManifestEntryIsAPluginOutOfTheMenus)
{
    for (const RendererGroup& entry : kRenderers) {
        const std::vector<std::string> ids = shaderManifestIds(entry.renderer);
        ASSERT_FALSE(ids.empty()) << entry.renderer;
        for (const std::string& id : ids) {
            Plugin* plugin = nullptr;
            try {
                plugin = appPTR->getPluginBinary(QString::fromUtf8(id.c_str()), -1, -1, false);
            } catch (const std::exception& e) {
                ADD_FAILURE() << id << ": " << e.what();
                continue;
            }
            ASSERT_TRUE(plugin) << id;
            EXPECT_EQ(QString::fromUtf8(entry.grouping), plugin->getGrouping().join(QLatin1Char('/'))) << id;
            // The Nodes menu, the toolbar and the main graph's Tab menu list user-creatable plug-ins only.
            EXPECT_FALSE(plugin->getIsUserCreatable()) << id;
            EXPECT_TRUE(plugin->getIsForInternalUseOnly()) << id;
        }
    }
}

TEST_F(ShaderRegistrationTest, EveryManifestEntryCreatesANodeById)
{
    for (const RendererGroup& entry : kRenderers) {
        for (const std::string& id : shaderManifestIds(entry.renderer)) {
            NodePtr node = createShaderNode(getApp(), id, getApp()->getProject());
            ASSERT_TRUE(bool(node)) << id;
            NetworkNode* effect = shaderEffect(node);
            ASSERT_TRUE(effect) << id;
            EXPECT_EQ(id, node->getPluginID()) << id;
            EXPECT_EQ(eDataKindNetwork, node->getEffectiveOutputDataKind()) << id;
            EXPECT_EQ(shaderManifestNode(entry.renderer, id).value(QLatin1String("outputs")).toArray().size(), (int)node->getOutputPorts().size()) << id;
            node->destroyNode(true, false);
        }
    }
}

TEST_F(ShaderRegistrationTest, PythonCreatesAShaderById)
{
    const std::string app = getApp()->getAppIDString();
    std::string script = "failed = []\n"
                         "for shaderId in ['cycles_principled_bsdf', 'cycles_image_texture', 'DwaBaseMaterial', 'ImageMap']:\n"
                         "    if "
        + app + ".createNode(shaderId) is None:\n"
                "        failed.append(shaderId)\n"
                "if failed:\n"
                "    raise RuntimeError('not created: ' + ' '.join(failed))\n";
    std::string error;
    std::string output;

    ASSERT_TRUE(interpretPythonScript(script, &error, &output)) << error;
    EXPECT_TRUE(error.empty()) << error;
}
