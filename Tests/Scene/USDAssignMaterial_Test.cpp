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

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDAssignMaterial.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDScopePrim.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
// /Asset/geo/{a,b}, and the materials M and N in the default Materials Scope.
const char* const kAsset = "#usda 1.0\n"
                           "def Xform \"Asset\"\n{\n"
                           "    def Xform \"geo\"\n    {\n"
                           "        def Mesh \"a\"\n        {\n        }\n"
                           "        def Mesh \"b\"\n        {\n        }\n"
                           "    }\n}\n"
                           "def Scope \"materials\"\n{\n"
                           "    def Material \"M\"\n    {\n    }\n"
                           "    def Material \"N\"\n    {\n    }\n}\n";

void
setString(const NodePtr& node,
          const char* name,
          const std::string& value)
{
    KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(name).get());

    ASSERT_TRUE(knob != NULL) << name;
    knob->setValue(value);
}

std::string
boundMaterial(const PXR_NS::UsdStageRefPtr& stage,
              const std::string& path)
{
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(path));

    EXPECT_TRUE(bool(prim)) << path;
    if (!prim) {
        return std::string();
    }
    const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();

    return material ? material.GetPath().GetString() : std::string();
}

std::vector<std::string>
childNames(const PXR_NS::UsdStageRefPtr& stage,
           const std::string& path)
{
    std::vector<std::string> names;

    for (const PXR_NS::UsdPrim& child : stage->GetPrimAtPath(PXR_NS::SdfPath(path)).GetChildren()) {
        names.push_back(child.GetName().GetString());
    }

    return names;
}

QString
getMessage(const NodePtr& node,
           int* type)
{
    QString message;

    node->getPersistentMessage(&message, type, false);

    return message;
}
} // anonymous namespace

class USDAssignMaterialTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static ScenePayload evaluate(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return payload;
    }

    static PXR_NS::UsdStageRefPtr compose(const NodePtr& node)
    {
        return SceneOps::compose(evaluate(node));
    }

    void setProjectMaterialsScope(const std::string& path)
    {
        KnobString* knob = dynamic_cast<KnobString*>(getApp()->getProject()->getKnobByName("materialsScope").get());

        ASSERT_TRUE(knob != NULL);
        knob->setValue(path);
    }

    // A USDRead of kAsset.
    NodePtr asset()
    {
        EXPECT_TRUE(_dir.isValid());
        const QString path = _dir.filePath(QString::fromUtf8("asset.usda"));
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(kAsset);
        file.close();

        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        EXPECT_TRUE(bool(reader));
        if (!reader) {
            return reader;
        }
        KnobFile* knob = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
        EXPECT_TRUE(knob != NULL);
        if (knob) {
            knob->setValue(path.toStdString());
        }

        return reader;
    }

    // A USDMaterial defining its material at path.
    NodePtr material(const char* path)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));

        EXPECT_TRUE(bool(node));
        if (node) {
            setString(node, kSceneParamPrimPath, path);
        }

        return node;
    }

    // A USDAssignMaterial on scene binding materialPath, or the material input when given.
    NodePtr assign(const NodePtr& scene,
                   const char* pattern,
                   const char* materialPath,
                   const NodePtr& materialInput = NodePtr())
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        setString(node, kUSDAssignMaterialParamPrimPattern, pattern);
        setString(node, kUSDAssignMaterialParamMaterialPath, materialPath);
        EXPECT_TRUE(node->connectInput(scene, kUSDAssignMaterialInputScene));
        if (materialInput) {
            EXPECT_TRUE(node->connectInput(materialInput, kUSDAssignMaterialInputMaterial));
        }

        return node;
    }

    // A reparenting node of pluginID at path, with input's roots grafted under it.
    NodePtr reparent(const char* pluginID,
                     const char* path,
                     const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        setString(node, kSceneParamPrimPath, path);
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }

    QTemporaryDir _dir;
};

