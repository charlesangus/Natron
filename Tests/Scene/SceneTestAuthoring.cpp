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

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "SceneTestAuthoring.h"

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/ViewIdx.h"
#include "SceneTestEffect.h"

NATRON_NAMESPACE_ENTER

namespace {
struct PrimSnapshot {
    std::string typeName;
    bool active;
    std::map<std::string, std::string> properties;
};

std::map<std::string, PrimSnapshot>
snapshot(const PXR_NS::UsdStageRefPtr& stage)
{
    std::map<std::string, PrimSnapshot> prims;
    PXR_NS::UsdPrimRange range = PXR_NS::UsdPrimRange::Stage(stage, PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimAllPrimsPredicate));

    for (PXR_NS::UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
        const PXR_NS::UsdPrim& prim = *it;
        // Below an unloaded payload only stronger layers' overs compose, as prims that are not
        // defined: what an evaluation on everything authored on content the load set leaves out,
        // which an evaluation on the load set cannot see.
        if (prim.HasAuthoredPayloads() && !prim.IsLoaded()) {
            it.PruneChildren();
        }
        PrimSnapshot entry;
        entry.typeName = prim.GetTypeName().GetString();
        entry.active = prim.IsActive();
        for (const PXR_NS::UsdProperty& property : prim.GetProperties()) {
            std::string value;
            if (const PXR_NS::UsdAttribute attribute = property.As<PXR_NS::UsdAttribute>()) {
                PXR_NS::VtValue authored;
                attribute.Get(&authored);
                value = PXR_NS::TfStringify(authored);
            } else if (const PXR_NS::UsdRelationship relationship = property.As<PXR_NS::UsdRelationship>()) {
                PXR_NS::SdfPathVector targets;
                relationship.GetTargets(&targets);
                for (const PXR_NS::SdfPath& target : targets) {
                    value += target.GetString() + ";";
                }
            }
            entry.properties[property.GetName().GetString()] = value;
        }
        prims[prim.GetPath().GetString()] = entry;
    }

    return prims;
}

PXR_NS::UsdStageRefPtr
openLoaded(const ScenePayload& payload,
           const SceneLoadSetPtr& loadSet)
{
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);

    if (stage) {
        stage->SetLoadRules(SceneOps::getLoadRules(loadSet.get()));
    }

    return stage;
}

bool
authorIntAttribute(const SceneLayerEdit& edit,
                   PXR_NS::SdfSpecifier specifier,
                   const char* attributeName,
                   int value,
                   std::string* errorMessage)
{
    PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(edit.layer, PXR_NS::SdfPath(kSceneTestPrimPath));

    if (!prim) {
        *errorMessage = "could not create " kSceneTestPrimPath;

        return false;
    }
    prim->SetSpecifier(specifier);
    PXR_NS::SdfAttributeSpecHandle attribute = PXR_NS::SdfAttributeSpec::New(prim, attributeName, PXR_NS::SdfValueTypeNames->Int);
    if (!attribute) {
        *errorMessage = std::string("could not create ") + attributeName;

        return false;
    }
    attribute->SetDefaultValue(PXR_NS::VtValue(value));

    return true;
}
} // anonymous namespace

bool
sceneTestAuthorSource(const SceneLayerEdit& edit,
                      int value,
                      std::string* errorMessage)
{
    if (!authorIntAttribute(edit, PXR_NS::SdfSpecifierDef, kSceneTestSourceAttribute, value, errorMessage)) {
        return false;
    }
    edit.layer->SetDefaultPrim(PXR_NS::TfToken(std::string(kSceneTestPrimPath).substr(1)));

    return true;
}

bool
sceneTestAuthorOp(const SceneLayerEdit& edit,
                  int value,
                  std::string* errorMessage)
{
    return authorIntAttribute(edit, PXR_NS::SdfSpecifierOver, kSceneTestOpAttribute, value, errorMessage);
}

