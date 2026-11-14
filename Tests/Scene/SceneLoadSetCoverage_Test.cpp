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
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>

#include "BaseTest.h"
#include "SceneTestAuthoring.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneReparentNode.h"
#include "Engine/Nodes/Scene/USDAssignMaterial.h"
#include "Engine/Nodes/Scene/USDCube.h"
#include "Engine/Nodes/Scene/USDEditPrim.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/USDProjectUV.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDReference.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/Nodes/Scene/USDSetAttribute.h"
#include "Engine/Nodes/Scene/USDVariant.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Project.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

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

    // The city read with its up axis and units converted to a project that differs from the file.
    NodePtr metricsReader(const CarCity& city)
    {
        NodePtr node = reader(city);
        KnobChoice* upAxis = dynamic_cast<KnobChoice*>(getApp()->getProject()->getKnobByName("upAxis").get());
        KnobBool* matchUpAxis = node ? dynamic_cast<KnobBool*>(node->getKnobByName(kUSDReadParamMatchUpAxis).get()) : nullptr;

        EXPECT_TRUE(upAxis && matchUpAxis);
        if (upAxis && matchUpAxis) {
            upAxis->setValue(1);
            matchUpAxis->setValue(true);
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

    NodePtr editTransform(const NodePtr& input,
                          const std::string& pattern)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobString* patternKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDEditTransformParamPrimPattern).get());
        KnobChoice* modeKnob = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDEditTransformParamMode).get());
        EXPECT_TRUE(patternKnob && modeKnob);
        if (patternKnob && modeKnob) {
            patternKnob->setValue(pattern);
            modeKnob->setValueFromID("replace", 0);
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

    NodePtr project3D(const NodePtr& cameraScene,
                      const std::string& cameraPath)
    {
        NodePtr plate = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));

        EXPECT_TRUE(plate && node);
        if (!plate || !node) {
            return NodePtr();
        }
        KnobString* cameraKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDProject3DParamCameraPath).get());
        EXPECT_TRUE(cameraKnob != nullptr);
        if (cameraKnob) {
            cameraKnob->setValue(cameraPath);
        }
        EXPECT_TRUE(node->connectInput(plate, kUSDProject3DInputImage));
        EXPECT_TRUE(node->connectInput(cameraScene, kUSDProject3DInputCamera));

        return node;
    }

    // Whether a projection shader in payload has a camera, its matrix not the zero one a missing
    // camera authors.
    static bool hasProjection(const ScenePayload& payload)
    {
        const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);

        if (!stage) {
            return false;
        }
        for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
            PXR_NS::GfMatrix4d matrix(0.);
            const PXR_NS::UsdShadeShader shader(prim);

            if (shader && shader.GetInput(PXR_NS::TfToken("worldToProjector")) && shader.GetInput(PXR_NS::TfToken("worldToProjector")).Get(&matrix, PXR_NS::UsdTimeCode::Default())) {
                return matrix != PXR_NS::GfMatrix4d(0.);
            }
        }

        return false;
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
 * on /World itself and on the Geo of /World/Block_0/B_0 and of /World/Block_1/B_0, below their
 * payloads, each to /Props/Lamp and to a prim no root holds. Adds the roots /Props and
 * /materials. Returns its path.
 *
 * The links are opinions of this layer, not of the building files, so every load set composes
 * them: USD ignores a link authored inside a payload whose target lies outside it.
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

/**
 * @brief Writes, next to city's root layer, a layer sublayering it that adds the root /Extra: an
 * over whose payload, extra.usda, defines it. Until that payload loads /Extra defines nothing, so
 * a graft has no root there. Returns the layer's path and sets *payloadPath to extra.usda's.
 **/
std::string
writePayloadRootCity(const CarCity& city,
                     std::string* payloadPath)
{
    const std::filesystem::path dir = std::filesystem::path(city.rootPath()).parent_path();
    const std::filesystem::path path = dir / "payloadroot.usda";
    *payloadPath = (dir / "extra.usda").string();
    std::ofstream payload(*payloadPath);
    payload << "#usda 1.0\n(\n    defaultPrim = \"Holder\"\n)\ndef Xform \"Holder\"\n{\n    def Cube \"Geo\"\n    {\n    }\n}\n";
    std::ofstream file(path.string());
    file << "#usda 1.0\n(\n    defaultPrim = \"World\"\n    upAxis = \"Y\"\n    metersPerUnit = 1\n    subLayers = [@./root.usda@]\n)\n"
         << "over \"Extra\" (\n    prepend payload = @./extra.usda@\n)\n{\n}\n";

    return path.string();
}

/**
 * @brief Writes, next to city's root layer, a layer sublayering it that adds /Props/Lamp with a
 * relationship aim to /World/Block_1/B_0/Geo, which only that building's payload defines.
 * Returns its path.
 **/
