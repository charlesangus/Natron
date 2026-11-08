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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <pxr/base/plug/registry.h>

#include "BaseTest.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
#include "RenderSettingsTestUtils.h"
#include "ofxNatron.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kCyclesPluginId = "HdCyclesPlugin";
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles-mdl-only.usda";
const char* const kCamera = "/World/Cam";
const int kSize = 32;

// The Cycles delegate comes from the build image (tools/ci/local/build-cycles.sh), not from this
// tree, so a build in an older image has none. The bundled gtest has no GTEST_SKIP, so the test
// returns early on false; NATRON_TEST_REQUIRE_CYCLES=1 turns that skip into a failure.
bool
cyclesOrSkip()
{
    if (PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hdCycles")) {
        return true;
    }
    const char* reason = "OpenUSD has no hdCycles plug-in registered; this build image predates the "
                         "Cycles render delegate (tools/ci/local/build-cycles.sh)";
    const char* require = std::getenv("NATRON_TEST_REQUIRE_CYCLES");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << "NATRON_TEST_REQUIRE_CYCLES=1 but " << reason;
    } else {
        std::printf("[  SKIPPED ] %s\n", reason);
    }

    return false;
}

std::size_t
pixelIndex(int x,
           int y)
{
    return (std::size_t)y * (std::size_t)kSize + (std::size_t)x;
}
} // anon namespace

class USDRenderCyclesMaterialTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        HydraRender::releaseAllCpuEngines();
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    template <typename KNOB>
    static KNOB* knob(const NodePtr& node,
                      const char* name)
    {
        KNOB* k = dynamic_cast<KNOB*>(node->getKnobByName(name).get());
        EXPECT_TRUE(k != nullptr) << node->getPluginID() << " has no knob " << name;

        return k;
    }

    static bool selectRenderer(KnobChoice* knob,
                               const std::string& id)
    {
        const std::vector<ChoiceOption> entries = knob->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                knob->setValue((int)i);

                return true;
            }
        }

        return false;
    }
};

// Cycles has no network for a material whose only output is outputs:mdl:surface. The sphere still
// has to render, lit by the grey world Cycles gives a stage with no light.
TEST_F(USDRenderCyclesMaterialTest, MeshRendersWithAMaterialWithNoUniversalSurface)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(read && settings && render);
    KnobFile* file = knob<KnobFile>(read, kUSDReadParamFile);
    KnobChoice* renderer = knob<KnobChoice>(render, kUSDRenderParamRenderer);
    ASSERT_TRUE(file && renderer);
    file->setValue(std::string(kFixture));
    ASSERT_TRUE(selectRenderer(renderer, kCyclesPluginId)) << "the Renderer menu has no " << kCyclesPluginId;
    connectNodes(read, settings, 0, true);
    connectNodes(settings, render, 0, true);
    ASSERT_TRUE(RenderSettingsTestUtils::configure(settings, kCamera, kSize, kSize));

    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(render, 1., ViewIdx(0), 0, RectI(0, 0, kSize, kSize), layers, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    const std::vector<float>& rgba = planes[0].pixels;
    ASSERT_EQ((std::size_t)kSize * (std::size_t)kSize * 4, rgba.size());

    const std::size_t centre = pixelIndex(kSize / 2, kSize / 2);
    EXPECT_GT(rgba[centre * 4] + rgba[centre * 4 + 1] + rgba[centre * 4 + 2], 0.05f);
    EXPECT_NEAR(1.f, rgba[centre * 4 + 3], 1e-3f);
    const std::size_t corner = pixelIndex(0, 0);
    EXPECT_EQ(0.f, rgba[corner * 4 + 3]);
}
