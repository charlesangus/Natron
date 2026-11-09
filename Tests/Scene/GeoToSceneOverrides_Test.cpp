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

#include <set>
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
#include "Engine/Nodes/Geometry/GeoToUSD.h"
#include "Engine/Nodes/Geometry/GeoTransform.h"
#include "Engine/Nodes/Geometry/USDToGeo.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// /World is translated, rotated and scaled non-uniformly, so a transform written without its
// parent's inverse lands elsewhere.
const char* const kScene = "#usda 1.0\n"
                           "(\n"
                           "    metersPerUnit = 0.01\n"
                           "    upAxis = \"Y\"\n"
                           ")\n"
                           "\n"
                           "def Xform \"World\"\n"
                           "{\n"
                           "    double3 xformOp:translate = (5, 0, 0)\n"
                           "    float xformOp:rotateZ = 90\n"
                           "    double3 xformOp:scale = (2, 3, 2)\n"
                           "    uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateZ\", \"xformOp:scale\"]\n"
                           "\n"
                           "    def Mesh \"A\"\n"
                           "    {\n"
                           "        int[] faceVertexCounts = [4]\n"
                           "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                           "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                           "        double3 xformOp:translate = (1, 0, 0)\n"
                           "        float xformOp:rotateX = 30\n"
                           "        uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateX\"]\n"
                           "    }\n"
                           "\n"
                           "    def Cube \"C\"\n"
                           "    {\n"
                           "        double3 xformOp:translate = (0, 2, 0)\n"
                           "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                           "    }\n"
                           "}\n";

const double kShift = 5.;

std::set<std::string>
primPaths(const PXR_NS::UsdStageRefPtr& stage)
{
    std::set<std::string> paths;
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        paths.insert(prim.GetPath().GetString());
    }

    return paths;
}

PXR_NS::GfMatrix4d
worldOf(const PXR_NS::UsdStageRefPtr& stage,
        const char* path)
{
    PXR_NS::UsdGeomXformCache cache(PXR_NS::UsdTimeCode::Default());

    return cache.GetLocalToWorldTransform(stage->GetPrimAtPath(PXR_NS::SdfPath(path)));
}

void
expectClose(const PXR_NS::GfMatrix4d& expected,
            const PXR_NS::GfMatrix4d& actual)
{
    EXPECT_TRUE(PXR_NS::GfIsClose(expected, actual, 1e-6)) << "expected " << expected << "\nactual   " << actual;
}

} // anonymous namespace

class GeoToSceneOverridesTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        KnobDouble* metersPerUnit = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("metersPerUnit").get());
        ASSERT_TRUE(metersPerUnit != NULL);
        metersPerUnit->setValue(0.01);
    }

    virtual void TearDown() OVERRIDE
    {
        for (NodePtr& node : _nodes) {
            if (node) {
                node->destroyNode(false, false);
            }
        }
        _nodes.clear();
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    NodePtr make(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node));
        if (node) {
            _nodes.push_back(node);
        }

        return node;
    }

    template <typename K>
    static K* knob(const NodePtr& node,
                   const char* name)
    {
        K* k = dynamic_cast<K*>(node->getKnobByName(name).get());

        EXPECT_TRUE(k != NULL) << name;

        return k;
    }

    // USDRead -> USDToGeo(pattern) -> GeoTransform(+kShift in Z) -> GeoToUSD, whose Scene input
    // is the reader, or sceneInput when given.
    void build(const char* pattern,
               bool unpack,
               const NodePtr& sceneInput = NodePtr())
    {
        const std::string path = _dir.path().toStdString() + "/overrides.usda";
        {
            QFile f(QString::fromStdString(path));
            ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(kScene);
        }
        if (!_reader) {
            _reader = make(PLUGINID_NATRON_USDREAD);
        }
        _toGeo = make(PLUGINID_NATRON_USDTOGEO);
        _transform = make(PLUGINID_NATRON_GEOTRANSFORM);
        _toUSD = make(PLUGINID_NATRON_GEOTOUSD);
        ASSERT_TRUE(_reader && _toGeo && _transform && _toUSD);

        KnobFile* file = knob<KnobFile>(_reader, kUSDReadParamFile);
        ASSERT_TRUE(file != NULL);
        file->setValue(path);
        KnobString* primPattern = knob<KnobString>(_toGeo, kUSDToGeoParamPrimPattern);
        ASSERT_TRUE(primPattern != NULL);
        primPattern->setValue(std::string(pattern));
        KnobBool* unpackKnob = knob<KnobBool>(_toGeo, kUSDToGeoParamUnpack);
        ASSERT_TRUE(unpackKnob != NULL);
        unpackKnob->setValue(unpack);
        KnobDouble* translate = knob<KnobDouble>(_transform, kGeoTransformParamTranslate);
        ASSERT_TRUE(translate != NULL);
        translate->setValue(kShift, ViewSpec::all(), 2);

        connectNodes(_reader, _toGeo, 0, true);
        connectNodes(_toGeo, _transform, 0, true);
        connectNodes(_transform, _toUSD, kGeoToUSDInputGeometry, true);
        connectNodes(sceneInput ? sceneInput : _reader, _toUSD, kGeoToUSDInputScene, true);
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

    static PXR_NS::UsdStageRefPtr stageOf(const ScenePayload& payload,
                                          SceneQueryPool::Lease* lease)
    {
        std::string error;
        *lease = SceneQueryPool::instance().borrow(payload, &error);
        EXPECT_TRUE(lease->isValid()) << error;

        return lease->getStage();
    }

    NodePtr _reader;
    NodePtr _toGeo;
    NodePtr _transform;
    NodePtr _toUSD;

private:
    QTemporaryDir _dir;
    std::vector<NodePtr> _nodes;
};

