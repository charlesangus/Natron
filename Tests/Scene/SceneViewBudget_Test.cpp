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

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneViewBudget.h"

#ifdef emit
#undef emit
#endif
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rotation.h>

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {
using Budget = SceneViewBudget;

// From eye down -Z, 60 degrees across, out to 1000.
Budget::Camera
cameraAt(const GfVec3d& eye)
{
    GfFrustum frustum;

    frustum.SetPosition(eye);
    frustum.SetPerspective(60., 1., 0.1, 1000.);
    Budget::Camera camera;
    camera.eye = eye;
    camera.viewProjection = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix();
    camera.valid = true;

    return camera;
}

// A 0.2-unit box centred on centre.
Budget::Unit
gprim(const std::string& path,
      const GfVec3f& centre,
      U64 faces,
      Budget::StateEnum state = Budget::eStateDrawn)
{
    Budget::Unit unit;

    unit.path = SdfPath(path);
    unit.min = centre - GfVec3f(0.1f);
    unit.max = centre + GfVec3f(0.1f);
    unit.bounded = true;
    unit.faces = faces;
    unit.state = state;
    unit.mesh = true;

    return unit;
}

// One prototype of protoFaces faces, a 0.2-unit box at its origin, at each position.
Budget::Unit
instancer(const std::string& path,
          const std::vector<GfVec3f>& positions,
          U64 protoFaces)
{
    std::shared_ptr<Budget::Instances> instances = std::make_shared<Budget::Instances>();
    Budget::Prototype prototype;

    prototype.min = GfVec3f(-0.1f);
    prototype.max = GfVec3f(0.1f);
    prototype.bounded = true;
    prototype.faces = protoFaces;
    instances->prototypes.push_back(prototype);
    for (const GfVec3f& position : positions) {
        instances->protoIndices.push_back(0);
        instances->positions.push_back(position);
    }
    Budget::Unit unit;
    unit.path = SdfPath(path);
    unit.bounded = true;
    unit.faces = protoFaces * positions.size();
    unit.instances = instances;
    if (!positions.empty()) {
        unit.min = positions.front();
        unit.max = positions.front();
        for (const GfVec3f& position : positions) {
            for (int axis = 0; axis < 3; ++axis) {
                unit.min[axis] = std::min(unit.min[axis], position[axis] - 0.1f);
                unit.max[axis] = std::max(unit.max[axis], position[axis] + 0.1f);
            }
        }
    }

    return unit;
}

Budget::Settings
limitOf(U64 limit)
{
    Budget::Settings settings;

    settings.limit = limit;

    return settings;
}

const GfVec3d kEye(0, 0, 10);
} // anon namespace

TEST(SceneViewBudget, AnInvalidCameraChangesNothing)
{
    const std::vector<Budget::Unit> units { gprim("/A", GfVec3f(0, 0, 0), 1000) };

    const Budget::Result result = Budget::solve(units, Budget::Camera(), limitOf(10), Budget::Result());
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.demoted.empty());
}

TEST(SceneViewBudget, TheFarthestUnitsAreDemotedFirstUntilTheCountFits)
{
    const std::vector<Budget::Unit> units {
        gprim("/Far", GfVec3f(0, 0, -30), 100),
        gprim("/Near", GfVec3f(0, 0, 0), 100),
        gprim("/Farther", GfVec3f(0, 0, -20), 100),
        gprim("/Middle", GfVec3f(0, 0, -10), 100),
    };

    const Budget::Result result = Budget::solve(units, cameraAt(kEye), limitOf(250), Budget::Result());
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.counted, 400u);
    EXPECT_EQ(result.drawn, 200u);
    EXPECT_EQ(result.demotedCount, 2u);
    EXPECT_TRUE(result.isDemoted(SdfPath("/Far")));
    EXPECT_TRUE(result.isDemoted(SdfPath("/Farther")));
    EXPECT_FALSE(result.isDemoted(SdfPath("/Middle")));
    EXPECT_FALSE(result.isDemoted(SdfPath("/Near")));
    EXPECT_TRUE(result.aloneOverLimit.IsEmpty());

    const Budget::Result roomy = Budget::solve(units, cameraAt(kEye), limitOf(400), Budget::Result());
    EXPECT_TRUE(roomy.demoted.empty());
    EXPECT_EQ(roomy.drawn, 400u);
    EXPECT_TRUE(std::isinf(roomy.demoteDistance));
}

