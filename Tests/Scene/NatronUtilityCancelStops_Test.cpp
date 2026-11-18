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

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/types.h>
#include <pxr/usd/sdf/path.h>

#include "Global/Macros.h"

#include <atomic>
#include <memory>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/Utility/UtilityScene.h"

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {
GfMatrix4d
translation(double x)
{
    return GfMatrix4d(1.).SetTranslate(GfVec3d(x, 0., 0.));
}

// A unit stripe whose transform goes from x0 at time t0 to x1 at time t1.
UtilityScene::Snapshot
movingStripe(double t0,
             double x0,
             double t1,
             double x1)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/stripe"), 1);
    std::shared_ptr<UtilityScene::TransformMotion> motion = std::make_shared<UtilityScene::TransformMotion>();

    scene.setTopology(id, VtVec3fArray { GfVec3f(0.f, -50.f, 0.f), GfVec3f(1.f, -50.f, 0.f), GfVec3f(1.f, 50.f, 0.f), GfVec3f(0.f, 50.f, 0.f) }, VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) });
    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });
    scene.setInstances(id, { GfMatrix4d(1.) });
    motion->times = { t0, t1 };
    motion->transforms = { { translation(x0) }, { translation(x1) } };
    scene.setTransformMotion(id, motion);

    return scene.snapshot();
}
} // anon namespace

TEST(NatronUtilityCancelStops, AStoppedEvaluationEndsEarly)
{
    const UtilityScene::Snapshot snapshot = movingStripe(-0.25, -1., 0.25, 3.);
    const std::atomic<bool> stop(true);
    UtilityScene::Snapshot evaluated;

    ASSERT_TRUE(UtilityScene::moves(snapshot));
    EXPECT_FALSE(UtilityScene::evaluateAt(snapshot, 0.125, stop, &evaluated));
}

TEST(NatronUtilityCancelStops, AnUnstoppedEvaluationMatchesThePlainOne)
{
    const UtilityScene::Snapshot snapshot = movingStripe(-0.25, -1., 0.25, 3.);
    const std::atomic<bool> stop(false);
    UtilityScene::Snapshot evaluated;

    ASSERT_TRUE(UtilityScene::evaluateAt(snapshot, 0.125, stop, &evaluated));
    ASSERT_EQ(1u, evaluated.instances.size());
    EXPECT_NEAR(2., evaluated.instances[0].transform.ExtractTranslation()[0], 1e-9);
    EXPECT_NEAR(2., UtilityScene::evaluateAt(snapshot, 0.125).instances[0].transform.ExtractTranslation()[0], 1e-9);
}