TEST_F(USDAssignMaterialTest, ItBindsAMaterialInTheSceneAndTheBindingFollowsAReparent)
{
    NodePtr reader = asset();
    ASSERT_TRUE(bool(reader));
    NodePtr node = assign(reader, "/Asset/geo/*", "/materials/M");
    ASSERT_TRUE(bool(node));

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/a"));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/b"));
    EXPECT_EQ("", boundMaterial(stage, "/Asset"));
    EXPECT_FALSE(node->hasPersistentMessage());
    // One collection and one binding for the root's two matches.
    const std::vector<PXR_NS::TfToken> bindings = SceneNodeAuthoring::getCollectionBindingNames(stage->GetPrimAtPath(PXR_NS::SdfPath("/Asset")));
    ASSERT_EQ(1u, bindings.size());
    EXPECT_EQ("material:binding:collection:" + node->getFullyQualifiedName(), bindings[0].GetString());

    NodePtr moved = reparent(PLUGINID_NATRON_USDXFORMPRIM, "/T1", node);
    ASSERT_TRUE(bool(moved));
    stage = compose(moved);
    ASSERT_TRUE(bool(stage));
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Asset")).IsValid());
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/T1/Asset/geo/a"));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/T1/Asset/geo/b"));
}

TEST_F(USDAssignMaterialTest, TheLaterOfTwoAssignsWins)
{
    NodePtr reader = asset();
    ASSERT_TRUE(bool(reader));
    NodePtr wide = assign(reader, "/Asset/geo", "/materials/M");
    NodePtr narrow = assign(wide, "/Asset/geo/a", "/materials/N");
    ASSERT_TRUE(wide && narrow);
    PXR_NS::UsdStageRefPtr stage = compose(narrow);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/materials/N", boundMaterial(stage, "/Asset/geo/a"));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/b"));

    NodePtr narrowFirst = assign(reader, "/Asset/geo/a", "/materials/N");
    NodePtr wideLast = assign(narrowFirst, "/Asset/geo", "/materials/M");
    ASSERT_TRUE(narrowFirst && wideLast);
    stage = compose(wideLast);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/a"));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/b"));
}

TEST_F(USDAssignMaterialTest, ItOverridesACardsOwnMaterial)
{
    NodePtr reader = asset();
    NodePtr own = material("/Own");
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(reader && own && card);
    setString(card, kSceneParamPrimPath, "/Card");
    ASSERT_TRUE(card->connectInput(own, 0));
    NodePtr merged = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(bool(merged));
    ASSERT_TRUE(merged->connectInput(reader, 0));
    ASSERT_TRUE(merged->connectInput(card, 1));

    PXR_NS::UsdStageRefPtr stage = compose(merged);
    ASSERT_TRUE(bool(stage));
    ASSERT_EQ("/Card/Own", boundMaterial(stage, "/Card"));

    NodePtr node = assign(merged, "/Card", "/materials/M");
    ASSERT_TRUE(bool(node));
    stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Card"));
}