std::string
writeAimedCity(const CarCity& city)
{
    const std::filesystem::path path = std::filesystem::path(city.rootPath()).parent_path() / "aimed.usda";
    std::ofstream file(path.string());

    file << "#usda 1.0\n(\n    defaultPrim = \"World\"\n    upAxis = \"Y\"\n    metersPerUnit = 1\n    subLayers = [@./root.usda@]\n)\n"
         << "def Xform \"Props\"\n{\n    def Xform \"Lamp\"\n    {\n        rel aim = </World/Block_1/B_0/Geo>\n    }\n}\n";

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
        SceneQueryPool::Lease lease = pool.borrow(payload, SceneLoadSet::everything(), &error);
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

TEST_F(SceneLoadSetCoverageTest, AProject3DEvaluatesTheSameOnALoadSetAsOnEverything)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr firstCamera = project3D(source, std::string());
    NodePtr namedCamera = project3D(source, "/World/Block_0/B_0/Cam");
    ASSERT_TRUE(source && firstCamera && namedCamera);

    expectParity(firstCamera, loadingOnly({ "/World/Block_0" }));
    expectParity(namedCamera, loadingOnly({ "/World/Block_0" }));
}

TEST_F(SceneLoadSetCoverageTest, AProject3DCameraInsideAPayloadResolvesWithOnlyItsBuildingOpen)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = project3D(source, "/World/Block_0/B_0/Cam");
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
    EXPECT_FALSE(message.contains(QString::fromUtf8("no camera"), Qt::CaseInsensitive)) << message.toStdString();
    EXPECT_TRUE(hasProjection(payload));
}

TEST_F(SceneLoadSetCoverageTest, AMetricsCorrectionOpensNoBuildingWithNothingLoaded)
{
    CarCity city(2, 2);
    NodePtr node = metricsReader(city);
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

TEST_F(SceneLoadSetCoverageTest, AMetricsCorrectionEvaluatesTheSameOnALoadSetAsOnEverything)
{
    CarCity city(2, 2);
    NodePtr node = metricsReader(city);
    ASSERT_TRUE(bool(node));

    expectParity(node, loadingOnly({ "/World/Block_0" }));
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

TEST_F(SceneLoadSetCoverageTest, AWrapRetargetsALinkIntoAnUnloadedPayloadTheSameOnceItLoads)
{
    CarCity city(2, 2);
    NodePtr node = xformPrim(readerOf(writeAimedCity(city)), "/World");
    ASSERT_TRUE(bool(node));
    setChoice(node, kSceneReparentParamMode, "wrap");
    setString(node, kSceneReparentParamWrapName, "Grp");
    const std::string link = "/Props/Lamp.aim";
    const SceneLoadSetPtr none = std::make_shared<const SceneLoadSet>(SceneLoadSet::none());
    const SceneLoadSetPtr loaded = loadingOnly({ "/Grp/World/Block_1" });
    const SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();
    const std::vector<std::string> moved(1, "/Grp/World/Block_1/B_0/Geo");

    ScenePayload before;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), SceneEvaluationContext(SceneEvaluationContext::makeId(), none, reach)), &before, &error))
        << error;
    EXPECT_EQ(0, city.openedBuildings());
    EXPECT_EQ(moved, relationshipTargets(before, none, link));
    EXPECT_TRUE(SceneOps::loadChangeReaches(none.get(), loaded.get(), reach->getPrefixes()));

    ScenePayload after;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), SceneEvaluationContext(SceneEvaluationContext::makeId(), loaded, std::make_shared<SceneLoadReach>())), &after, &error))
        << error;
    EXPECT_EQ(2, city.openedBuildings());
    EXPECT_EQ(moved, relationshipTargets(after, loaded, link));
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
    // The links sit in the root layer stack, so an unloaded building still composes its Geo's.
    const std::vector<std::string> allLinks({ "/Grp/World.lost -> /nowhere", "/Grp/World/Block_0/B_0/Geo.lost -> /nowhere/Inside", "/Grp/World/Block_1/B_0/Geo.lost -> /nowhere/Inside" });
    EXPECT_EQ(allLinks, loaded.unmapped);

    const GraftReport everything = graftIn(SceneEvaluationContext(), args, &memo);
    EXPECT_EQ(4, city.openedBuildings());
    EXPECT_FALSE(everything.grafts.empty());
    EXPECT_EQ(everything.grafts, loaded.grafts);
    EXPECT_EQ(allLinks, everything.unmapped);
}