bool
sceneTestAuthorFactOp(const SceneLayerEdit& edit,
                      int value,
                      int sourceValue,
                      std::string* errorMessage)
{
    return authorIntAttribute(edit, PXR_NS::SdfSpecifierOver, kSceneTestOpAttribute, value, errorMessage)
        && authorIntAttribute(edit, PXR_NS::SdfSpecifierOver, kSceneTestFactAttribute, sourceValue, errorMessage);
}

bool
sceneTestReadSourceValue(const ScenePayload& payload,
                         int* value,
                         std::string* errorMessage)
{
    *value = 0;
    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(payload, SceneLoadSet::everything(), errorMessage);
    if (!lease.isValid()) {
        return false;
    }
    const PXR_NS::UsdPrim prim = lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath(kSceneTestPrimPath));
    if (prim.IsValid()) {
        prim.GetAttribute(PXR_NS::TfToken(kSceneTestSourceAttribute)).Get(value);
    }

    return true;
}

bool
sceneTestRenderGeometry(SceneTestGeometry* effect,
                        const ScenePayload& material,
                        const SceneEvaluationContext& materialContext,
                        const std::string& primPath,
                        SceneGraftMemo* memo,
                        ScenePayload* output,
                        std::string* errorMessage)
{
    SceneMaterialGraft graft;
    if (!SceneNodeAuthoring::graftMaterialInput(material, materialContext, primPath, effect->getScriptName_mt_safe(), memo, &graft, errorMessage)) {
        return false;
    }
    const std::string materialPath = graft.materialPath;

    const auto author = [primPath, materialPath](const SceneLayerEdit& edit, std::string* error) {
        PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
        if (!stage) {
            return false;
        }
        if (!PXR_NS::SdfPath::IsValidPathString(primPath)) {
            *error = "\"" + primPath + "\" is not a prim path";

            return false;
        }
        const PXR_NS::UsdPrim cube = stage->DefinePrim(PXR_NS::SdfPath(primPath), PXR_NS::TfToken("Cube"));
        if (!cube) {
            *error = "could not define a cube at " + primPath;

            return false;
        }

        return SceneNodeAuthoring::authorMaterialBinding(cube, materialPath, error);
    };

    return effect->emitLayer(graft.payload, graft.factsDigest, author, output, errorMessage);
}

CarCity::CarCity(int blocks,
                 int buildingsPerBlock,
                 bool authorExtentsHints)
{
    static std::atomic<int> counter(0);

    _dir = std::filesystem::temp_directory_path() / ("scene_car_city_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    std::filesystem::create_directories(_dir / "buildings");
    _dir = std::filesystem::canonical(_dir);
    std::ofstream root(rootPath());
    const std::string quad = "        int[] faceVertexCounts = [4]\n        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                             "        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]\n";
    root << "#usda 1.0\n(\n    defaultPrim = \"World\"\n    upAxis = \"Y\"\n    metersPerUnit = 1\n)\ndef Xform \"World\"\n{\n"
         << "    def Mesh \"Ground\"\n    {\n"
         << quad << "    }\n";
    for (int b = 0; b < blocks; ++b) {
        root << "    def Xform \"Block_" << b << "\"\n    {\n";
        for (int n = 0; n < buildingsPerBlock; ++n) {
            const std::string name = "b" + std::to_string(b) + "_" + std::to_string(n) + ".usda";
            const std::string file = (_dir / "buildings" / name).string();
            std::ofstream building(file);
            building << "#usda 1.0\n(\n    defaultPrim = \"Building\"\n)\ndef Xform \"Building\" (\n    variantSets = \"look\"\n)\n{\n"
                     << "    variantSet \"look\" = {\n        \"plain\" {\n        }\n        \"tinted\" {\n            def Cube \"Tint\"\n            {\n            }\n        }\n    }\n"
                     << "    def Cube \"Geo\"\n    {\n    }\n    def Xform \"car_0\"\n    {\n    }\n"
                     << "    def Mesh \"Panel\"\n    {\n"
                     << quad << "    }\n";
            if ((b == 0) && (n == 0)) {
                building << "    def Camera \"Cam\"\n    {\n        double3 xformOp:translate = (0, 0, 10)\n"
                         << "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n    }\n"
                         << "    def Xform \"Reset\"\n    {\n        double3 xformOp:translate = (1, 2, 3)\n"
                         << "        uniform token[] xformOpOrder = [\"!resetXformStack!\", \"xformOp:translate\"]\n    }\n";
            }
            building << "}\n";
            root << "        def Xform \"B_" << n << "\" (\n            prepend payload = @./buildings/" << name << "@\n        )\n        {\n";
            if (authorExtentsHints) {
                root << "            float3[] extentsHint = [(0, 0, 0), (2, 4, 6)]\n";
            }
            root << "        }\n";
            _buildingPaths.push_back(file);
        }
        root << "    }\n";
    }
    root << "}\n";
}