TEST_F(USDAssignMaterialTest, APatternMatchingNothingWarnsAndPassesThrough)
{
    NodePtr reader = asset();
    ASSERT_TRUE(bool(reader));
    NodePtr node = assign(reader, "/Nothing", "/materials/M");
    ASSERT_TRUE(bool(node));

    const ScenePayload upstream = evaluate(reader);
    const ScenePayload output = evaluate(node);
    EXPECT_EQ(SceneOps::getLayers(upstream).size(), SceneOps::getLayers(output).size());
    ASSERT_TRUE(node->hasPersistentMessage());
    int type = 0;
    const QString message = getMessage(node, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Nothing"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("matches no prim"))) << message.toStdString();
}

TEST_F(USDAssignMaterialTest, MatchesInsideTheMaterialsScopeAreSkippedWithAWarning)
{
    NodePtr reader = asset();
    ASSERT_TRUE(bool(reader));
    NodePtr node = assign(reader, "/materials/N /Asset/geo/a", "/materials/M");
    ASSERT_TRUE(bool(node));

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/a"));
    EXPECT_TRUE(SceneNodeAuthoring::getCollectionBindingNames(stage->GetPrimAtPath(PXR_NS::SdfPath("/materials"))).empty());
    ASSERT_TRUE(node->hasPersistentMessage());
    int type = 0;
    const QString message = getMessage(node, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/materials/N"))) << message.toStdString();
}

TEST_F(USDAssignMaterialTest, AnUnresolvedMaterialPathStillAuthorsTheBindingWithAWarning)
{
    NodePtr reader = asset();
    ASSERT_TRUE(bool(reader));
    NodePtr node = assign(reader, "/Asset/geo/a", "/materials/Missing");
    ASSERT_TRUE(bool(node));

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim root = stage->GetPrimAtPath(PXR_NS::SdfPath("/Asset"));
    const std::vector<PXR_NS::TfToken> bindings = SceneNodeAuthoring::getCollectionBindingNames(root);
    ASSERT_EQ(1u, bindings.size());
    PXR_NS::SdfPathVector targets;
    EXPECT_TRUE(root.GetRelationship(bindings[0]).GetTargets(&targets));
    ASSERT_EQ(2u, targets.size());
    EXPECT_EQ(PXR_NS::SdfPath("/materials/Missing"), targets[1]);
    EXPECT_EQ("", boundMaterial(stage, "/Asset/geo/a"));
    ASSERT_TRUE(node->hasPersistentMessage());
    int type = 0;
    const QString message = getMessage(node, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/materials/Missing"))) << message.toStdString();
}

TEST_F(USDAssignMaterialTest, TheMaterialInputLandsInTheMaterialsScope)
{
    NodePtr reader = asset();
    NodePtr shared = material("/Material1");
    ASSERT_TRUE(reader && shared);
    NodePtr node = assign(reader, "/Asset/geo/*", "", shared);
    ASSERT_TRUE(bool(node));

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/materials/Material1")).IsA<PXR_NS::UsdShadeMaterial>());
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1")).IsValid());
    EXPECT_EQ("/materials/Material1", boundMaterial(stage, "/Asset/geo/a"));
    EXPECT_EQ("/materials/Material1", boundMaterial(stage, "/Asset/geo/b"));
    EXPECT_FALSE(node->hasPersistentMessage());
}

TEST_F(USDAssignMaterialTest, TwoAssignsSharingOneMaterialNodeHoldOneMaterial)
{
    NodePtr reader = asset();
    NodePtr shared = material("/Material1");
    ASSERT_TRUE(reader && shared);
    NodePtr first = assign(reader, "/Asset/geo/a", "", shared);
    NodePtr second = assign(first, "/Asset/geo/b", "", shared);
    ASSERT_TRUE(first && second);

    PXR_NS::UsdStageRefPtr stage = compose(second);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(std::vector<std::string>({ "M", "N", "Material1" }), childNames(stage, "/materials"));
    EXPECT_EQ("/materials/Material1", boundMaterial(stage, "/Asset/geo/a"));
    EXPECT_EQ("/materials/Material1", boundMaterial(stage, "/Asset/geo/b"));
}

TEST_F(USDAssignMaterialTest, AMaterialAlreadyInTheScopeDoesNotNest)
{
    NodePtr reader = asset();
    NodePtr shared = material("/Material1");
    ASSERT_TRUE(reader && shared);
    NodePtr scope = reparent(PLUGINID_NATRON_USDSCOPEPRIM, "/materials", shared);
    ASSERT_TRUE(bool(scope));
    NodePtr node = assign(reader, "/Asset/geo/a", "", scope);
    ASSERT_TRUE(bool(node));

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/materials/materials")).IsValid());
    // The passed-through scope arrives by reference, so its children compose ahead of the input's.
    std::vector<std::string> names = childNames(stage, "/materials");
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::vector<std::string>({ "M", "Material1", "N" }), names);
    EXPECT_EQ("/materials/Material1", boundMaterial(stage, "/Asset/geo/a"));
}

TEST_F(USDAssignMaterialTest, TheMaterialInputWithNoMaterialsScopeIsAnError)
{
    setProjectMaterialsScope("");
    NodePtr reader = asset();
    NodePtr shared = material("/Material1");
    ASSERT_TRUE(reader && shared);
    NodePtr node = assign(reader, "/Asset/geo/a", "", shared);
    ASSERT_TRUE(bool(node));

    ScenePayload payload;
    std::string error;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error));
    EXPECT_FALSE(error.empty());
    ASSERT_TRUE(node->hasPersistentMessage());
    int type = 0;
    getMessage(node, &type);
    EXPECT_EQ((int)eMessageTypeError, type);
}

TEST_F(USDAssignMaterialTest, MaterialPathIsGreyedWhileTheMaterialInputIsConnected)
{
    NodePtr reader = asset();
    NodePtr shared = material("/Material1");
    ASSERT_TRUE(reader && shared);
    NodePtr node = assign(reader, "/Asset/geo/a", "/materials/M");
    ASSERT_TRUE(bool(node));
    KnobIPtr materialPath = node->getKnobByName(kUSDAssignMaterialParamMaterialPath);
    ASSERT_TRUE(bool(materialPath));

    EXPECT_TRUE(materialPath->isEnabled(0));
    ASSERT_TRUE(node->connectInput(shared, kUSDAssignMaterialInputMaterial));
    EXPECT_FALSE(materialPath->isEnabled(0));
    node->disconnectInput(kUSDAssignMaterialInputMaterial);
    EXPECT_TRUE(materialPath->isEnabled(0));
}