TEST_F(SceneLoadSetCoverageTest, AViewerGraftAndARenderGraftKeepTheirOwnMemos)
{
    CarCity city(2, 2);
    std::string extraPath;
    const SceneReparentArgs args = graftUnderGroup(linkedCityPayload(writePayloadRootCity(city, &extraPath)));
    SceneGraftMemo memo;
    const SceneLoadReachPtr reach = std::make_shared<SceneLoadReach>();
    const SceneEvaluationContext viewer(SceneEvaluationContext::makeId(), loadingOnly({ "/World/Block_0" }), reach);
    const SceneEvaluationContext render;
    const auto graftsExtra = [](const GraftReport& report) {
        return std::any_of(report.grafts.begin(), report.grafts.end(), [](const std::string& graft) { return graft.compare(0, 20, "/Extra -> /Grp/Extra") == 0; });
    };

    const GraftReport firstViewer = graftIn(viewer, args, &memo);
    EXPECT_EQ(2, city.openedBuildings());
    EXPECT_FALSE(bool(PXR_NS::SdfLayer::Find(extraPath)));
    EXPECT_FALSE(graftsExtra(firstViewer));
    EXPECT_TRUE(reachesUnder(reach->getPrefixes(), "/Extra"));

    const GraftReport firstRender = graftIn(render, args, &memo);
    EXPECT_TRUE(graftsExtra(firstRender));
    // The two read different roots, so neither can take the other's read or graft layer.
    EXPECT_EQ(2u, memo.getInputReadCount());
    EXPECT_EQ(2u, memo.getGraftLayerAuthorCount());
    EXPECT_FALSE(firstViewer.output.isSameStack(firstRender.output));

    for (int round = 0; round < 3; ++round) {
        const GraftReport againViewer = graftIn(viewer, args, &memo);
        const GraftReport againRender = graftIn(render, args, &memo);
        EXPECT_TRUE(againViewer.output.isSameStack(firstViewer.output)) << round;
        EXPECT_TRUE(againRender.output.isSameStack(firstRender.output)) << round;
        EXPECT_EQ(firstViewer.grafts, againViewer.grafts) << round;
        EXPECT_EQ(firstRender.grafts, againRender.grafts) << round;
    }
    EXPECT_EQ(2u, memo.getInputReadCount());
    EXPECT_EQ(2u, memo.getGraftLayerAuthorCount());

    // A second viewer on the same load set reads what the first did, so it takes the first's.
    const SceneEvaluationContext otherViewer(SceneEvaluationContext::makeId(), loadingOnly({ "/World/Block_0" }), std::make_shared<SceneLoadReach>());
    const GraftReport other = graftIn(otherViewer, args, &memo);
    EXPECT_EQ(firstViewer.grafts, other.grafts);
    EXPECT_TRUE(other.output.isSameStack(firstViewer.output));
    EXPECT_EQ(2u, memo.getInputReadCount());
    EXPECT_EQ(2u, memo.getGraftLayerAuthorCount());
}

