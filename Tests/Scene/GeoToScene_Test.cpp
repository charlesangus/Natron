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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoCopyToPoints.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoToUSD.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// The layer the node's own layer sublayers, or null.
PXR_NS::SdfLayerRefPtr
geometryLayer(const ScenePayload& payload)
{
    const std::shared_ptr<const ScenePayloadImpl>& imp = payload.getImpl();
    if (!imp || !imp->top.dependencies || imp->top.dependencies->layers.empty()) {
        return PXR_NS::SdfLayerRefPtr();
    }

    return imp->top.dependencies->layers.front();
}

} // anonymous namespace

class GeoToSceneTest
    : public BaseTest {
protected:
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

    // A card copied, packed, onto the 100 points of a 9x9-segment grid, into a GeoToUSD.
    void buildScatter()
    {
        _card = make(PLUGINID_NATRON_GEOPRIMITIVE);
        _grid = make(PLUGINID_NATRON_GEOPRIMITIVE);
        _copy = make(PLUGINID_NATRON_GEOCOPYTOPOINTS);
        _toUSD = make(PLUGINID_NATRON_GEOTOUSD);
        ASSERT_TRUE(_card && _grid && _copy && _toUSD);

        KnobChoice* type = knob<KnobChoice>(_grid, kGeoPrimitiveParamType);
        ASSERT_TRUE(type != NULL);
        type->setValueFromID("grid", 0);
        KnobInt* rows = knob<KnobInt>(_grid, kGeoPrimitiveParamGridRows);
        KnobInt* columns = knob<KnobInt>(_grid, kGeoPrimitiveParamGridColumns);
        ASSERT_TRUE(rows && columns);
        rows->setValue(9);
        columns->setValue(9);
        KnobBool* pack = knob<KnobBool>(_copy, kGeoCopyToPointsParamPackAndInstance);
        ASSERT_TRUE(pack != NULL);
        pack->setValue(true);

        connectNodes(_card, _copy, 0, true);
        connectNodes(_grid, _copy, 1, true);
        connectNodes(_copy, _toUSD, kGeoToUSDInputGeometry, true);
    }

    void setPackedMode(const char* id)
    {
        KnobChoice* mode = knob<KnobChoice>(_toUSD, kGeoToUSDParamPackedPrimitives);
        ASSERT_TRUE(mode != NULL);
        mode->setValueFromID(id, 0);
    }

    static ScenePayload evaluate(const NodePtr& node,
                                 double time = 1.,
                                 ViewIdx view = ViewIdx(0))
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(time, view), &payload, &error))
            << error;

        return payload;
    }

    // The identifier of the geometry layer the node serves at time and view, or empty.
    static std::string layerIdentifier(const NodePtr& node,
                                       double time,
                                       ViewIdx view = ViewIdx(0))
    {
        const PXR_NS::SdfLayerRefPtr layer = geometryLayer(evaluate(node, time, view));
        EXPECT_TRUE(layer);

        return layer ? layer->GetIdentifier() : std::string();
    }

    static PXR_NS::UsdStageRefPtr stageOf(const ScenePayload& payload,
                                          SceneQueryPool::Lease* lease)
    {
        std::string error;
        *lease = SceneQueryPool::instance().borrow(payload, &error);
        EXPECT_TRUE(lease->isValid()) << error;

        return lease->getStage();
    }

    NodePtr _card;
    NodePtr _grid;
    NodePtr _copy;
    NodePtr _toUSD;

private:
    std::vector<NodePtr> _nodes;
};

TEST_F(GeoToSceneTest, GeometryGoesUnderTheRootPath)
{
    buildScatter();
    KnobString* root = knob<KnobString>(_toUSD, kGeoToUSDParamRootPath);
    ASSERT_TRUE(root != NULL);
    root->setValue(std::string("/scatter"));

    const ScenePayload payload = evaluate(_toUSD);
    SceneQueryPool::Lease lease;
    const PXR_NS::UsdStageRefPtr stage = stageOf(payload, &lease);
    ASSERT_TRUE(stage);
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/scatter")).IsValid());
    EXPECT_TRUE(static_cast<bool>(PXR_NS::UsdGeomPointInstancer(stage->GetPrimAtPath(PXR_NS::SdfPath("/scatter/instancer_0")))));
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/geo")).IsValid());
}

