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

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>

#include "Engine/Nodes/Scene/UsdIncludes.h"
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/tokens.h>

#include "Global/Macros.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"

NATRON_NAMESPACE_USING
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const int kRefineWidth = 256;
const int kRefineHeight = 192;
// Enough passes that the frame is still refining when its first one is shown.
const int kRefineSamples = 256;

void
defineTriangle(const UsdStageRefPtr& stage,
               const char* path,
               const GfVec3f& a,
               const GfVec3f& b,
               const GfVec3f& c)
{
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath(path));

    mesh.CreatePointsAttr(VtValue(VtVec3fArray { a, b, c }));
    mesh.CreateFaceVertexCountsAttr(VtValue(VtIntArray { 3 }));
    mesh.CreateFaceVertexIndicesAttr(VtValue(VtIntArray { 0, 1, 2 }));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
}

// A triangle whose slanted edges every pass antialiases a little differently, on the left or on the right of the
// frame.
SdfLayerRefPtr
triangleLayer(bool onTheRight)
{
    SdfLayerRefPtr layer = SceneOps::createNodeLayer(onTheRight ? "NatronUtilityRefineRight" : "NatronUtilityRefineLeft");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    const float x = onTheRight ? 1.5f : -1.5f;

    defineTriangle(stage, onTheRight ? "/Right" : "/Left", GfVec3f(x - 1.2f, -1.1f, -10.f), GfVec3f(x + 1.3f, -0.9f, -10.f), GfVec3f(x + 0.1f, 1.4f, -10.f));

    return layer;
}

GfFrustum
refineCamera()
{
    GfFrustum camera;

    camera.SetPerspective(2. * std::atan(3. / 10.) * 180. / M_PI, false, static_cast<double>(kRefineWidth) / kRefineHeight, 1., 100.);
    camera.SetPosition(GfVec3d(0.));

    return camera;
}

// The scenes are made once, as a node's output is: a payload made again would be a stage edit to the engine.
const ScenePayload&
triangleScene(bool onTheRight)
{
    // Never destroyed: the layers must not outlive USD's teardown at exit.
    static const ScenePayload* const left = new ScenePayload(SceneOps::appendLayer(ScenePayload(), triangleLayer(false), 1));
    static const ScenePayload* const right = new ScenePayload(SceneOps::appendLayer(ScenePayload(), triangleLayer(true), 1));

    return onTheRight ? *right : *left;
}

HydraRender::Request
refineRequest(const void* owner,
              bool onTheRight,
              bool refine,
              std::uint64_t sequence)
{
    HydraRender::Request request;

    request.owner = owner;
    request.scene = triangleScene(onTheRight);
    request.time = UsdTimeCode(1.);
    request.width = kRefineWidth;
    request.height = kRefineHeight;
    request.renderer = kRenderDelegateUtilityId;
    request.frustum = refineCamera();
    request.settings[kUtilitySettingDevice] = VtValue(TfToken(kUtilityDeviceCpu));
    request.settings[kUtilitySettingPixelSamples] = VtValue(kRefineSamples);
    request.interactive = true;
    request.refine = refine;
    request.sequence = sequence;

    return request;
}

std::string
pathAt(const HydraRender::Result& result,
       int x,
       int y)
{
    const float id = result.primId[static_cast<std::size_t>(y * kRefineWidth + x)];

    for (const auto& entry : result.primPaths) {
        if (HydraRender::hashToFloat(entry.first) == id) {
            return entry.second;
        }
    }

    return std::string();
}

// Asks with a new sequence each time until the frame converges, at most maxRequests times.
bool
refineToConvergence(const HydraRender::Request& first,
                    int maxRequests,
                    std::uint64_t* sequence,
                    HydraRender::Result* last,
                    std::string* error)
{
    HydraRender::Request request = first;

    for (int i = 0; i < maxRequests; ++i) {
        request.sequence = ++*sequence;
        *last = HydraRender::Result();
        if (!HydraRender::render(request, last, error)) {
            return false;
        }
        if (!last->partial) {
            return true;
        }
    }
    *error = "the frame did not converge";

    return false;
}
} // anon namespace