namespace {
// A layer over city whose active render settings prim renders 320x200 through the camera inside
// the first building.
std::string
writeRenderSettingsWrapper(const CarCity& city)
{
    const std::string wrapper = (std::filesystem::path(city.rootPath()).parent_path() / "withSettings.usda").string();
    std::ofstream out(wrapper);

    out << "#usda 1.0\n(\n    subLayers = [@" << city.rootPath() << "@]\n    renderSettingsPrimPath = \"/Render/Settings\"\n)\n"
        << "def Scope \"Render\"\n{\n    def RenderSettings \"Settings\"\n    {\n"
        << "        rel camera = </World/Block_0/B_0/Cam>\n        int2 resolution = (320, 200)\n    }\n}\n";

    return wrapper;
}

std::string
primIdKeyOf(const std::string& path)
{
    char key[32];
    std::snprintf(key, sizeof(key), kUSDRenderMetadataPrimIdPrefix "%08x", HydraRender::primPathHash(path));

    return key;
}

// The region of definition effect gives the main thread at time once the settings worker stored
// expected, polled for up to ten seconds; the last one given when it never does.
RectD
mainThreadRodOnceRead(EffectInstance* effect,
                      U64 hash,
                      double time,
                      const RectD& expected)
{
    RectD rod;
    for (int i = 0; i < 1000; ++i) {
        QCoreApplication::processEvents();
        EXPECT_EQ(eStatusOK, effect->getRegionOfDefinition(hash, time, RenderScale::identity, ViewIdx(0), &rod));
        if (rod == expected) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    QCoreApplication::processEvents();

    return rod;
}
} // anonymous namespace

TEST_F(SceneLoadSetCoverageTest, AUSDRenderReadsItsResolutionOpeningNoBuilding)
{
    CarCity city(2, 2);
    const std::string wrapper = writeRenderSettingsWrapper(city);
    NodePtr source = readerOf(wrapper);
    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(source && render);
    ASSERT_TRUE(render->connectInput(source, 0));

    // The main thread evaluates nothing: it is given a provisional format while the worker it
    // starts reads the settings prim, which, like any reader off the main thread, opens no building.
    EffectInstance* effect = render->getEffectInstance().get();
    const U64 hash = render->getHashValue();
    RectD rod;
    ASSERT_EQ(eStatusOK, effect->getRegionOfDefinition(hash, 1., RenderScale::identity, ViewIdx(0), &rod));
    EXPECT_EQ(0, city.openedBuildings());
    EXPECT_EQ(RectD(0., 0., 320., 200.), mainThreadRodOnceRead(effect, hash, 1., RectD(0., 0., 320., 200.)));
    EXPECT_EQ(0, city.openedBuildings());

    const RectD offMain = std::async(std::launch::async, [effect, hash]() {
        RectD r;
        EXPECT_EQ(eStatusOK, effect->getRegionOfDefinition(hash, 2., RenderScale::identity, ViewIdx(0), &r));

        return r; }).get();
    EXPECT_EQ(RectD(0., 0., 320., 200.), offMain);
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, AUSDRendersOutputMetadataOpensNoBuildingOnTheMainThreadAndArrivesWhole)
{
    CarCity city(2, 2);
    const std::string wrapper = writeRenderSettingsWrapper(city);
    NodePtr source = readerOf(wrapper);
    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(source && render);
    ASSERT_TRUE(render->connectInput(source, 0));
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(render->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);

    // The table names every drawable prim, the buildings' included, which the main thread never
    // opens; nor does the output format, which the settings worker reads off the settings prim alone.
    const ImageMetadata provisional = effect->getOutputMetadata(1., ViewIdx(0));
    EXPECT_EQ(0, city.openedBuildings());
    EXPECT_TRUE(provisional.empty());
    // Not cached: asking again still opens nothing.
    EXPECT_TRUE(effect->getOutputMetadata(1., ViewIdx(0)).empty());
    EXPECT_EQ(0, city.openedBuildings());
    EXPECT_EQ(RectD(0., 0., 320., 200.), mainThreadRodOnceRead(effect, render->getHashValue(), 1., RectD(0., 0., 320., 200.)));
    EXPECT_EQ(0, city.openedBuildings());

    const U64 revision = render->getMetadataRevision();
    const ImageMetadata whole = std::async(std::launch::async, [effect]() { return effect->getOutputMetadata(1., ViewIdx(0)); }).get();
    std::vector<std::string> expected(1, "/World/Ground");
    for (int b = 0; b < 2; ++b) {
        for (int n = 0; n < 2; ++n) {
            const std::string building = "/World/Block_" + std::to_string(b) + "/B_" + std::to_string(n);
            expected.push_back(building + "/Geo");
            expected.push_back(building + "/Panel");
        }
    }
    EXPECT_EQ(expected.size(), whole.size());
    for (const std::string& path : expected) {
        EXPECT_EQ(std::optional<std::string>(path), whole.getString(primIdKeyOf(path))) << path;
    }

    // The main thread is served the stored table, and the readers it gave the empty one are told to ask again.
    EXPECT_EQ(whole, effect->getOutputMetadata(1., ViewIdx(0)));
    EXPECT_EQ(revision, render->getMetadataRevision());
    QCoreApplication::processEvents();
    EXPECT_LT(revision, render->getMetadataRevision());
    EXPECT_EQ(whole, effect->getOutputMetadata(1., ViewIdx(0)));
}

TEST_F(SceneLoadSetCoverageTest, AnEditTransformKeepsEachContextsMatchesApart)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = editTransform(source, "/World//car*");
    ASSERT_TRUE(source && node);
    USDEditTransform* edit = dynamic_cast<USDEditTransform*>(node->getEffectInstance().get());
    ASSERT_TRUE(edit != nullptr);
    const SceneEvaluationContext none(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    const SceneEvaluationContext block(SceneEvaluationContext::makeId(), loadingOnly({ "/World/Block_0" }));
    const std::string car = carPath(0, 0);

    for (int round = 0; round < 2; ++round) {
        for (const SceneEvaluationContext* context : { &none, &block }) {
            std::string error;
            ScenePayload payload;
            ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                      node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), *context), &payload, &error))
                << error;
        }
    }
    EXPECT_FALSE(edit->transformsPrim(car, none));
    EXPECT_TRUE(edit->transformsPrim(car, block));

    // A pattern changed since: each context matches it on its own load set, once.
    KnobString* patternKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDEditTransformParamPrimPattern).get());
    ASSERT_TRUE(patternKnob != nullptr);
    patternKnob->setValue(std::string("/World/Block_0//car*"));
    const U64 before = edit->getStageReadCount();
    for (int round = 0; round < 3; ++round) {
        EXPECT_FALSE(edit->transformsPrim(car, none));
        EXPECT_TRUE(edit->transformsPrim(car, block));
    }
    EXPECT_EQ(before + 2, edit->getStageReadCount());
}

TEST_F(SceneLoadSetCoverageTest, AnEditTransformsStaleMatchesOnNothingLoadedOpenNoBuilding)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = editTransform(source, "/World/Block_0/B_0/car*");
    ASSERT_TRUE(source && node);
    USDEditTransform* edit = dynamic_cast<USDEditTransform*>(node->getEffectInstance().get());
    ASSERT_TRUE(edit != nullptr);
    const SceneEvaluationContext none(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    std::string error;
    ScenePayload payload;

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), none), &payload, &error))
        << error;
    KnobString* patternKnob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDEditTransformParamPrimPattern).get());
    ASSERT_TRUE(patternKnob != nullptr);
    patternKnob->setValue(std::string("/World//car*"));
    EXPECT_FALSE(edit->transformsPrim(carPath(0, 0), none));
    EXPECT_EQ(0, city.openedBuildings());
}