CarCity::~CarCity()
{
    std::error_code ignored;

    std::filesystem::remove_all(_dir, ignored);
}

std::string
CarCity::rootPath() const
{
    return (_dir / "root.usda").string();
}

ScenePayload
CarCity::payload() const
{
    const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(rootPath());

    EXPECT_TRUE(bool(layer));

    return SceneOps::appendLayer(ScenePayload(), layer, 1);
}

int
CarCity::getNumBuildings() const
{
    return (int)_buildingPaths.size();
}

int
CarCity::openedBuildings() const
{
    int count = 0;

    for (const std::string& path : _buildingPaths) {
        if (PXR_NS::SdfLayer::Find(path)) {
            ++count;
        }
    }

    return count;
}

std::string
carPath(int block,
        int index)
{
    return "/World/Block_" + std::to_string(block) + "/B_" + std::to_string(index) + "/car_0";
}

SceneLoadSetPtr
loadingOnly(const std::vector<std::string>& paths)
{
    std::vector<SceneLoadSet::Entry> entries;
    SceneLoadSet::Entry none;

    none.path = "/";
    none.rule = SceneLoadSet::eRuleNone;
    entries.push_back(none);
    for (const std::string& path : paths) {
        SceneLoadSet::Entry all;
        all.path = path;
        all.rule = SceneLoadSet::eRuleAll;
        entries.push_back(all);
    }

    return std::make_shared<const SceneLoadSet>(entries);
}

void
expectParity(const NodePtr& node,
             const SceneLoadSetPtr& loadSet)
{
    ASSERT_TRUE(bool(node));
    std::string error;
    ScenePayload viewer;
    ScenePayload full;
    const EffectInstancePtr effect = node->getEffectInstance();

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              effect->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), SceneEvaluationContext(SceneEvaluationContext::makeId(), loadSet)), &viewer, &error))
        << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              effect->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), SceneEvaluationContext()), &full, &error))
        << error;

    const PXR_NS::UsdStageRefPtr viewerStage = openLoaded(viewer, loadSet);
    const PXR_NS::UsdStageRefPtr fullStage = openLoaded(full, loadSet);
    ASSERT_TRUE(viewerStage && fullStage);
    const std::map<std::string, PrimSnapshot> expected = snapshot(fullStage);
    const std::map<std::string, PrimSnapshot> actual = snapshot(viewerStage);

    for (const auto& item : expected) {
        const auto found = actual.find(item.first);
        if (found == actual.end()) {
            ADD_FAILURE() << "missing prim " << item.first;
            continue;
        }
        EXPECT_EQ(item.second.typeName, found->second.typeName) << item.first;
        EXPECT_EQ(item.second.active, found->second.active) << item.first;
        EXPECT_EQ(item.second.properties, found->second.properties) << item.first;
    }
    for (const auto& item : actual) {
        if (expected.find(item.first) == expected.end()) {
            ADD_FAILURE() << "extra prim " << item.first;
        }
    }
}

NATRON_NAMESPACE_EXIT