TEST_F(GeoToSceneTest, TheStackServesAGeometryLayerWithoutACopy)
{
    buildScatter();
    const ScenePayload payload = evaluate(_toUSD);
    const std::shared_ptr<const ScenePayloadImpl>& imp = payload.getImpl();
    ASSERT_TRUE(imp != nullptr);

    const PXR_NS::SdfLayerRefPtr geo = geometryLayer(payload);
    ASSERT_TRUE(geo);
    EXPECT_FALSE(geo->IsAnonymous());
    EXPECT_EQ(0u, geo->GetIdentifier().rfind("natrongeo:", 0)) << geo->GetIdentifier();
    EXPECT_EQ(1u, imp->top.layer->GetSubLayerPaths().size());
    EXPECT_FALSE(imp->top.layer->GetPrimAtPath(PXR_NS::SdfPath("/geo")));
    EXPECT_FALSE(imp->top.layer->GetPrimAtPath(PXR_NS::SdfPath("/geo/instancer_0")));
}

TEST_F(GeoToSceneTest, AnUnchangedDetailServesTheSameLayer)
{
    buildScatter();
    const ScenePayload first = evaluate(_toUSD);
    const ScenePayload second = evaluate(_toUSD);
    const PXR_NS::SdfLayerRefPtr a = geometryLayer(first);
    const PXR_NS::SdfLayerRefPtr b = geometryLayer(second);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(get_pointer(a), get_pointer(b));
    EXPECT_TRUE(first.isSameStack(second));
}

TEST_F(GeoToSceneTest, AChangedPointKeepsTheLayerAndUpdatesIt)
{
    buildScatter();
    const ScenePayload before = evaluate(_toUSD);
    const PXR_NS::SdfLayerRefPtr layer = geometryLayer(before);
    ASSERT_TRUE(layer);
    const std::string identifier = layer->GetIdentifier();
    const PXR_NS::SdfPath positions("/geo/instancer_0.positions");
    PXR_NS::SdfAttributeSpecHandle spec = layer->GetAttributeAtPath(positions);
    ASSERT_TRUE(spec);
    const PXR_NS::VtValue oldValue = spec->GetDefaultValue();
    ASSERT_TRUE(oldValue.IsHolding<PXR_NS::VtVec3fArray>());

    KnobDouble* size = knob<KnobDouble>(_grid, kGeoPrimitiveParamGridSize);
    ASSERT_TRUE(size != NULL);
    size->setValue(4., ViewSpec::all(), 0);
    size->setValue(4., ViewSpec::all(), 1);

    const ScenePayload after = evaluate(_toUSD);
    const PXR_NS::SdfLayerRefPtr changed = geometryLayer(after);
    ASSERT_TRUE(changed);
    EXPECT_EQ(identifier, changed->GetIdentifier());
    EXPECT_EQ(get_pointer(layer), get_pointer(changed));
    spec = changed->GetAttributeAtPath(positions);
    ASSERT_TRUE(spec);
    const PXR_NS::VtValue newValue = spec->GetDefaultValue();
    ASSERT_TRUE(newValue.IsHolding<PXR_NS::VtVec3fArray>());
    EXPECT_EQ(oldValue.UncheckedGet<PXR_NS::VtVec3fArray>().size(), newValue.UncheckedGet<PXR_NS::VtVec3fArray>().size());
    EXPECT_NE(oldValue.UncheckedGet<PXR_NS::VtVec3fArray>(), newValue.UncheckedGet<PXR_NS::VtVec3fArray>());
}

TEST_F(GeoToSceneTest, HundredPackedCopiesGiveOnePointInstancer)
{
    buildScatter();
    const ScenePayload payload = evaluate(_toUSD);
    SceneQueryPool::Lease lease;
    const PXR_NS::UsdStageRefPtr stage = stageOf(payload, &lease);
    ASSERT_TRUE(stage);

    std::size_t instancers = 0;
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<PXR_NS::UsdGeomPointInstancer>()) {
            ++instancers;
        }
    }
    EXPECT_EQ(1u, instancers);
    const PXR_NS::UsdGeomPointInstancer instancer(stage->GetPrimAtPath(PXR_NS::SdfPath("/geo/instancer_0")));
    ASSERT_TRUE(static_cast<bool>(instancer));
    PXR_NS::VtIntArray indices;
    ASSERT_TRUE(instancer.GetProtoIndicesAttr().Get(&indices, PXR_NS::UsdTimeCode::Default()));
    EXPECT_EQ(100u, indices.size());
    PXR_NS::SdfPathVector targets;
    instancer.GetPrototypesRel().GetTargets(&targets);
    EXPECT_EQ(1u, targets.size());
}

