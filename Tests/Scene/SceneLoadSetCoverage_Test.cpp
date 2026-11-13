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
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "SceneTestAuthoring.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneReparentNode.h"
#include "Engine/Nodes/Scene/USDAssignMaterial.h"
#include "Engine/Nodes/Scene/USDCube.h"
#include "Engine/Nodes/Scene/USDEditPrim.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDProjectUV.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDReference.h"
#include "Engine/Nodes/Scene/USDSetAttribute.h"
#include "Engine/Nodes/Scene/USDVariant.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

class SceneLoadSetCoverageTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    NodePtr reader(const CarCity& city)
    {
        return readerOf(city.rootPath());
    }

    NodePtr readerOf(const std::string& path)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));

        EXPECT_TRUE(bool(node));
        if (node) {
            KnobFilePtr file = std::dynamic_pointer_cast<KnobFile>(node->getKnobByName(kUSDReadParamFile));
            EXPECT_TRUE(bool(file));
            if (file) {
                file->setValue(path);
            }
        }

        return node;
    }

    NodePtr merge(const std::vector<NodePtr>& inputs)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            EXPECT_TRUE(node->connectInput(inputs[i], (int)i)) << i;
        }

        return node;
    }

    NodePtr xformPrim(const NodePtr& input,
                      const std::string& path)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobString* pathKnob = dynamic_cast<KnobString*>(node->getKnobByName(kSceneParamPrimPath).get());
        EXPECT_TRUE(pathKnob != NULL);
        if (pathKnob) {
            pathKnob->setValue(path);
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }

    NodePtr prune(const NodePtr& input,
                  const std::string& pattern)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDPruneParamPrimPattern).get());
        EXPECT_TRUE(knob != NULL);
        if (knob) {
            knob->setValue(pattern);
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }

    NodePtr setAttribute(const NodePtr& input,
                         const std::string& pattern)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDSETATTRIBUTE));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobString* patternKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDSetAttributeParamPrimPattern).get());
        KnobString* nameKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDSetAttributeParamAttributeName).get());
        EXPECT_TRUE(patternKnob && nameKnob);
        if (patternKnob && nameKnob) {
            patternKnob->setValue(pattern);
            nameKnob->setValue(std::string("tag"));
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }

    NodePtr editPrim(const NodePtr& input,
                     const std::string& mode,
                     const std::string& target)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITPRIM));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobChoice* modeKnob = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDEditPrimParamMode).get());
        KnobChoice* schemaKnob = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDEditPrimParamSchema).get());
        KnobString* targetKnob = dynamic_cast<KnobString*>(node->getKnobByName(mode == "edit" ? kUSDEditPrimParamPrimPattern : kSceneParamPrimPath).get());
        EXPECT_TRUE(modeKnob && schemaKnob && targetKnob);
        if (modeKnob && schemaKnob && targetKnob) {
            modeKnob->setValueFromID(mode, 0);
            schemaKnob->setValueFromID("Camera", 0);
            targetKnob->setValue(target);
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }

    NodePtr reference(const NodePtr& input,
                      const CarCity& city,
                      const std::string& target,
                      const std::string& sourcePrim)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREFERENCE));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobFile* fileKnob = dynamic_cast<KnobFile*>(node->getKnobByName(kUSDReferenceParamFile).get());
        KnobString* primKnob = dynamic_cast<KnobString*>(node->getKnobByName(kSceneParamPrimPath).get());
        KnobString* sourcePrimKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDReferenceParamSourcePrim).get());
        EXPECT_TRUE(fileKnob && primKnob && sourcePrimKnob);
        if (fileKnob && primKnob && sourcePrimKnob) {
            fileKnob->setValue(city.rootPath());
            primKnob->setValue(target);
            sourcePrimKnob->setValue(sourcePrim);
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }

    NodePtr variant(const NodePtr& input,
                    const std::string& pattern)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDVARIANT));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobString* patternKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDVariantParamPrimPattern).get());
        KnobString* setKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDVariantParamVariantSet).get());
        KnobString* variantKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDVariantParamVariant).get());
        EXPECT_TRUE(patternKnob && setKnob && variantKnob);
        if (patternKnob && setKnob && variantKnob) {
            patternKnob->setValue(pattern);
            setKnob->setValue(std::string("look"));
            variantKnob->setValue(std::string("tinted"));
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }

    NodePtr projectUV(const NodePtr& input,
                      const std::string& pattern,
                      const std::string& projection,
                      const std::string& cameraPath)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECTUV));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobString* patternKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDProjectUVParamPrimPattern).get());
        KnobChoice* projectionKnob = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDProjectUVParamProjection).get());
        KnobString* cameraKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDProjectUVParamCameraPath).get());
        EXPECT_TRUE(patternKnob && projectionKnob && cameraKnob);
        if (patternKnob && projectionKnob && cameraKnob) {
            patternKnob->setValue(pattern);
            projectionKnob->setValueFromID(projection, 0);
            cameraKnob->setValue(cameraPath);
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }
};

