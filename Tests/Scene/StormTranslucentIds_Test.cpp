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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/ViewIdx.h"
#include "ProjectionTestUtils.h"
#include "RenderBothWays.h"

NATRON_NAMESPACE_USING

using namespace ProjectionTestUtils;

namespace {
// stormScene(): 200 x 100, ten pixels a unit on the card's plane, so the 16 x 8 card covers
// pixels 20 to 180 by 10 to 90 and the projector's frame, cropped, 50 to 150 by 25 to 75.
const int kWidth = 200;
const int kHeight = 100;

// The dev image's /usr/local glvnd lists only NVIDIA; the system directory holds Mesa. Set before
// the first EGL use, as the render thread's context is made once.
void
pointEGLAtMesa()
{
    if (!std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") && !std::getenv("__EGL_VENDOR_LIBRARY_DIRS") && (::access("/usr/share/glvnd/egl_vendor.d", F_OK) == 0) && (::access("/usr/local/share/glvnd/egl_vendor.d", F_OK) == 0)) {
        ::setenv("__EGL_VENDOR_LIBRARY_DIRS", "/usr/share/glvnd/egl_vendor.d", 1);
    }
}

// True when the test must stop: no 4.5 context could be made, which fails the test only under
// NATRON_TEST_REQUIRE_GL=1.
bool
skipWithoutGL()
{
    pointEGLAtMesa();
    std::string error;
    if (!HydraRender::contextDescription(&error).empty()) {
        return false;
    }
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << error << " (NATRON_TEST_REQUIRE_GL=1)";

        return true;
    }
    std::cout << "[  SKIPPED ] StormTranslucentIds: " << error << std::endl;

    return true;
}

std::uint32_t
bitsOf(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    return bits;
}

std::uint32_t
idOf(const char* path)
{
    return bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash(path)));
}

// The planes of one render, each as its bits.
struct Planes {
    std::vector<std::uint32_t> rgba;
    std::vector<std::uint32_t> depth;
    std::vector<std::uint32_t> primId;

    std::uint32_t idAt(int x,
                       int y) const
    {
        return primId[(std::size_t)y * (std::size_t)kWidth + (std::size_t)x];
    }
};

class StormTranslucentIdsBase
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        HydraRender::setStormTranslucentIds(true);
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

class StormTranslucentIdsTest
    : public ProjectionFixture<StormTranslucentIdsBase> {
protected:
    virtual const char* rendererId() const OVERRIDE
    {
        return kRenderDelegateStormId;
    }

    // An opaque card at /Back behind the projected one, filling the frame.
    NodePtr addBackCard(const Graph& graph)
    {
        NodePtr back = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        if (!back || !setString(back, kSceneParamPrimPath, "/Back") || !setTriple(back, kSceneParamTranslate, 0., 0., -1.) || !setTriple(back, kSceneParamScale, 30., 15., 1.)) {
            ADD_FAILURE() << "no /Back card";

            return NodePtr();
        }
        this->connectNodes(back, graph.merge, 2, true);

        return back;
    }

    /**
     * Renders colour, depth and primId with the translucent id pass on or off. The engines go
     * first, so that no ids decoded by an earlier render are kept; off the main thread, which is
     * where the plate's live texture is prepared from.
     **/
    bool renderPlanes(const NodePtr& render,
                      bool translucentIds,
                      Planes* out)
    {
        HydraRender::setStormTranslucentIds(translucentIds);
        HydraRender::releaseAll();
        appPTR->clearAllCaches();
        std::list<ImageLayerDesc> layers;
        layers.push_back(ImageLayerDesc::getRGBAComponents());
        layers.push_back(ImageLayerDesc("depth", std::vector<std::string>(1, "Z")));
        layers.push_back(ImageLayerDesc("primId", std::vector<std::string>(1, "id")));
        bool rendered = false;
        std::string error;
        std::vector<RenderedPlane> planes;
        std::thread worker([&]() {
            rendered = renderNodePlanesDirect(render, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), layers, &planes, &error, false);
        });
        worker.join();
        appPTR->clearAllCaches();
        EXPECT_TRUE(rendered) << error;
        const std::size_t pixels = (std::size_t)kWidth * (std::size_t)kHeight;
        if (!rendered || (planes.size() != 3u) || (planes[0].pixels.size() != pixels * 4) || (planes[1].pixels.size() != pixels) || (planes[2].pixels.size() != pixels)) {
            ADD_FAILURE() << "no " << kWidth << " x " << kHeight << " colour, depth and primId planes";

            return false;
        }
        std::vector<std::uint32_t>* const bits[3] = { &out->rgba, &out->depth, &out->primId };
        for (int p = 0; p < 3; ++p) {
            bits[p]->resize(planes[p].pixels.size());
            for (std::size_t i = 0; i < planes[p].pixels.size(); ++i) {
                (*bits[p])[i] = bitsOf(planes[p].pixels[i]);
            }
        }

        return true;
    }
};
} // anonymous namespace