TEST(SceneViewBudget, InstancesAreUnitsOfTheirOwn)
{
    const std::vector<Budget::Unit> units { instancer("/Instancer", { GfVec3f(0, 0, -30), GfVec3f(0, 0, 0), GfVec3f(0, 0, -20), GfVec3f(0, 0, -10) }, 100) };

    const Budget::Result result = Budget::solve(units, cameraAt(kEye), limitOf(250), Budget::Result());
    EXPECT_EQ(result.counted, 400u);
    EXPECT_EQ(result.drawn, 200u);
    EXPECT_EQ(result.demotedCount, 2u);
    EXPECT_TRUE(result.isInstanceDemoted(SdfPath("/Instancer"), 0));
    EXPECT_FALSE(result.isInstanceDemoted(SdfPath("/Instancer"), 1));
    EXPECT_TRUE(result.isInstanceDemoted(SdfPath("/Instancer"), 2));
    EXPECT_FALSE(result.isInstanceDemoted(SdfPath("/Instancer"), 3));
    EXPECT_FALSE(result.isDemoted(SdfPath("/Instancer")));
}

TEST(SceneViewBudget, OnlyUnitsInTheFrustumCountUnlessTheWholeSceneDoes)
{
    const std::vector<Budget::Unit> units {
        gprim("/Near", GfVec3f(0, 0, 0), 100),
        gprim("/Middle", GfVec3f(0, 0, -10), 100),
        // Behind the eye.
        gprim("/Behind", GfVec3f(0, 0, 40), 100),
    };

    Budget::Settings settings = limitOf(250);
    const Budget::Result framed = Budget::solve(units, cameraAt(kEye), settings, Budget::Result());
    EXPECT_EQ(framed.counted, 200u);
    EXPECT_TRUE(framed.demoted.empty());

    settings.countWholeScene = true;
    const Budget::Result whole = Budget::solve(units, cameraAt(kEye), settings, Budget::Result());
    EXPECT_EQ(whole.counted, 300u);
    EXPECT_EQ(whole.demotedCount, 1u);
    EXPECT_TRUE(whole.isDemoted(SdfPath("/Behind")));

    // Turning to face it changes nothing while the distances stay.
    GfFrustum backwards;
    backwards.SetPosition(kEye);
    backwards.SetRotation(GfRotation(GfVec3d(0, 1, 0), 180.));
    backwards.SetPerspective(60., 1., 0.1, 1000.);
    Budget::Camera turned = cameraAt(kEye);
    turned.viewProjection = backwards.ComputeViewMatrix() * backwards.ComputeProjectionMatrix();
    const Budget::Result turnedWhole = Budget::solve(units, turned, settings, Budget::Result());
    EXPECT_EQ(turnedWhole.demoted, whole.demoted);
}

TEST(SceneViewBudget, UnitsBeyondTheCullDistanceAreCulledAndNotCounted)
{
    const std::vector<Budget::Unit> units {
        gprim("/Near", GfVec3f(0, 0, 0), 100),
        gprim("/Far", GfVec3f(0, 0, -20), 100),
        instancer("/Instancer", { GfVec3f(1, 0, 0), GfVec3f(1, 0, -20) }, 10),
    };

    Budget::Settings settings = limitOf(1000);
    settings.cullDistance = 15.;
    const Budget::Result result = Budget::solve(units, cameraAt(kEye), settings, Budget::Result());
    EXPECT_TRUE(result.isCulled(SdfPath("/Far")));
    EXPECT_FALSE(result.isCulled(SdfPath("/Near")));
    EXPECT_FALSE(result.isInstanceCulled(SdfPath("/Instancer"), 0));
    EXPECT_TRUE(result.isInstanceCulled(SdfPath("/Instancer"), 1));
    EXPECT_EQ(result.culledCount, 2u);
    EXPECT_EQ(result.counted, 110u);
    EXPECT_TRUE(result.demoted.empty());
}

