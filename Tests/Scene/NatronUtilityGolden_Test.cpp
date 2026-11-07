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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "RenderSettingsTestUtils.h"
#include "Tests/Native/NativeGolden.h"
#include "ofxNatron.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";
const char* const kGroup = "NatronUtility";

class NatronUtilityGoldenTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        HydraRender::releaseAll();
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    // USDRead of the reference scene, through a USDRenderSettings framing its camera at the golden
    // window's size, into a Natron Utility USDRender.
    NodePtr createRender()
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));

        if (!reader || !settings || !render) {
            ADD_FAILURE() << "USDRead, USDRenderSettings or USDRender is not registered";

            return NodePtr();
        }
        KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
        KnobChoice* renderer = dynamic_cast<KnobChoice*>(render->getKnobByName(kUSDRenderParamRenderer).get());

        if (!file || !renderer) {
            ADD_FAILURE() << "missing knobs";

            return NodePtr();
        }
        file->setValue(std::string(kFixture));
        if (!RenderSettingsTestUtils::configure(settings, "/World/Cam", kGoldenWidth, kGoldenHeight)) {
            return NodePtr();
        }

        const std::vector<ChoiceOption> entries = renderer->getEntries_mt_safe();
        bool selected = false;

        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == kRenderDelegateUtilityId) {
                renderer->setValue((int)i);
                selected = true;
            }
        }
        if (!selected) {
            ADD_FAILURE() << "USDRender does not list Natron Utility";

            return NodePtr();
        }
        connectNodes(reader, settings, 0, true);
        connectNodes(settings, render, 0, true);

        return render;
    }
};
} // anon namespace

TEST_F(NatronUtilityGoldenTest, ReferenceScene)
{
    NodePtr render = createRender();

    ASSERT_TRUE(render != nullptr);
    EXPECT_TRUE(checkGolden(render, kGroup, "reference_f1", 0, 1.));
    EXPECT_TRUE(checkGolden(render, kGroup, "reference_f24", 0, 24.));
}