namespace {
// Whether the scene, composed with nothing loaded, has UVs authored on the prim at path.
bool
hasUVsUnloaded(const ScenePayload& payload,
               const std::string& path)
{
    std::string error;
    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(payload, std::make_shared<const SceneLoadSet>(SceneLoadSet::none()), &error);
    const PXR_NS::UsdStageRefPtr stage = lease.getStage();
    EXPECT_TRUE(bool(stage)) << error;
    if (!stage) {
        return false;
    }
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(path));

    return prim && prim.GetAttribute(PXR_NS::TfToken("primvars:st")).HasAuthoredValue();
}

/**
 * @brief Writes, next to city's root layer, a layer sublayering it that adds links out of /World:
 * on /World itself and on the Geo of /World/Block_0/B_0 and of /World/Block_1/B_0, inside their
 * payloads, each to /Props/Lamp and to a prim no root holds. Adds the roots /Props and
 * /materials. Returns its path.
 **/
std::string
writeLinkedCity(const CarCity& city)
{
    const std::filesystem::path path = std::filesystem::path(city.rootPath()).parent_path() / "linked.usda";
    std::ofstream file(path.string());
    const std::string links = "rel lookAt = </Props/Lamp>\n        rel lost = </nowhere/Inside>\n";

    file << "#usda 1.0\n(\n    defaultPrim = \"World\"\n    upAxis = \"Y\"\n    metersPerUnit = 1\n    subLayers = [@./root.usda@]\n)\n"
         << "over \"World\"\n{\n    rel lookAt = </Props/Lamp>\n    rel lost = </nowhere>\n";
    for (int block = 0; block < 2; ++block) {
        file << "    over \"Block_" << block << "\"\n    {\n        over \"B_0\"\n        {\n            over \"Geo\"\n            {\n"
             << "                " << links << "            }\n        }\n    }\n";
    }
    file << "}\ndef Xform \"Props\"\n{\n    def Xform \"Lamp\"\n    {\n    }\n}\n"
         << "def Scope \"materials\"\n{\n    def Material \"M\"\n    {\n    }\n}\n";

    return path.string();
}

ScenePayload
linkedCityPayload(const std::string& path)
{
    const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path);

    EXPECT_TRUE(bool(layer));

    return SceneOps::appendLayer(ScenePayload(), layer, 1);
}

SceneReparentArgs
graftUnderGroup(const ScenePayload& input)
{
    SceneReparentArgs args;

    args.inputs.push_back(input);
    args.destinationPath = "/Grp";
    args.destinationType = "Xform";
    args.ancestorType = "Xform";
    args.passThroughRoot = "materials";
    args.layerTag = "SceneLoadSetCoverageTest";

    return args;
}

struct GraftReport {
    ScenePayload output;
    std::vector<std::string> grafts;
    std::vector<std::string> unmapped;
};

// reparentRoots() as an evaluation in context runs it.
GraftReport
graftIn(const SceneEvaluationContext& context,
        const SceneReparentArgs& args,
        SceneGraftMemo* memo)
{
    GraftReport report;
    std::vector<SceneRootGraft> grafts;
    std::vector<SceneUnmappedLink> unmapped;
    std::string error;
    {
        SceneEvaluationContext::Scope scope(context);
        EXPECT_TRUE(SceneOps::reparentRoots(args, memo, &report.output, &grafts, &unmapped, &error)) << error;
    }
    for (const SceneRootGraft& graft : grafts) {
        report.grafts.push_back(graft.sourcePath + " -> " + graft.graftedPath + (graft.passedThrough ? " passed" : "") + (graft.holdsGprim ? " gprim" : ""));
    }
    for (const SceneUnmappedLink& link : unmapped) {
        report.unmapped.push_back(link.propertyPath + " -> " + link.targetPath);
    }
    std::sort(report.unmapped.begin(), report.unmapped.end());

    return report;
}