// The projected card picks where its plate shows, the opaque card behind it everywhere else.
TEST_F(StormTranslucentIdsTest, AProjectedCardWritesItsIdOverTheOpaqueCard)
{
    if (skipWithoutGL()) {
        return;
    }
    const Graph graph = createProjectionGraph(stormScene());
    ASSERT_TRUE(bool(graph.render));
    ASSERT_TRUE(addBackCard(graph) != nullptr);

    Planes planes;
    ASSERT_TRUE(renderPlanes(graph.render, true, &planes));
    EXPECT_FALSE(graph.render->hasPersistentMessage()) << messageOf(graph.render);
    const std::uint32_t card = idOf("/Card");
    const std::uint32_t back = idOf("/Back");
    // Inside the projector's frame.
    EXPECT_EQ(card, planes.idAt(100, 50));
    EXPECT_EQ(card, planes.idAt(60, 30));
    EXPECT_EQ(card, planes.idAt(140, 70));
    // On the card but beyond the cropped frame, where the material discards its fragments.
    EXPECT_EQ(back, planes.idAt(30, 50));
    EXPECT_EQ(back, planes.idAt(100, 15));
    // Off the card.
    EXPECT_EQ(back, planes.idAt(5, 50));
    EXPECT_EQ(back, planes.idAt(100, 95));

    // Without the pass, Storm leaves the projected card out of primId.
    Planes storm;
    ASSERT_TRUE(renderPlanes(graph.render, false, &storm));
    EXPECT_EQ(back, storm.idAt(100, 50));
    EXPECT_EQ(back, storm.idAt(30, 50));
    // The pass writes ids alone: colour and depth are Storm's.
    EXPECT_TRUE(planes.rgba == storm.rgba);
    EXPECT_TRUE(planes.depth == storm.depth);
}

// Of two translucent prims over the same pixels, the nearer keeps its id whatever their order.
TEST_F(StormTranslucentIdsTest, TheNearestTranslucentPrimWins)
{
    if (skipWithoutGL()) {
        return;
    }
    const Graph graph = createProjectionGraph(stormScene());
    ASSERT_TRUE(bool(graph.render));
    ASSERT_TRUE(addBackCard(graph) != nullptr);
    // 4 x 2 units at z 0.5, about pixels 79 to 121 by 39 to 61, inside the projector's frame there.
    NodePtr front = this->createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(front != nullptr);
    ASSERT_TRUE(setString(front, kSceneParamPrimPath, "/Front"));
    ASSERT_TRUE(setTriple(front, kSceneParamTranslate, 0., 0., 0.5));
    ASSERT_TRUE(setTriple(front, kSceneParamScale, 4., 2., 1.));
    this->connectNodes(graph.projector, front, 0, true);
    this->connectNodes(front, graph.merge, 3, true);

    Planes planes;
    ASSERT_TRUE(renderPlanes(graph.render, true, &planes));
    EXPECT_FALSE(graph.render->hasPersistentMessage()) << messageOf(graph.render);
    EXPECT_EQ(idOf("/Front"), planes.idAt(100, 50));
    EXPECT_EQ(idOf("/Card"), planes.idAt(65, 50));
    EXPECT_EQ(idOf("/Back"), planes.idAt(30, 50));
}

// Without a translucent prim the pass draws nothing: every plane is Storm's, bit for bit.
TEST_F(StormTranslucentIdsTest, AnOpaqueSceneIsUnchangedByThePass)
{
    if (skipWithoutGL()) {
        return;
    }
    const Graph graph = createProjectionGraph(stormScene());
    ASSERT_TRUE(bool(graph.render));
    ASSERT_TRUE(addBackCard(graph) != nullptr);
    graph.card->disconnectInput(0);

    Planes withPass;
    ASSERT_TRUE(renderPlanes(graph.render, true, &withPass));
    Planes without;
    ASSERT_TRUE(renderPlanes(graph.render, false, &without));
    EXPECT_EQ(idOf("/Card"), withPass.idAt(100, 50));
    EXPECT_EQ(idOf("/Back"), withPass.idAt(5, 50));
    EXPECT_TRUE(withPass.primId == without.primId);
    EXPECT_TRUE(withPass.rgba == without.rgba);
    EXPECT_TRUE(withPass.depth == without.depth);
}