namespace {
std::shared_ptr<CentrePivotJob>
centreIn(USDEditTransform* edit,
         const NodePtr& node,
         const SceneEvaluationContext* context)
{
    if (context) {
        std::string error;
        ScenePayload payload;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), *context), &payload, &error))
            << error;
    }

    return edit->startCentrePivot(context);
}
} // anonymous namespace

TEST_F(SceneLoadSetCoverageTest, ACentrePivotOnLoadedPrimsEqualsTheFullyLoadedCentre)
{
    CarCity city(2, 2, true);
    NodePtr source = reader(city);
    NodePtr node = editTransform(source, "/World/Block_0/B_*");
    ASSERT_TRUE(source && node);
    USDEditTransform* edit = dynamic_cast<USDEditTransform*>(node->getEffectInstance().get());
    ASSERT_TRUE(edit != nullptr);
    const SceneEvaluationContext block(SceneEvaluationContext::makeId(), loadingOnly({ "/World/Block_0" }));
    const SceneEvaluationContext everything(SceneEvaluationContext::makeId(), SceneLoadSetPtr());

    // One at a time: a new request cancels the one before it.
    std::shared_ptr<CentrePivotJob> covered = centreIn(edit, node, &block);
    ASSERT_EQ(CentrePivotJob::eStateDone, covered->wait()) << covered->getError();
    std::shared_ptr<CentrePivotJob> full = centreIn(edit, node, &everything);
    ASSERT_EQ(CentrePivotJob::eStateDone, full->wait()) << full->getError();
    double a[3], b[3];
    covered->getCentre(&a[0], &a[1], &a[2]);
    full->getCentre(&b[0], &b[1], &b[2]);
    for (int d = 0; d < 3; ++d) {
        EXPECT_DOUBLE_EQ(b[d], a[d]) << d;
    }
    // The loaded content fills the box around the origin, not the hint authored beside it.
    EXPECT_DOUBLE_EQ(0., a[0]);
    EXPECT_DOUBLE_EQ(0., a[1]);
    EXPECT_DOUBLE_EQ(0., a[2]);
}

TEST_F(SceneLoadSetCoverageTest, ACentrePivotOnNothingLoadedUsesExtentsHintsAndOpensNoBuilding)
{
    CarCity city(2, 2, true);
    NodePtr source = reader(city);
    NodePtr node = editTransform(source, "/World/Block_0");
    ASSERT_TRUE(source && node);
    USDEditTransform* edit = dynamic_cast<USDEditTransform*>(node->getEffectInstance().get());
    ASSERT_TRUE(edit != nullptr);
    const SceneEvaluationContext none(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));

    std::shared_ptr<CentrePivotJob> job = centreIn(edit, node, &none);
    ASSERT_EQ(CentrePivotJob::eStateDone, job->wait()) << job->getError();
    double centre[3];
    job->getCentre(&centre[0], &centre[1], &centre[2]);
    EXPECT_DOUBLE_EQ(1., centre[0]);
    EXPECT_DOUBLE_EQ(2., centre[1]);
    EXPECT_DOUBLE_EQ(3., centre[2]);
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, ACentrePivotOnMatchesInsideUnloadedPayloadsUsesThePayloadsHint)
{
    CarCity city(2, 2, true);
    NodePtr source = reader(city);
    NodePtr node = editTransform(source, "/World/Block_0/B_*/car_0");
    ASSERT_TRUE(source && node);
    USDEditTransform* edit = dynamic_cast<USDEditTransform*>(node->getEffectInstance().get());
    ASSERT_TRUE(edit != nullptr);
    const SceneEvaluationContext everything(SceneEvaluationContext::makeId(), SceneLoadSetPtr());
    const SceneEvaluationContext none(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    std::string error;
    ScenePayload payload;

    // The matches come from a fully loaded evaluation; the pivot is taken where none is loaded.
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), everything), &payload, &error))
        << error;
    // The idle fully loaded stage would otherwise be reused by the job, whose unloads close
    // buildings and hide whether it opened any.
    SceneQueryPool::instance().releaseIdle();
    ASSERT_EQ(0, city.openedBuildings());
    std::shared_ptr<CentrePivotJob> job = edit->startCentrePivot(&none);
    ASSERT_EQ(CentrePivotJob::eStateDone, job->wait()) << job->getError();
    double centre[3];
    job->getCentre(&centre[0], &centre[1], &centre[2]);
    EXPECT_DOUBLE_EQ(1., centre[0]);
    EXPECT_DOUBLE_EQ(2., centre[1]);
    EXPECT_DOUBLE_EQ(3., centre[2]);
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, ACentrePivotOnUnloadedPrimsWithoutHintsFailsAndOpensNoBuilding)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = editTransform(source, "/World/Block_0");
    ASSERT_TRUE(source && node);
    USDEditTransform* edit = dynamic_cast<USDEditTransform*>(node->getEffectInstance().get());
    ASSERT_TRUE(edit != nullptr);
    const SceneEvaluationContext none(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));

    std::shared_ptr<CentrePivotJob> job = centreIn(edit, node, &none);
    EXPECT_EQ(CentrePivotJob::eStateFailed, job->wait());
    EXPECT_FALSE(job->getError().empty());
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, ASecondCentrePivotRequestCancelsTheFirst)
{
    CarCity city(2, 2, true);
    NodePtr source = reader(city);
    NodePtr node = editTransform(source, "/World/Block_0");
    ASSERT_TRUE(source && node);
    USDEditTransform* edit = dynamic_cast<USDEditTransform*>(node->getEffectInstance().get());
    ASSERT_TRUE(edit != nullptr);
    KnobDoublePtr pivot = edit->getPivotKnob();
    ASSERT_TRUE(bool(pivot));
    const SceneEvaluationContext none(SceneEvaluationContext::makeId(), std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));

    std::shared_ptr<CentrePivotJob> first = centreIn(edit, node, &none);
    std::shared_ptr<CentrePivotJob> second = edit->startCentrePivot(&none);
    EXPECT_TRUE(first->isCancelled());
    EXPECT_FALSE(second->isCancelled());
    first->wait();
    second->wait();

    EXPECT_FALSE(edit->applyCentrePivot(first));
    for (int d = 0; d < 3; ++d) {
        EXPECT_DOUBLE_EQ(0., pivot->getValue(d)) << d;
    }
    EXPECT_TRUE(edit->applyCentrePivot(second));
    EXPECT_DOUBLE_EQ(1., pivot->getValue(0));
    EXPECT_DOUBLE_EQ(2., pivot->getValue(1));
    EXPECT_DOUBLE_EQ(3., pivot->getValue(2));
}