TEST_F(GeoToSceneTest, NativeInstancesGiveOneInstanceablePrimPerCopy)
{
    buildScatter();
    setPackedMode("native");
    const ScenePayload payload = evaluate(_toUSD);
    SceneQueryPool::Lease lease;
    const PXR_NS::UsdStageRefPtr stage = stageOf(payload, &lease);
    ASSERT_TRUE(stage);

    const PXR_NS::UsdPrim container = stage->GetPrimAtPath(PXR_NS::SdfPath("/geo/instances_0"));
    ASSERT_TRUE(container.IsValid());
    std::size_t instances = 0;
    for (const PXR_NS::UsdPrim& child : container.GetChildren()) {
        if (child.IsInstance()) {
            ++instances;
        }
    }
    EXPECT_EQ(100u, instances);
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        EXPECT_FALSE(prim.IsA<PXR_NS::UsdGeomPointInstancer>()) << prim.GetPath().GetString();
    }
}

TEST_F(GeoToSceneTest, UnpackGivesMeshes)
{
    buildScatter();
    setPackedMode("unpack");
    GeoDetailConstPtr card;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              _card->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0)), &card, &error))
        << error;
    ASSERT_TRUE(card != nullptr);

    const ScenePayload payload = evaluate(_toUSD);
    SceneQueryPool::Lease lease;
    const PXR_NS::UsdStageRefPtr stage = stageOf(payload, &lease);
    ASSERT_TRUE(stage);

    std::size_t faces = 0;
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        EXPECT_FALSE(prim.IsA<PXR_NS::UsdGeomPointInstancer>()) << prim.GetPath().GetString();
        EXPECT_FALSE(prim.IsInstance()) << prim.GetPath().GetString();
        const PXR_NS::UsdGeomMesh mesh(prim);
        if (mesh) {
            PXR_NS::VtIntArray counts;
            mesh.GetFaceVertexCountsAttr().Get(&counts, PXR_NS::UsdTimeCode::Default());
            faces += counts.size();
        }
    }
    EXPECT_EQ(std::size_t(100 * card->primitiveCount()), faces);
}

TEST_F(GeoToSceneTest, CopyIntoLayerAuthorsAnAnonymousLayer)
{
    buildScatter();
    KnobBool* copy = knob<KnobBool>(_toUSD, kGeoToUSDParamCopyIntoLayer);
    ASSERT_TRUE(copy != NULL);
    copy->setValue(true);

    const ScenePayload payload = evaluate(_toUSD);
    const std::shared_ptr<const ScenePayloadImpl>& imp = payload.getImpl();
    ASSERT_TRUE(imp != nullptr);
    ASSERT_TRUE(imp->top.layer);
    EXPECT_TRUE(imp->top.layer->IsAnonymous());
    EXPECT_TRUE(imp->top.layer->GetSubLayerPaths().empty());
    EXPECT_FALSE(geometryLayer(payload));
    EXPECT_TRUE(imp->top.layer->GetPrimAtPath(PXR_NS::SdfPath("/geo/instancer_0")));

    SceneQueryPool::Lease lease;
    const PXR_NS::UsdStageRefPtr stage = stageOf(payload, &lease);
    ASSERT_TRUE(stage);
    EXPECT_TRUE(static_cast<bool>(PXR_NS::UsdGeomPointInstancer(stage->GetPrimAtPath(PXR_NS::SdfPath("/geo/instancer_0")))));
}

TEST_F(GeoToSceneTest, EachProjectViewGetsItsOwnLayer)
{
    buildScatter();
    EXPECT_EQ(layerIdentifier(_toUSD, 1.), layerIdentifier(_toUSD, 1.));

    getApp()->getProject()->createProjectViews(std::vector<std::string> { "Right" });
    ASSERT_EQ(2, getApp()->getProject()->getProjectViewsCount());
    const std::string left = layerIdentifier(_toUSD, 1., ViewIdx(0));
    const std::string right = layerIdentifier(_toUSD, 1., ViewIdx(1));
    ASSERT_FALSE(left.empty());
    ASSERT_FALSE(right.empty());
    EXPECT_NE(left, right);
    EXPECT_EQ(left, layerIdentifier(_toUSD, 1., ViewIdx(0)));
}

TEST_F(GeoToSceneTest, CloseSubframesOfAnimatedGeometryGetLayersOfTheirOwn)
{
    buildScatter();
    KnobDouble* size = knob<KnobDouble>(_grid, kGeoPrimitiveParamGridSize);
    ASSERT_TRUE(size != NULL);
    size->setValueAtTime(1., 1., ViewSpec::all(), 0);
    size->setValueAtTime(2., 4., ViewSpec::all(), 0);

    // Both print as 1 with nine significant digits.
    const double a = 1.0000000001;
    const double b = 1.0000000002;
    const std::string first = layerIdentifier(_toUSD, a);
    const std::string second = layerIdentifier(_toUSD, b);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_NE(first, second);
    EXPECT_EQ(first, layerIdentifier(_toUSD, a));
}
