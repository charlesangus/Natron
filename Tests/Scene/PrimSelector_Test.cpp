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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneOps.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";

// The prims expression selects on stage, as path strings, or "error: ..." when it fails.
std::vector<std::string>
select(const PXR_NS::UsdStageRefPtr& stage,
       const std::string& expression)
{
    std::vector<PXR_NS::SdfPath> paths;
    std::string error;

    if (!SceneOps::selectPrims(stage, expression, &paths, &error)) {
        return std::vector<std::string>(1, "error: " + error);
    }
    std::vector<std::string> result;
    for (const PXR_NS::SdfPath& path : paths) {
        result.push_back(path.GetString());
    }

    return result;
}

typedef std::vector<std::string> Paths;
} // anonymous namespace

TEST(PrimSelector, AnAbsolutePathSelectsThatPrim)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(bool(stage));

    EXPECT_EQ(Paths({ "/World/Box" }), select(stage, "/World/Box"));
    EXPECT_EQ(Paths({ "/World" }), select(stage, "/World"));
    EXPECT_EQ(Paths(), select(stage, "/World/Nothing"));
}

TEST(PrimSelector, WildcardsMatchNamesAtOneLevel)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(bool(stage));

    EXPECT_EQ(Paths({ "/World/Box", "/World/Ball", "/World/Cam" }), select(stage, "/World/*"));
    EXPECT_EQ(Paths({ "/World/Box", "/World/Ball" }), select(stage, "/World/B*"));
    EXPECT_EQ(Paths({ "/World/Box" }), select(stage, "/World/B?x"));
    EXPECT_EQ(Paths({ "/World" }), select(stage, "/*"));
}

TEST(PrimSelector, DescendantPatternsMatchAtAnyDepth)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(bool(stage));

    EXPECT_EQ(Paths({ "/World", "/World/Box", "/World/Ball", "/World/Cam" }), select(stage, "//*"));
    EXPECT_EQ(Paths({ "/World/Cam" }), select(stage, "//Ca*"));
    EXPECT_EQ(Paths({ "/World/Box", "/World/Ball" }), select(stage, "//*B*"));
    EXPECT_EQ(Paths({ "/World/Box", "/World/Ball", "/World/Cam" }), select(stage, "/World//*"));
    EXPECT_EQ(Paths({ "/World/Cam" }), select(stage, "//{isa:Camera}"));
}

TEST(PrimSelector, SetOperatorsCombineSelections)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(bool(stage));

    // The space-separated form scene nodes have always taken is an implied union. Results come
    // in stage order, whatever order the expression names them in.
    EXPECT_EQ(Paths({ "/World/Box", "/World/Cam" }), select(stage, "/World/Cam /World/Box"));
    EXPECT_EQ(Paths({ "/World/Box", "/World/Cam" }), select(stage, "/World/Box + /World/Cam"));
    EXPECT_EQ(Paths({ "/World/Box", "/World/Cam" }), select(stage, "/World/* - /World/Ball"));
    EXPECT_EQ(Paths({ "/World/Ball" }), select(stage, "/World/B* & //*l*"));
    // A prim named twice is listed once.
    EXPECT_EQ(Paths({ "/World/Box", "/World/Ball" }), select(stage, "/World/B* /World/Box"));
}

TEST(PrimSelector, InactivePrimsAndTheirDescendantsDoNotMatch)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(bool(stage));
    // In the session layer, so the fixture's layer, shared with other tests, stays as read.
    stage->SetEditTarget(stage->GetSessionLayer());
    PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/World/Ball/Child"));
    stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball")).SetActive(false);

    EXPECT_EQ(Paths({ "/World/Box", "/World/Cam" }), select(stage, "/World/*"));
    EXPECT_EQ(Paths(), select(stage, "/World/Ball/Child"));
}

TEST(PrimSelector, BlankSelectsNothing)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(bool(stage));

    EXPECT_EQ(Paths(), select(stage, ""));
    EXPECT_EQ(Paths(), select(stage, "  \t "));
}

TEST(PrimSelector, InvalidExpressionsAreErrorsNamingTheProblem)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kFixture);
    ASSERT_TRUE(bool(stage));

    for (const char* bad : { "/World/Box &", "/World/(Box", "World/Box", "%/World:things" }) {
        std::vector<PXR_NS::SdfPath> paths(1, PXR_NS::SdfPath("/stale"));
        std::string error;
        EXPECT_FALSE(SceneOps::selectPrims(stage, bad, &paths, &error)) << bad;
        EXPECT_TRUE(paths.empty()) << bad;
        EXPECT_NE(std::string::npos, error.find(bad)) << bad << ": " << error;
    }

    // Parsing alone needs no stage.
    PXR_NS::SdfPathExpression expression;
    std::string error;
    EXPECT_FALSE(SceneOps::parsePrimExpression("/World/Box &", &expression, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(expression.IsEmpty());
    error.clear();
    EXPECT_TRUE(SceneOps::parsePrimExpression("/World/Box //Cam", &expression, &error)) << error;
    EXPECT_FALSE(expression.IsEmpty());
}