// Evaluates node as a viewer loading loadSet, reach gathering the paths a load change reaches.
void
evaluateAsViewer(const NodePtr& node,
                 const SceneLoadSetPtr& loadSet,
                 const SceneLoadReachPtr& reach)
{
    ASSERT_TRUE(bool(node));
    std::string error;
    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), SceneEvaluationContext(SceneEvaluationContext::makeId(), loadSet, reach)), &payload, &error))
        << error;
    EXPECT_FALSE(payload.isEmpty());
}

// The targets of the relationship at path in payload composed with loadSet; empty when it is absent.
std::vector<std::string>
relationshipTargets(const ScenePayload& payload,
                    const SceneLoadSetPtr& loadSet,
                    const std::string& path)
{
    std::vector<std::string> targets;
    std::string error;
    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(payload, loadSet, &error);
    const PXR_NS::UsdStageRefPtr stage = lease.getStage();
    EXPECT_TRUE(bool(stage)) << error;
    if (!stage) {
        return targets;
    }
    const PXR_NS::UsdRelationship relationship = stage->GetRelationshipAtPath(PXR_NS::SdfPath(path));
    std::vector<PXR_NS::SdfPath> paths;
    if (relationship) {
        relationship.GetTargets(&paths);
    }
    for (const PXR_NS::SdfPath& target : paths) {
        targets.push_back(target.GetString());
    }

    return targets;
}

// Whether some prefix is root or lies below it.
bool
reachesUnder(const std::vector<std::string>& prefixes,
             const std::string& root)
{
    for (const std::string& prefix : prefixes) {
        if ((prefix == root) || (prefix.compare(0, root.size() + 1, root + "/") == 0)) {
            return true;
        }
    }

    return false;
}

void
setChoice(const NodePtr& node,
          const char* name,
          const std::string& id)
{
    KnobChoice* knob = dynamic_cast<KnobChoice*>(node->getKnobByName(name).get());
    ASSERT_TRUE(knob != NULL) << name;
    knob->setValueFromID(id, 0);
}

void
setString(const NodePtr& node,
          const char* name,
          const std::string& value)
{
    KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(name).get());
    ASSERT_TRUE(knob != NULL) << name;
    knob->setValue(value);
}

// /Lib/A and /Lib/B, each a payload file holding one material, in a directory of their own.
class PayloadMaterials {
public:
    PayloadMaterials()
    {
        static std::atomic<int> counter(0);

        _dir = std::filesystem::temp_directory_path() / ("scene_payload_materials_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(_dir);
        _dir = std::filesystem::canonical(_dir);
        std::ofstream root(rootPath());
        root << "#usda 1.0\n(\n    defaultPrim = \"Lib\"\n)\ndef Xform \"Lib\"\n{\n";
        for (const char* name : { "A", "B" }) {
            const std::string file = (_dir / (std::string(name) + ".usda")).string();
            std::ofstream payload(file);
            payload << "#usda 1.0\n(\n    defaultPrim = \"Holder\"\n)\ndef Xform \"Holder\"\n{\n    def Material \"M\"\n    {\n    }\n}\n";
            root << "    def Xform \"" << name << "\" (\n        prepend payload = @./" << name << ".usda@\n    )\n    {\n    }\n";
            _payloadPaths.push_back(file);
        }
        root << "}\n";
    }

    ~PayloadMaterials()
    {
        std::error_code ignored;

        std::filesystem::remove_all(_dir, ignored);
    }

    PayloadMaterials(const PayloadMaterials&) = delete;
    PayloadMaterials& operator=(const PayloadMaterials&) = delete;

    std::string rootPath() const
    {
        return (_dir / "root.usda").string();
    }

    int openedPayloads() const
    {
        int count = 0;

        for (const std::string& path : _payloadPaths) {
            if (PXR_NS::SdfLayer::Find(path)) {
                ++count;
            }
        }

        return count;
    }

private:
    std::filesystem::path _dir;
    std::vector<std::string> _payloadPaths;
};
} // anonymous namespace