namespace {
// The ids of menu's entries.
std::vector<std::string>
entryIds(const KnobChoice* menu)
{
    std::vector<std::string> ids;

    for (const ChoiceOption& option : menu->getEntries_mt_safe()) {
        ids.push_back(option.id);
    }

    return ids;
}
} // anonymous namespace

TEST_F(SceneLoadSetCoverageTest, ACameraMenuRefreshReturnsBeforeTheMenuFillsAndCountsTheUnsearchedBranches)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
    ASSERT_TRUE(source && node);
    ASSERT_TRUE(node->connectInput(source, 0));
    USDRenderSettings* effect = dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get());
    KnobChoice* menu = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDRenderSettingsParamCamera).get());
    ASSERT_TRUE(effect && menu);
    const std::vector<std::string> before = entryIds(menu);

    const std::shared_ptr<SceneMenuJob> job = effect->refreshCameraMenu();
    ASSERT_TRUE(bool(job));
    EXPECT_EQ(before, entryIds(menu));
    job->wait();
    // Filled on the main thread only, which this test is and which has not run the queued fill.
    EXPECT_EQ(before, entryIds(menu));
    EXPECT_TRUE(job->hasNoViewer());
    ASSERT_TRUE(effect->applyCameraMenu(job));

    // No viewer shows the node: nothing is loaded, and the camera inside a building is not found.
    const std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();
    ASSERT_EQ((std::size_t)1, entries.size());
    EXPECT_EQ(std::string(kSceneMenuUnsearchedId), entries[0].id);
    EXPECT_EQ(std::string("4 unloaded branches not searched"), entries[0].label);
    EXPECT_EQ(0, city.openedBuildings());
    EXPECT_FALSE(effect->applyCameraMenu(job));
}

TEST_F(SceneLoadSetCoverageTest, ACameraMenuOnACameraInputLoadsOnlyTheChosenCamerasBuilding)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = project3D(source, "/World/Block_0/B_0/Cam");
    ASSERT_TRUE(source && node);
    USDProject3D* effect = dynamic_cast<USDProject3D*>(node->getEffectInstance().get());
    KnobChoice* menu = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDProject3DParamCamera).get());
    ASSERT_TRUE(effect && menu);

    const std::shared_ptr<SceneMenuJob> job = effect->refreshCameraMenu();
    job->wait();
    ASSERT_TRUE(effect->applyCameraMenu(job));

    const std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();
    ASSERT_EQ((std::size_t)2, entries.size());
    EXPECT_EQ(std::string("/World/Block_0/B_0/Cam"), entries[0].id);
    EXPECT_EQ(std::string(kSceneMenuUnsearchedId), entries[1].id);
    EXPECT_EQ(std::string("3 unloaded branches not searched"), entries[1].label);
    EXPECT_EQ(0, menu->getValue());
    EXPECT_LE(city.openedBuildings(), 1);

    // The note is disabled, and a script that sets it anyway leaves the camera path as it was.
    EXPECT_TRUE(entries[1].disabled);
    EXPECT_TRUE(menu->isEntryDisabled(1));
    EXPECT_FALSE(menu->isEntryDisabled(0));
    menu->setValue(1);
    menu->onValueChanged(1, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    KnobString* path = dynamic_cast<KnobString*>(node->getKnobByName(kUSDProject3DParamCameraPath).get());
    ASSERT_TRUE(path != nullptr);
    EXPECT_EQ(std::string("/World/Block_0/B_0/Cam"), path->getValue());
}

