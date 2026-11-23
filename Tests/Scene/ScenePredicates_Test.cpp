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
#include "Engine/Nodes/Scene/ScenePredicates.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kPurposes = R"(#usda 1.0
def Xform "World"
{
    def Xform "Proxy"
    {
        uniform token purpose = "proxy"

        def Mesh "Mesh"
        {
        }

        def "Group"
        {
            def Mesh "Geo"
            {
            }
        }
    }

    def Mesh "Guide"
    {
        uniform token purpose = "guide"
    }

    def Xform "Render"
    {
        uniform token purpose = "render"
    }

    def Scope "Plain"
    {
    }

    def "Typeless"
    {
        uniform token purpose = "proxy"
    }
}
)";

PXR_NS::UsdStageRefPtr
purposeStage()
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateInMemory();

    if (!stage->GetRootLayer()->ImportFromString(kPurposes)) {
        ADD_FAILURE() << "cannot read the fixture";
    }

    return stage;
}

typedef std::vector<std::string> Paths;

// The prims expression selects on stage, as path strings, or "error" when it fails.
Paths
select(const PXR_NS::UsdStageRefPtr& stage,
       const std::string& expression)
{
    std::vector<PXR_NS::SdfPath> paths;
    std::string error;

    if (!SceneOps::selectPrims(stage, expression, &paths, &error)) {
        EXPECT_FALSE(error.empty());

        return Paths(1, "error");
    }
    Paths result;
    for (const PXR_NS::SdfPath& path : paths) {
        result.push_back(path.GetString());
    }

    return result;
}
} // anonymous namespace

TEST(ScenePredicates, PurposeMatchesTheComputedPurposeOfImageablePrims)
{
    PXR_NS::UsdStageRefPtr stage = purposeStage();

    // A purpose authored on a prim that is not imageable is not one, and a prim that is not
    // imageable passes its parent's on to the prims below.
    EXPECT_EQ(Paths({ "/World/Proxy", "/World/Proxy/Mesh", "/World/Proxy/Group/Geo" }), select(stage, "//{purpose:proxy}"));
    EXPECT_EQ(Paths({ "/World/Guide" }), select(stage, "//{purpose:guide}"));
    EXPECT_EQ(Paths({ "/World/Render" }), select(stage, "//{purpose:render}"));
    EXPECT_EQ(Paths({ "/World", "/World/Plain" }), select(stage, "//{purpose:default}"));
    EXPECT_EQ(Paths(), select(stage, "//{purpose:nothing}"));
}

TEST(ScenePredicates, MatchesWhatUsdComputes)
{
    PXR_NS::UsdStageRefPtr stage = purposeStage();
    const PXR_NS::TfToken purposes[] = { PXR_NS::UsdGeomTokens->default_, PXR_NS::UsdGeomTokens->render, PXR_NS::UsdGeomTokens->proxy, PXR_NS::UsdGeomTokens->guide };

    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        const bool imageable = prim.IsA<PXR_NS::UsdGeomImageable>();
        for (const PXR_NS::TfToken& purpose : purposes) {
            const bool expected = imageable && (PXR_NS::UsdGeomImageable(prim).ComputePurposeInfo().purpose == purpose);
            EXPECT_EQ(expected, ScenePredicates::hasPurpose(prim, purpose)) << prim.GetPath().GetString() << " " << purpose.GetString();
        }
    }
}

TEST(ScenePredicates, KeepsTheCollectionPredicates)
{
    PXR_NS::UsdStageRefPtr stage = purposeStage();

    EXPECT_EQ(Paths({ "/World/Proxy/Mesh", "/World/Proxy/Group/Geo", "/World/Guide" }), select(stage, "//{isa:Mesh}"));
}

TEST(ScenePredicates, ABarePurposeOrAViewerStateIsAnError)
{
    PXR_NS::UsdStageRefPtr stage = purposeStage();

    EXPECT_EQ(Paths(1, "error"), select(stage, "//{purpose}"));
    EXPECT_EQ(Paths(1, "error"), select(stage, "//{loaded:false}"));
    EXPECT_EQ(Paths(1, "error"), select(stage, "//{active:false}"));
}