TEST(NatronUtilityRefineInfo, RefinementBitsStartUnsetAndStaySet)
{
    const AbortableRenderInfoPtr info = AbortableRenderInfo::create(true, 0);

    EXPECT_FALSE(info->acceptsRefinement());
    EXPECT_FALSE(info->isRefining());
    info->setAcceptsRefinement();
    info->markRefining();
    EXPECT_TRUE(info->acceptsRefinement());
    EXPECT_TRUE(info->isRefining());
    info->setSuperseded();
    EXPECT_TRUE(info->isRefining());
}

// A second window of one request takes the frame as the first left it; the next request takes the frame on until it
// converges.
TEST(NatronUtilityRefine, OneRequestSeesOnePassAndTheNextAdvancesIt)
{
    static const int owner = 0;
    std::uint64_t sequence = 1;
    std::string error;
    HydraRender::Result first;

    ASSERT_TRUE(HydraRender::render(refineRequest(&owner, false, true, sequence), &first, &error)) << error;
    ASSERT_TRUE(first.partial) << "the first pass shown should leave the frame refining";

    HydraRender::Result sibling;

    ASSERT_TRUE(HydraRender::render(refineRequest(&owner, false, true, sequence), &sibling, &error)) << error;
    EXPECT_TRUE(sibling.partial) << "a second window of the same request waited for another pass";

    HydraRender::Result next;

    ASSERT_TRUE(HydraRender::render(refineRequest(&owner, false, true, ++sequence), &next, &error)) << error;
    EXPECT_FALSE(first.rgba == next.rgba) << "the next request did not advance the frame";

    HydraRender::Result converged;

    ASSERT_TRUE(refineToConvergence(refineRequest(&owner, false, true, sequence), 2 * kRefineSamples, &sequence, &converged, &error)) << error;
    EXPECT_FALSE(converged.partial);

    HydraRender::Result again;

    ASSERT_TRUE(HydraRender::render(refineRequest(&owner, false, true, ++sequence), &again, &error)) << error;
    EXPECT_FALSE(again.partial);
    EXPECT_TRUE(converged.rgba == again.rgba) << "a converged frame changed when asked for again";
}

// A caller that does not refine, as playback or a writer, waits for the frame to converge even after a pass was shown.
TEST(NatronUtilityRefine, ARequestThatDoesNotRefineWaitsForConvergence)
{
    static const int owner = 0;
    std::string error;
    HydraRender::Result partial;

    ASSERT_TRUE(HydraRender::render(refineRequest(&owner, false, true, 1), &partial, &error)) << error;
    ASSERT_TRUE(partial.partial);

    HydraRender::Result whole;

    ASSERT_TRUE(HydraRender::render(refineRequest(&owner, false, false, 2), &whole, &error)) << error;
    EXPECT_FALSE(whole.partial);

    HydraRender::Result fresh;
    HydraRender::Request plain = refineRequest(&owner, true, false, 0);

    plain.interactive = false;
    ASSERT_TRUE(HydraRender::render(plain, &fresh, &error)) << error;
    EXPECT_FALSE(fresh.partial);
}

// An edit while the frame refines shows the edited scene from its first pass, never a pass of the frame before.
TEST(NatronUtilityRefine, AnEditDuringRefinementShowsNoStalePass)
{
    static const int owner = 0;
    std::string error;
    HydraRender::Result before;

    ASSERT_TRUE(HydraRender::render(refineRequest(&owner, false, true, 1), &before, &error)) << error;
    ASSERT_TRUE(before.partial);
    ASSERT_EQ(std::string("/Left"), pathAt(before, kRefineWidth / 4, kRefineHeight / 2));

    HydraRender::Result edited;

    ASSERT_TRUE(HydraRender::render(refineRequest(&owner, true, true, 2), &edited, &error)) << error;
    EXPECT_EQ(std::string(), pathAt(edited, kRefineWidth / 4, kRefineHeight / 2)) << "the edit showed the frame before it";
    EXPECT_EQ(std::string("/Right"), pathAt(edited, 3 * kRefineWidth / 4, kRefineHeight / 2));
}