TEST_F(SceneLoadSetCoverageTest, TheProbeCountsTheBuildingsOpenInTheProcess)
{
    CarCity city(2, 3);
    SceneQueryPool pool(2);
    const ScenePayload payload = city.payload();
    std::string error;

    EXPECT_EQ(6, city.getNumBuildings());
    EXPECT_EQ(0, city.openedBuildings());
    {
        SceneQueryPool::Lease lease = pool.borrow(payload, std::make_shared<const SceneLoadSet>(SceneLoadSet::none()), &error);
        ASSERT_TRUE(lease.isValid()) << error;
        EXPECT_EQ(0, city.openedBuildings());
    }
    pool.releaseIdle();
    {
        SceneQueryPool::Lease lease = pool.borrow(payload, &error);
        ASSERT_TRUE(lease.isValid()) << error;
        EXPECT_EQ(6, city.openedBuildings());
    }
    pool.releaseIdle();
}

TEST_F(SceneLoadSetCoverageTest, APruneEvaluatesTheSameOnALoadSetAsOnEverything)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = prune(source, "/World/Block_0//car*");
    ASSERT_TRUE(source && node);

    expectParity(node, loadingOnly({ "/World/Block_0" }));
}

TEST_F(SceneLoadSetCoverageTest, ASetAttributeOnABuildingOpensNoBuildingWithNothingLoaded)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = setAttribute(source, "/World/Block_0/B_0");
    ASSERT_TRUE(source && node);
    std::string error;
    ScenePayload payload;

    const SceneEvaluationContext context(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error))
        << error;
    EXPECT_FALSE(payload.isEmpty());
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, ASetAttributeEvaluatesTheSameOnALoadSetAsOnEverything)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = setAttribute(source, "/World/Block_0//car*");
    ASSERT_TRUE(source && node);

    expectParity(node, loadingOnly({ "/World/Block_0" }));
}

TEST_F(SceneLoadSetCoverageTest, ASetAttributeThatFindsNothingSaysHowManyBranchesWereNotSearched)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = setAttribute(source, "/World//car*");
    ASSERT_TRUE(source && node);
    std::string error;
    ScenePayload payload;

    const SceneEvaluationContext context(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error))
        << error;
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("0 matches, 4 unloaded branches not searched"))) << message.toStdString();
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, AnEditPrimInEachModeOpensNoBuildingWithNothingLoaded)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr edit = editPrim(source, "edit", "/World/Block_0/B_0");
    NodePtr define = editPrim(source, "define", "/World/Block_0/Added");
    ASSERT_TRUE(source && edit && define);
    const SceneEvaluationContext context(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));

    for (const NodePtr& node : { edit, define }) {
        std::string error;
        ScenePayload payload;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error))
            << error;
        EXPECT_FALSE(payload.isEmpty());
    }
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, AnEditPrimEvaluatesTheSameOnALoadSetAsOnEverything)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr edit = editPrim(source, "edit", "/World/Block_0//car*");
    NodePtr define = editPrim(source, "define", "/World/Block_0/Added");
    ASSERT_TRUE(source && edit && define);

    expectParity(edit, loadingOnly({ "/World/Block_0" }));
    expectParity(define, loadingOnly({ "/World/Block_0" }));
}

TEST_F(SceneLoadSetCoverageTest, AReferenceOpensNoBuildingOffItsTargetChain)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr outside = reference(source, city, "/World/Block_0/Added", "/World");
    NodePtr inside = reference(source, city, "/World/Block_0/B_0/Added", "/World");
    ASSERT_TRUE(source && outside && inside);
    const SceneEvaluationContext context(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));

    std::string error;
    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              outside->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error))
        << error;
    EXPECT_FALSE(payload.isEmpty());
    EXPECT_EQ(0, city.openedBuildings());

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              inside->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error))
        << error;
    EXPECT_FALSE(payload.isEmpty());
    EXPECT_LE(city.openedBuildings(), 1);
}

TEST_F(SceneLoadSetCoverageTest, AReferenceEvaluatesTheSameOnALoadSetAsOnEverything)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr inside = reference(source, city, "/World/Block_0/B_0/Added", "/World");
    ASSERT_TRUE(source && inside);

    expectParity(inside, loadingOnly({ "/World/Block_0" }));
}