TEST_F(SceneLoadSetCoverageTest, AVariantMenuOnAnUnloadedPrimNotesItsBranchAndOpensNoBuilding)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = variant(source, "/World/Block_0/B_0");
    ASSERT_TRUE(source && node);
    USDVariant* effect = dynamic_cast<USDVariant*>(node->getEffectInstance().get());
    KnobChoice* sets = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDVariantParamVariantSets).get());
    ASSERT_TRUE(effect && sets);
    const std::vector<std::string> before = entryIds(sets);

    const std::shared_ptr<SceneMenuJob> job = effect->refreshVariantMenus();
    EXPECT_EQ(before, entryIds(sets));
    job->wait();
    EXPECT_EQ(before, entryIds(sets));
    ASSERT_TRUE(effect->applyVariantMenus(job));

    // The building's variant set is authored inside its payload, which nothing loads.
    const std::vector<ChoiceOption> entries = sets->getEntries_mt_safe();
    ASSERT_EQ((std::size_t)1, entries.size());
    EXPECT_EQ(std::string(kSceneMenuUnsearchedId), entries[0].id);
    EXPECT_EQ(std::string("1 unloaded branches not searched"), entries[0].label);
    EXPECT_TRUE(sets->isEntryDisabled(0));
    EXPECT_EQ(0, city.openedBuildings());
}

TEST_F(SceneLoadSetCoverageTest, ASecondMenuRefreshSupersedesTheFirst)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = project3D(source, "/World/Block_0/B_0/Cam");
    ASSERT_TRUE(source && node);
    USDProject3D* effect = dynamic_cast<USDProject3D*>(node->getEffectInstance().get());
    KnobChoice* menu = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDProject3DParamCamera).get());
    ASSERT_TRUE(effect && menu);
    const std::vector<std::string> before = entryIds(menu);

    const std::shared_ptr<SceneMenuJob> first = effect->refreshCameraMenu();
    const std::shared_ptr<SceneMenuJob> second = effect->refreshCameraMenu();
    EXPECT_TRUE(first->isCancelled());
    EXPECT_FALSE(second->isCancelled());
    first->wait();
    second->wait();

    EXPECT_FALSE(effect->applyCameraMenu(first));
    EXPECT_EQ(before, entryIds(menu));
    EXPECT_TRUE(effect->applyCameraMenu(second));
    EXPECT_EQ((std::vector<std::string> { "/World/Block_0/B_0/Cam", kSceneMenuUnsearchedId }), entryIds(menu));
    EXPECT_FALSE(effect->applyCameraMenu(first));
}

TEST_F(SceneLoadSetCoverageTest, AVariantMenuAboveAGraftSearchesTheSetMappedToItsOwnPaths)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = variant(source, "/World//B_*");
    NodePtr graft = xformPrim(node, "/Grp");
    ASSERT_TRUE(source && node && graft);
    const SceneEvaluationContext viewer(SceneEvaluationContext::makeId(), loadingOnly({ "/Grp/World/Block_0" }));

    SceneEvaluationContext mapped;
    ASSERT_TRUE(SceneNodeAuthoring::mapContextDownToNode(graft->getEffectInstance().get(), node->getEffectInstance().get(), viewer, 1., ViewIdx(0), &mapped));
    ASSERT_TRUE(mapped.loadSet != nullptr);
    EXPECT_TRUE(*mapped.loadSet == *loadingOnly({ "/World/Block_0" }));

    // The menu job borrows the variant's input on the context: the other block's buildings stay unsearched.
    const SceneMenuJob job(false);
    ScenePayload scene;
    SceneQueryPool::Lease lease;
    ASSERT_TRUE(SceneNodeAuthoring::borrowMenuScene(source->getEffectInstance(), 1., mapped, job, &scene, &lease));
    EXPECT_EQ((std::size_t)2, SceneNodeAuthoring::countMenuUnsearched(lease.getStage(), mapped));
    SceneQueryPool::Lease unmappedLease;
    ScenePayload unmappedScene;
    ASSERT_TRUE(SceneNodeAuthoring::borrowMenuScene(source->getEffectInstance(), 1., viewer, job, &unmappedScene, &unmappedLease));
    EXPECT_EQ((std::size_t)4, SceneNodeAuthoring::countMenuUnsearched(unmappedLease.getStage(), viewer));
}