TEST_F(GeoToSceneOverridesTest, PackedPrimsMoveAtTheirPathsUnderATransformedParent)
{
    build("/World/*", false);
    const ScenePayload source = evaluate(_reader);
    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease sourceLease;
    SceneQueryPool::Lease resultLease;
    const PXR_NS::UsdStageRefPtr before = stageOf(source, &sourceLease);
    const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
    ASSERT_TRUE(before && after);

    EXPECT_EQ(primPaths(before), primPaths(after));
    PXR_NS::GfMatrix4d shift(1.);
    shift.SetTranslate(PXR_NS::GfVec3d(0., 0., kShift));
    for (const char* path : { "/World/A", "/World/C" }) {
        SCOPED_TRACE(path);
        expectClose(worldOf(before, path) * shift, worldOf(after, path));
    }
    expectClose(worldOf(before, "/World"), worldOf(after, "/World"));

    QString message;
    int type = 0;
    _toUSD->getPersistentMessage(&message, &type, false);
    EXPECT_NE((int)eMessageTypeWarning, type) << message.toStdString();
}

TEST_F(GeoToSceneOverridesTest, AnUnmovedPackedRoundTripKeepsEveryWorldTransform)
{
    build("/World/*", false);
    KnobDouble* translate = knob<KnobDouble>(_transform, kGeoTransformParamTranslate);
    ASSERT_TRUE(translate != NULL);
    translate->setValue(0., ViewSpec::all(), 2);

    const ScenePayload source = evaluate(_reader);
    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease sourceLease;
    SceneQueryPool::Lease resultLease;
    const PXR_NS::UsdStageRefPtr before = stageOf(source, &sourceLease);
    const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
    ASSERT_TRUE(before && after);
    for (const char* path : { "/World", "/World/A", "/World/C" }) {
        SCOPED_TRACE(path);
        expectClose(worldOf(before, path), worldOf(after, path));
    }
}

TEST_F(GeoToSceneOverridesTest, AnUnpackedPrimIsOverriddenOnItsPointsOnly)
{
    build("/World/A", true);
    const ScenePayload source = evaluate(_reader);
    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease sourceLease;
    SceneQueryPool::Lease resultLease;
    const PXR_NS::UsdStageRefPtr before = stageOf(source, &sourceLease);
    const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
    ASSERT_TRUE(before && after);
    EXPECT_EQ(primPaths(before), primPaths(after));

    const std::shared_ptr<const ScenePayloadImpl>& imp = result.getImpl();
    ASSERT_TRUE(imp && imp->top.dependencies && !imp->top.dependencies->layers.empty());
    const PXR_NS::SdfLayerRefPtr geo = imp->top.dependencies->layers.front();
    const PXR_NS::SdfPrimSpecHandle spec = geo->GetPrimAtPath(PXR_NS::SdfPath("/World/A"));
    ASSERT_TRUE(spec);
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, spec->GetSpecifier());
    EXPECT_TRUE(spec->GetAttributes().get(PXR_NS::UsdGeomTokens->points));
    for (const PXR_NS::TfToken& name : { PXR_NS::UsdGeomTokens->faceVertexCounts, PXR_NS::UsdGeomTokens->faceVertexIndices }) {
        EXPECT_FALSE(spec->GetAttributes().get(name)) << name.GetString();
    }

    PXR_NS::VtVec3fArray oldPoints;
    PXR_NS::VtVec3fArray newPoints;
    ASSERT_TRUE(PXR_NS::UsdGeomMesh(before->GetPrimAtPath(PXR_NS::SdfPath("/World/A"))).GetPointsAttr().Get(&oldPoints));
    ASSERT_TRUE(PXR_NS::UsdGeomMesh(after->GetPrimAtPath(PXR_NS::SdfPath("/World/A"))).GetPointsAttr().Get(&newPoints));
    ASSERT_EQ(oldPoints.size(), newPoints.size());
    for (std::size_t i = 0; i < oldPoints.size(); ++i) {
        EXPECT_TRUE(PXR_NS::GfIsClose(oldPoints[i] + PXR_NS::GfVec3f(0.f, 0.f, float(kShift)), newPoints[i], 1e-5)) << i;
    }
    expectClose(worldOf(before, "/World/A"), worldOf(after, "/World/A"));
}

TEST_F(GeoToSceneOverridesTest, APrunedSourcePathRaisesAWarning)
{
    _reader = make(PLUGINID_NATRON_USDREAD);
    const NodePtr prune = make(PLUGINID_NATRON_USDPRUNE);
    ASSERT_TRUE(_reader && prune);
    KnobString* pattern = knob<KnobString>(prune, kUSDPruneParamPrimPattern);
    ASSERT_TRUE(pattern != NULL);
    pattern->setValue(std::string("/World/C"));
    connectNodes(_reader, prune, 0, true);
    build("/World/*", false, prune);

    const ScenePayload result = evaluate(_toUSD);
    EXPECT_FALSE(result.isEmpty());
    QString message;
    int type = 0;
    _toUSD->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/C"))) << message.toStdString();
    EXPECT_FALSE(message.contains(QString::fromUtf8("/World/A"))) << message.toStdString();
}