TEST(SceneViewBudget, AFractionKeepsTheSameHashedSubsetFromAnyCamera)
{
    std::vector<GfVec3f> positions;
    for (int index = 0; index < 1000; ++index) {
        positions.push_back(GfVec3f(float(index % 40) - 20.f, float(index / 40) - 12.f, -200.f));
    }
    std::vector<Budget::Unit> units { instancer("/Instancer", positions, 10), gprim("/Mesh", GfVec3f(0, 0, -200), 1000) };

    Budget::Settings settings = limitOf(1000000);
    settings.fractionDistance = 50.;
    settings.fractionPercent = 25.;
    const Budget::Result first = Budget::solve(units, cameraAt(kEye), settings, Budget::Result());
    const Budget::Result second = Budget::solve(units, cameraAt(GfVec3d(15, -5, 30)), settings, Budget::Result());

    std::size_t kept = 0;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        const bool keeps = first.isInstanceKept(SdfPath("/Instancer"), index);
        EXPECT_EQ(keeps, second.isInstanceKept(SdfPath("/Instancer"), index)) << index;
        EXPECT_EQ(keeps, Budget::keepsInstance(SdfPath("/Instancer"), index, 25.)) << index;
        kept += keeps ? 1 : 0;
    }
    EXPECT_GT(kept, 180u);
    EXPECT_LT(kept, 320u);
    ASSERT_EQ(first.keptFaceFraction.count(SdfPath("/Mesh")), 1u);
    EXPECT_DOUBLE_EQ(first.keptFaceFraction.at(SdfPath("/Mesh")), 0.25);
    // Fractioned-out instances are budget boxes, which count nothing; the mesh counts its share.
    EXPECT_EQ(first.counted, kept * 10 + 250u);

    // Nearer than the fraction distance everything draws.
    settings.fractionDistance = 500.;
    const Budget::Result near = Budget::solve(units, cameraAt(kEye), settings, Budget::Result());
    EXPECT_TRUE(near.keptFaceFraction.empty());
    EXPECT_TRUE(near.instancers.empty());
}

TEST(SceneViewBudget, ASmallCameraMoveKeepsThePreviousResult)
{
    // /A and /B sit at about the same distance, and which is farther flips as the eye moves 0.5
    // across, under 1% of their distance.
    const std::vector<Budget::Unit> units {
        gprim("/Near", GfVec3f(0, 0, 0), 100),
        gprim("/A", GfVec3f(-1, 0, -50), 100),
        gprim("/B", GfVec3f(1, 0, -50.01f), 100),
    };
    const Budget::Settings settings = limitOf(250);

    const Budget::Result before = Budget::solve(units, cameraAt(kEye), settings, Budget::Result());
    ASSERT_EQ(before.demotedCount, 1u);
    EXPECT_TRUE(before.isDemoted(SdfPath("/B")));

    const Budget::Camera moved = cameraAt(GfVec3d(0.5, 0, 10));
    const Budget::Result fresh = Budget::solve(units, moved, settings, Budget::Result());
    ASSERT_EQ(fresh.demotedCount, 1u);
    EXPECT_TRUE(fresh.isDemoted(SdfPath("/A")));

    const Budget::Result after = Budget::solve(units, moved, settings, before);
    EXPECT_EQ(after.demoted, before.demoted);
    EXPECT_EQ(after.drawn, before.drawn);
}