TEST_F(SceneLoadSetCoverageTest, ACentrePivotAboveAGraftUsesTheMappedSet)
{
    CarCity city(2, 2, true);
    NodePtr source = reader(city);
    NodePtr node = editTransform(source, "/World/Block_0/B_*");
    NodePtr graft = xformPrim(node, "/Grp");
    ASSERT_TRUE(source && node && graft);
    USDEditTransform* edit = dynamic_cast<USDEditTransform*>(node->getEffectInstance().get());
    ASSERT_TRUE(edit != nullptr);
    const SceneEvaluationContext viewer(SceneEvaluationContext::makeId(), loadingOnly({ "/Grp/World/Block_0" }));

    SceneEvaluationContext mapped;
    ASSERT_TRUE(SceneNodeAuthoring::mapContextDownToNode(graft->getEffectInstance().get(), node->getEffectInstance().get(), viewer, 1., ViewIdx(0), &mapped));
    std::shared_ptr<CentrePivotJob> job = centreIn(edit, node, &mapped);
    ASSERT_EQ(CentrePivotJob::eStateDone, job->wait()) << job->getError();
    double centre[3];
    job->getCentre(&centre[0], &centre[1], &centre[2]);
    for (int d = 0; d < 3; ++d) {
        EXPECT_DOUBLE_EQ(0., centre[d]) << d;
    }
}

TEST_F(SceneLoadSetCoverageTest, ADiamondOfGraftsUnitesTheSetsOfItsRoutes)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr left = xformPrim(source, "/Left");
    NodePtr right = xformPrim(source, "/Right");
    ASSERT_TRUE(source && left && right);
    NodePtr joined = merge({ left, right });
    ASSERT_TRUE(bool(joined));
    const SceneEvaluationContext viewer(SceneEvaluationContext::makeId(), loadingOnly({ "/Left/World/Block_0", "/Right/World/Block_1" }));

    SceneEvaluationContext mapped;
    ASSERT_TRUE(SceneNodeAuthoring::mapContextDownToNode(joined->getEffectInstance().get(), source->getEffectInstance().get(), viewer, 1., ViewIdx(0), &mapped));
    ASSERT_TRUE(mapped.loadSet != nullptr);
    EXPECT_TRUE(*mapped.loadSet == *loadingOnly({ "/World/Block_0", "/World/Block_1" }));

    SceneEvaluationContext unrelated;
    EXPECT_FALSE(SceneNodeAuthoring::mapContextDownToNode(left->getEffectInstance().get(), joined->getEffectInstance().get(), viewer, 1., ViewIdx(0), &unrelated));
}

TEST_F(SceneLoadSetCoverageTest, AViewerLoadReachingWhatAVariantMenuSearchedRefillsIt)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = variant(source, "/World/Block_0/B_0");
    ASSERT_TRUE(source && node);
    CreateNodeArgs viewerArgs(PLUGINID_NATRON_VIEWER, getApp()->getProject());
    viewerArgs.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
    NodePtr viewerNode = getApp()->createNode(viewerArgs);
    ASSERT_TRUE(bool(viewerNode));
    ViewerInstance* viewer = viewerNode->isEffectViewer();
    ASSERT_TRUE(viewer != nullptr);
    ASSERT_TRUE(viewerNode->connectInput(node, 0));
    viewer->setSceneLoadSet(std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    USDVariant* effect = dynamic_cast<USDVariant*>(node->getEffectInstance().get());
    KnobChoice* sets = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDVariantParamVariantSets).get());
    KnobChoice* variants = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDVariantParamVariants).get());
    ASSERT_TRUE(effect && sets && variants);

    const std::shared_ptr<SceneMenuJob> first = effect->refreshVariantMenus();
    first->wait();
    EXPECT_FALSE(first->hasNoViewer());
    ASSERT_TRUE(effect->applyVariantMenus(first));
    EXPECT_EQ((std::vector<std::string> { kSceneMenuUnsearchedId }), entryIds(sets));
    EXPECT_EQ(std::string("1 unloaded branches not searched"), sets->getEntry(0).label);

    // Every refresh followLoadChanges() starts is waited for, any other node's included, so none
    // outlives the test; only this node's own job fills its menus.
    const auto follow = [effect]() {
        bool applied = false;
        for (const std::shared_ptr<SceneMenuJob>& job : SceneMenuRefresh::followLoadChanges()) {
            job->wait();
            applied = effect->applyVariantMenus(job) || applied;
        }

        return applied;
    };

    // The other block holds nothing the search read: the menu is left as it is.
    viewer->setSceneLoadSet(loadingOnly({ "/World/Block_1" }));
    EXPECT_FALSE(follow());
    EXPECT_EQ((std::vector<std::string> { kSceneMenuUnsearchedId }), entryIds(sets));

    // The building's own block: its variant set is found, and nothing is left unsearched.
    viewer->setSceneLoadSet(loadingOnly({ "/World/Block_0" }));
    EXPECT_TRUE(follow());
    EXPECT_EQ((std::vector<std::string> { "look" }), entryIds(sets));
    EXPECT_EQ((std::vector<std::string> { "plain", "tinted" }), entryIds(variants));
}