TEST_F(SceneLoadSetCoverageTest, AVariantOnABuildingOpensNoBuildingWithNothingLoaded)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = variant(source, "/World/Block_0/B_0");
    ASSERT_TRUE(source && node);
    std::string error;
    ScenePayload payload;

    const SceneEvaluationContext context(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error))
        << error;
    EXPECT_FALSE(payload.isEmpty());
    EXPECT_EQ(0, city.openedBuildings());

    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_FALSE(message.contains(QString::fromUtf8("No variant set"))) << message.toStdString();
}

TEST_F(SceneLoadSetCoverageTest, AVariantSetAuthoredInAPayloadEvaluatesTheSameOnALoadSetAsOnEverything)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = variant(source, "/World/Block_0/B_0");
    ASSERT_TRUE(source && node);

    expectParity(node, loadingOnly({ "/World/Block_0" }));
}

TEST_F(SceneLoadSetCoverageTest, AProjectUVOnMeshesOutsidePayloadsOpensNoBuildingWithNothingLoaded)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = projectUV(source, "/World//*", "planar", std::string());
    ASSERT_TRUE(source && node);
    std::string error;
    ScenePayload payload;

    const SceneEvaluationContext context(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error))
        << error;
    EXPECT_FALSE(payload.isEmpty());
    EXPECT_EQ(0, city.openedBuildings());
    EXPECT_TRUE(hasUVsUnloaded(payload, "/World/Ground"));
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, AProjectUVEvaluatesTheSameOnALoadSetAsOnEverything)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr firstCamera = projectUV(source, "/World//Panel /World/Ground", "perspective", std::string());
    NodePtr namedCamera = projectUV(source, "/World//Panel /World/Ground", "perspective", "/World/Block_0/B_0/Cam");
    ASSERT_TRUE(source && firstCamera && namedCamera);

    expectParity(firstCamera, loadingOnly({ "/World/Block_0" }));
    expectParity(namedCamera, loadingOnly({ "/World/Block_0" }));
}

TEST_F(SceneLoadSetCoverageTest, AProjectUVCameraInsideAPayloadResolvesWithOnlyItsBuildingOpen)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = projectUV(source, "/World/Ground", "perspective", "/World/Block_0/B_0/Cam");
    ASSERT_TRUE(source && node);
    std::string error;
    ScenePayload payload;

    const SceneEvaluationContext context(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error))
        << error;
    EXPECT_LE(city.openedBuildings(), 1);

    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_FALSE(message.contains(QString::fromUtf8("No camera"))) << message.toStdString();
    EXPECT_TRUE(hasUVsUnloaded(payload, "/World/Ground"));
}

TEST_F(SceneLoadSetCoverageTest, AMergeOfTheCityOpensNoBuildingWithNothingLoaded)
{
    CarCity city(2, 2);
    const std::string linked = writeLinkedCity(city);
    NodePtr first = reader(city);
    NodePtr second = readerOf(linked);
    ASSERT_TRUE(first && second);
    NodePtr node = merge({ first, second });
    ASSERT_TRUE(bool(node));
    std::string error;
    ScenePayload payload;

    const SceneEvaluationContext context(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error))
        << error;
    EXPECT_FALSE(payload.isEmpty());
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, AGraftOfTheCityOpensNoBuildingWithNothingLoaded)
{
    CarCity city(2, 2);
    NodePtr source = readerOf(writeLinkedCity(city));
    NodePtr node = xformPrim(source, "/Grp");
    ASSERT_TRUE(source && node);
    std::string error;
    ScenePayload payload;

    const SceneEvaluationContext context(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error))
        << error;
    EXPECT_FALSE(payload.isEmpty());
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, AGraftEvaluatesTheSameOnALoadSetAsOnEverything)
{
    CarCity city(2, 2);
    NodePtr source = readerOf(writeLinkedCity(city));
    NodePtr pruned = prune(source, "/World//car*");
    NodePtr node = xformPrim(pruned, "/Grp");
    ASSERT_TRUE(source && pruned && node);

    expectParity(node, loadingOnly({ "/Grp/World/Block_0" }));
}