TEST(SceneViewBudget, ASmallCameraMoveKeepsThePreviousCullAndFraction)
{
    std::vector<GfVec3f> positions;
    for (int index = 0; index < 200; ++index) {
        positions.push_back(GfVec3f(float(index % 20) - 10.f, float(index / 20) - 5.f, -40.f));
    }
    const std::vector<Budget::Unit> units { instancer("/Instancer", positions, 10), gprim("/Edge", GfVec3f(0, 0, -40), 10) };
    Budget::Settings settings = limitOf(1000000);
    settings.cullDistance = 49.7;
    settings.fractionDistance = 49.;

    const Budget::Result before = Budget::solve(units, cameraAt(kEye), settings, Budget::Result());
    const Budget::Result after = Budget::solve(units, cameraAt(GfVec3d(0, 0, 10.5)), settings, before);
    EXPECT_EQ(after.isCulled(SdfPath("/Edge")), before.isCulled(SdfPath("/Edge")));
    EXPECT_EQ(after.keptFaceFraction.size(), before.keptFaceFraction.size());
    for (std::size_t index = 0; index < positions.size(); ++index) {
        EXPECT_EQ(after.isInstanceCulled(SdfPath("/Instancer"), index), before.isInstanceCulled(SdfPath("/Instancer"), index)) << index;
        EXPECT_EQ(after.isInstanceKept(SdfPath("/Instancer"), index), before.isInstanceKept(SdfPath("/Instancer"), index)) << index;
    }
}

TEST(SceneViewBudget, AUnitOverTheLimitAloneIsDemotedAndNamed)
{
    const std::vector<Budget::Unit> units {
        gprim("/Near", GfVec3f(0, 0, 0), 100),
        gprim("/Huge", GfVec3f(0, 0, -10), 1000),
        gprim("/Far", GfVec3f(0, 0, -20), 100),
    };

    const Budget::Result result = Budget::solve(units, cameraAt(kEye), limitOf(500), Budget::Result());
    EXPECT_TRUE(result.isDemoted(SdfPath("/Huge")));
    // Farther than it, so demoted before it.
    EXPECT_TRUE(result.isDemoted(SdfPath("/Far")));
    EXPECT_FALSE(result.isDemoted(SdfPath("/Near")));
    EXPECT_EQ(result.aloneOverLimit, SdfPath("/Huge"));
    EXPECT_FALSE(result.aloneOverLimitIsInstance);
    EXPECT_EQ(result.drawn, 100u);

    // The previous result never holds it back.
    const Budget::Result held = Budget::solve(units, cameraAt(kEye), limitOf(990), Budget::Result());
    const Budget::Result again = Budget::solve(units, cameraAt(kEye), limitOf(990), held);
    EXPECT_TRUE(again.isDemoted(SdfPath("/Huge")));
}

TEST(SceneViewBudget, BoxedHiddenAndUnloadedUnitsAreNeverChangedOrCounted)
{
    const std::vector<Budget::Unit> units {
        gprim("/Boxed", GfVec3f(0, 0, 0), 1000, Budget::eStateBoxed),
        gprim("/Hidden", GfVec3f(0, 0, -1), 1000, Budget::eStateHidden),
        gprim("/Unloaded", GfVec3f(0, 0, -30), 0, Budget::eStateUnloaded),
        gprim("/Drawn", GfVec3f(0, 0, -2), 100),
    };

    Budget::Settings settings = limitOf(50);
    settings.cullDistance = 5.;
    settings.fractionDistance = 1.;
    const Budget::Result result = Budget::solve(units, cameraAt(kEye), settings, Budget::Result());
    EXPECT_EQ(result.counted, 0u);
    for (const char* path : { "/Boxed", "/Hidden", "/Unloaded" }) {
        EXPECT_FALSE(result.isDemoted(SdfPath(path))) << path;
        EXPECT_FALSE(result.isCulled(SdfPath(path))) << path;
        EXPECT_EQ(result.keptFaceFraction.count(SdfPath(path)), 0u) << path;
    }
    EXPECT_TRUE(result.isCulled(SdfPath("/Drawn")));
    EXPECT_EQ(result.culledCount, 1u);
}