TEST_F(SceneLoadSetCoverageTest, APruneAboveAGraftOpensOnlyTheGraftedPathsLoadedAndReachesThem)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr pruned = prune(source, "/World//car*");
    NodePtr node = xformPrim(pruned, "/Grp");
    ASSERT_TRUE(source && pruned && node);
    const SceneLoadSetPtr loaded = loadingOnly({ "/Grp/World/Block_0" });
    const SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();

    evaluateAsViewer(node, loaded, reach);
    EXPECT_EQ(2, city.openedBuildings());
    const std::vector<std::string> prefixes = reach->getPrefixes();
    EXPECT_TRUE(reachesUnder(prefixes, "/Grp/World"));
    EXPECT_FALSE(reachesUnder(prefixes, "/World"));
    EXPECT_TRUE(SceneOps::loadChangeReaches(loaded.get(), loadingOnly({ "/Grp/World/Block_0", "/Grp/World/Block_1" }).get(), prefixes));

    expectParity(node, loaded);
}

TEST_F(SceneLoadSetCoverageTest, APruneAboveAWrapEvaluatesTheSameAndReachesTheMovedPaths)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr pruned = prune(source, "/World//car*");
    NodePtr node = xformPrim(pruned, "/World");
    ASSERT_TRUE(source && pruned && node);
    setChoice(node, kSceneReparentParamMode, "wrap");
    setString(node, kSceneReparentParamWrapName, "Grp");
    const SceneLoadSetPtr loaded = loadingOnly({ "/Grp/World/Block_0" });
    const SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();

    evaluateAsViewer(node, loaded, reach);
    const std::vector<std::string> prefixes = reach->getPrefixes();
    EXPECT_TRUE(reachesUnder(prefixes, "/Grp/World"));
    EXPECT_TRUE(SceneOps::loadChangeReaches(loaded.get(), loadingOnly({ "/Grp/World/Block_0", "/Grp/World/Block_1" }).get(), prefixes));
    EXPECT_EQ(2, city.openedBuildings());

    expectParity(node, loaded);
}

TEST_F(SceneLoadSetCoverageTest, AWrapOpensNoBuildingsWhenTheViewerLoadsNone)
{
    CarCity city(2, 2);
    NodePtr node = xformPrim(reader(city), "/World");
    ASSERT_TRUE(bool(node));
    setChoice(node, kSceneReparentParamMode, "wrap");
    setString(node, kSceneReparentParamWrapName, "Grp");

    evaluateAsViewer(node, std::make_shared<const SceneLoadSet>(SceneLoadSet::none()), std::make_shared<SceneLoadReach>());

    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, AWrapRetargetsALinkInsideAPayloadOnceThePayloadLoads)
{
    CarCity city(2, 2);
    NodePtr node = xformPrim(readerOf(writeLinkedCity(city)), "/World");
    ASSERT_TRUE(bool(node));
    setChoice(node, kSceneReparentParamMode, "wrap");
    setString(node, kSceneReparentParamWrapName, "Grp");
    const std::string link = "/Grp/World/Block_0/B_0/Geo.lookAt";
    const SceneLoadSetPtr none = std::make_shared<const SceneLoadSet>(SceneLoadSet::none());
    const SceneLoadSetPtr loaded = loadingOnly({ "/Grp/World/Block_0" });
    const SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();
    const std::vector<std::string> lamp(1, "/Props/Lamp");

    ScenePayload before;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), SceneEvaluationContext(SceneEvaluationContext::makeId(), none, reach)), &before, &error))
        << error;
    EXPECT_EQ(0, city.openedBuildings());
    EXPECT_TRUE(relationshipTargets(before, none, link).empty());
    EXPECT_TRUE(SceneOps::loadChangeReaches(none.get(), loaded.get(), reach->getPrefixes()));

    ScenePayload after;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), SceneEvaluationContext(SceneEvaluationContext::makeId(), loaded, std::make_shared<SceneLoadReach>())), &after, &error))
        << error;
    EXPECT_EQ(lamp, relationshipTargets(after, loaded, link));
}

TEST_F(SceneLoadSetCoverageTest, AGraftRenamingACollidingRootLoadsAtLeastWhatItsOutputLoads)
{
    CarCity first(2, 2);
    CarCity second(2, 2);
    NodePtr a = prune(reader(first), "/World//car*");
    NodePtr b = prune(reader(second), "/World//car*");
    ASSERT_TRUE(a && b);
    NodePtr node = xformPrim(a, "/Grp");
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(node->connectInput(b, 1));
    KnobBool* autoRename = dynamic_cast<KnobBool*>(node->getKnobByName(kSceneReparentParamAutoRename).get());
    ASSERT_TRUE(autoRename != NULL);
    autoRename->setValue(true);
    const SceneLoadSetPtr loaded = loadingOnly({ "/Grp/World_1/Block_0" });
    const SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();

    evaluateAsViewer(node, loaded, reach);
    // Which input lands under World_1 is known only once both are read, so both load Block_0.
    EXPECT_EQ(2, second.openedBuildings());
    EXPECT_GE(2, first.openedBuildings());
    EXPECT_TRUE(SceneOps::loadChangeReaches(loaded.get(), loadingOnly({ "/Grp/World_1/Block_0", "/Grp/World_1/Block_1" }).get(), reach->getPrefixes()));

    expectParity(node, loaded);
}

TEST_F(SceneLoadSetCoverageTest, AnAssignedMaterialInputReachesTheScopeItIsGraftedUnder)
{
    CarCity city(2, 2);
    CarCity materialCity(2, 2);
    NodePtr scene = reader(city);
    NodePtr material = prune(readerOf(writeLinkedCity(materialCity)), "/World//car*");
    ASSERT_TRUE(scene && material);
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(node->connectInput(scene, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(node->connectInput(material, kUSDAssignMaterialInputMaterial));
    setString(node, kUSDAssignMaterialParamPrimPattern, "/World/Ground");
    const SceneLoadSetPtr loaded = loadingOnly({ "/World/Block_0" });
    const SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();

    evaluateAsViewer(node, loaded, reach);
    EXPECT_EQ(0, materialCity.openedBuildings());
    const std::vector<std::string> prefixes = reach->getPrefixes();
    EXPECT_TRUE(reachesUnder(prefixes, "/materials/World"));
    EXPECT_TRUE(SceneOps::loadChangeReaches(loaded.get(), loadingOnly({ "/World/Block_0", "/materials/World/Block_0" }).get(), prefixes));

    expectParity(node, loaded);
    expectParity(node, loadingOnly({ "/materials/World/Block_0" }));
}

TEST_F(SceneLoadSetCoverageTest, AGeometryMaterialInputOpensOnlyThePayloadsTheGraftLoadsAndReachesThem)
{
    PayloadMaterials materials;
    NodePtr material = readerOf(materials.rootPath());
    ASSERT_TRUE(bool(material));
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCUBE));
    ASSERT_TRUE(bool(node));
    setString(node, kSceneParamPrimPath, "/Cube");
    ASSERT_TRUE(node->connectInput(material, 0));
    const SceneLoadSetPtr loaded = loadingOnly({ "/Cube/Lib/A" });
    const SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();

    evaluateAsViewer(node, loaded, reach);
    EXPECT_EQ(1, materials.openedPayloads());
    const std::vector<std::string> prefixes = reach->getPrefixes();
    EXPECT_TRUE(reachesUnder(prefixes, "/Cube/Lib"));
    EXPECT_TRUE(SceneOps::loadChangeReaches(loaded.get(), loadingOnly({ "/Cube/Lib/A", "/Cube/Lib/B" }).get(), prefixes));

    expectParity(node, loaded);
}

TEST_F(SceneLoadSetCoverageTest, AGeometryMaterialInputWithNothingLoadedOpensNoPayload)
{
    PayloadMaterials materials;
    NodePtr material = readerOf(materials.rootPath());
    ASSERT_TRUE(bool(material));
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCUBE));
    ASSERT_TRUE(bool(node));
    setString(node, kSceneParamPrimPath, "/Cube");
    ASSERT_TRUE(node->connectInput(material, 0));

    evaluateAsViewer(node, loadingOnly({}), std::make_shared<SceneLoadReach>());
    EXPECT_EQ(0, materials.openedPayloads());
}

TEST_F(SceneLoadSetCoverageTest, AReferencedSourceInputOpensOnlyWhatTheTargetLoadsAndReachesIt)
{
    CarCity city(2, 2);
    NodePtr source = prune(reader(city), "/World//car*");
    ASSERT_TRUE(bool(source));
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREFERENCE));
    ASSERT_TRUE(bool(node));
    setChoice(node, kUSDReferenceParamSource, "input");
    setString(node, kSceneParamPrimPath, "/Ref");
    setString(node, kUSDReferenceParamSourcePrim, "/World/Block_0");
    ASSERT_TRUE(node->connectInput(source, 1));
    const SceneLoadSetPtr loaded = loadingOnly({ "/Ref/B_0" });
    const SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();

    evaluateAsViewer(node, loaded, reach);
    EXPECT_EQ(1, city.openedBuildings());
    const std::vector<std::string> prefixes = reach->getPrefixes();
    EXPECT_TRUE(reachesUnder(prefixes, "/Ref"));
    EXPECT_FALSE(reachesUnder(prefixes, "/World"));
    EXPECT_TRUE(SceneOps::loadChangeReaches(loaded.get(), loadingOnly({ "/Ref/B_0", "/Ref/B_1" }).get(), prefixes));

    expectParity(node, loaded);
}

TEST_F(SceneLoadSetCoverageTest, GraftsAndUnmappedLinksOnALoadSetAreThoseOfEverythingItLoads)
{
    CarCity city(2, 2);
    const SceneReparentArgs args = graftUnderGroup(linkedCityPayload(writeLinkedCity(city)));
    SceneGraftMemo memo;
    const SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();
    const SceneEvaluationContext viewer(SceneEvaluationContext::makeId(), loadingOnly({ "/World/Block_0" }), reach);

    const GraftReport loaded = graftIn(viewer, args, &memo);
    EXPECT_EQ(2, city.openedBuildings());
    const std::vector<std::string> prefixes = reach->getPrefixes();
    for (const char* path : { "/World/Block_0/B_0", "/World/Block_1/B_0", "/materials" }) {
        EXPECT_TRUE(std::find(prefixes.begin(), prefixes.end(), std::string(path)) != prefixes.end()) << path;
    }
    EXPECT_EQ(std::vector<std::string>({ "/Grp/World.lost -> /nowhere", "/Grp/World/Block_0/B_0/Geo.lost -> /nowhere/Inside" }), loaded.unmapped);

    const GraftReport everything = graftIn(SceneEvaluationContext(), args, &memo);
    EXPECT_EQ(everything.grafts, loaded.grafts);
    EXPECT_EQ(std::vector<std::string>({ "/Grp/World.lost -> /nowhere", "/Grp/World/Block_0/B_0/Geo.lost -> /nowhere/Inside", "/Grp/World/Block_1/B_0/Geo.lost -> /nowhere/Inside" }), everything.unmapped);
}

TEST_F(SceneLoadSetCoverageTest, AViewerGraftAndARenderGraftKeepTheirOwnMemos)
{
    CarCity city(2, 2);
    const SceneReparentArgs args = graftUnderGroup(linkedCityPayload(writeLinkedCity(city)));
    SceneGraftMemo memo;
    const SceneEvaluationContext viewer(SceneEvaluationContext::makeId(), loadingOnly({ "/World/Block_0" }), std::make_shared<SceneLoadReach>());
    const SceneEvaluationContext render;

    const GraftReport firstViewer = graftIn(viewer, args, &memo);
    const GraftReport firstRender = graftIn(render, args, &memo);
    // The two read different links, so neither can take the other's graft layer.
    EXPECT_EQ(2u, memo.getInputReadCount());
    EXPECT_EQ(2u, memo.getGraftLayerAuthorCount());
    EXPECT_FALSE(firstViewer.output.isSameStack(firstRender.output));

    for (int round = 0; round < 3; ++round) {
        const GraftReport againViewer = graftIn(viewer, args, &memo);
        const GraftReport againRender = graftIn(render, args, &memo);
        EXPECT_TRUE(againViewer.output.isSameStack(firstViewer.output)) << round;
        EXPECT_TRUE(againRender.output.isSameStack(firstRender.output)) << round;
        EXPECT_EQ(2u, againViewer.unmapped.size()) << round;
        EXPECT_EQ(3u, againRender.unmapped.size()) << round;
    }
    EXPECT_EQ(2u, memo.getInputReadCount());
    EXPECT_EQ(2u, memo.getGraftLayerAuthorCount());
}
