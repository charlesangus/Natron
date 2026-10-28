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

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDVariant.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-variants.usda";

PXR_NS::SdfLayerRefPtr
getTopLayer(const ScenePayload& payload)
{
    return payload.getImpl() ? payload.getImpl()->top.layer : PXR_NS::SdfLayerRefPtr();
}

template <typename KNOB>
std::shared_ptr<KNOB>
getKnob(const NodePtr& node,
        const std::string& name)
{
    return std::dynamic_pointer_cast<KNOB>(node->getKnobByName(name));
}

std::vector<std::string>
getEntryIds(const KnobChoicePtr& menu)
{
    std::vector<std::string> ids;

    for (const ChoiceOption& option : menu->getEntries_mt_safe()) {
        ids.push_back(option.id);
    }
    std::sort(ids.begin(), ids.end());

    return ids;
}

// The material /World/Asset/Geo is bound to in the composed scene.
std::string
getBoundMaterial(const ScenePayload& payload)
{
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    if (!stage) {
        return "<no stage>";
    }
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Asset/Geo"));
    if (!prim) {
        return "<no prim>";
    }
    PXR_NS::SdfPathVector targets;
    const PXR_NS::UsdRelationship binding = prim.GetRelationship(PXR_NS::TfToken("material:binding"));
    if (!binding || !binding.GetTargets(&targets) || (targets.size() != 1)) {
        return "<no binding>";
    }

    return targets.front().GetString();
}
} // anonymous namespace

class Variant3DTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                                     ScenePayload* payload)
    {
        std::string error;

        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, &error);
    }

    // A USDVariant over the fixture selecting set { variant } on the prims pattern matches.
    NodePtr createVariant(const std::string& pattern,
                          const std::string& set,
                          const std::string& variant)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        EXPECT_TRUE(bool(reader));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDVARIANT));
        EXPECT_TRUE(bool(node));
        if (!reader || !node) {
            return NodePtr();
        }
        getKnob<KnobFile>(reader, kUSDReadParamFile)->setValue(std::string(kFixture));
        EXPECT_TRUE(node->connectInput(reader, 0));
        getKnob<KnobString>(node, kUSDVariantParamPrimPattern)->setValue(pattern);
        getKnob<KnobString>(node, kUSDVariantParamVariantSet)->setValue(set);
        getKnob<KnobString>(node, kUSDVariantParamVariant)->setValue(variant);

        return node;
    }

    static void refreshMenus(const NodePtr& node)
    {
        USDVariant* effect = dynamic_cast<USDVariant*>(node->getEffectInstance().get());
        ASSERT_TRUE(effect != nullptr);
        effect->refreshVariantMenus();
    }
};

TEST_F(Variant3DTest, TheSelectedVariantDecidesTheBinding)
{
    NodePtr node = createVariant("/World/Asset", "shadingVariant", "blue");
    ASSERT_TRUE(bool(node));

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    EXPECT_EQ(std::string("/World/Looks/Blue"), getBoundMaterial(payload));
    EXPECT_FALSE(node->hasPersistentMessage());

    // Only the selection is authored, over the prim.
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Asset"));
    ASSERT_TRUE(bool(spec));
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, spec->GetSpecifier());
    EXPECT_TRUE(spec->GetProperties().empty());
    const PXR_NS::SdfVariantSelectionMap selections = spec->GetVariantSelections();
    ASSERT_EQ(1u, selections.count("shadingVariant"));
    EXPECT_EQ(std::string("blue"), selections.at("shadingVariant"));

    getKnob<KnobString>(node, kUSDVariantParamVariant)->setValue(std::string("red"));
    ScenePayload red;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &red));
    EXPECT_EQ(std::string("/World/Looks/Red"), getBoundMaterial(red));
}

TEST_F(Variant3DTest, TheMenusListTheSetsAndVariantsOfTheFirstMatch)
{
    NodePtr node = createVariant("/World/Asset", "shadingVariant", std::string());
    ASSERT_TRUE(bool(node));
    refreshMenus(node);

    KnobChoicePtr sets = getKnob<KnobChoice>(node, kUSDVariantParamVariantSets);
    KnobChoicePtr variants = getKnob<KnobChoice>(node, kUSDVariantParamVariants);
    ASSERT_TRUE(bool(sets));
    ASSERT_TRUE(bool(variants));
    EXPECT_EQ(std::vector<std::string>({ "shadingVariant" }), getEntryIds(sets));
    EXPECT_EQ(std::vector<std::string>({ "blue", "red" }), getEntryIds(variants));
    EXPECT_FALSE(sets->getIsPersistent());
    EXPECT_FALSE(variants->getIsPersistent());

    // Picking a variant from the menu names it.
    variants->setValueFromID("blue", 0);
    EXPECT_EQ(std::string("blue"), getKnob<KnobString>(node, kUSDVariantParamVariant)->getValue());
    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    EXPECT_EQ(std::string("/World/Looks/Blue"), getBoundMaterial(payload));
}

TEST_F(Variant3DTest, ANameTheMenusLackIsKept)
{
    NodePtr node = createVariant("/World/Asset", "shadingVariant", "green");
    ASSERT_TRUE(bool(node));
    refreshMenus(node);

    EXPECT_EQ(std::string("green"), getKnob<KnobString>(node, kUSDVariantParamVariant)->getValue());
    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::SdfPrimSpecHandle spec = getTopLayer(payload)->GetPrimAtPath(PXR_NS::SdfPath("/World/Asset"));
    ASSERT_TRUE(bool(spec));
    const PXR_NS::SdfVariantSelectionMap selections = spec->GetVariantSelections();
    ASSERT_EQ(1u, selections.count("shadingVariant"));
    EXPECT_EQ(std::string("green"), selections.at("shadingVariant"));
}

TEST_F(Variant3DTest, APrimLackingTheSetIsAuthoredAndFlagged)
{
    NodePtr node = createVariant("/World/Looks/Red", "shadingVariant", "blue");
    ASSERT_TRUE(bool(node));

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    EXPECT_TRUE(bool(layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Looks/Red"))));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = -1;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeInfo, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/Looks/Red"))) << message.toStdString();
}

TEST_F(Variant3DTest, AnInvalidSetNameFails)
{
    NodePtr node = createVariant("/World/Asset", "shading Variant", "blue");
    ASSERT_TRUE(bool(node));

    ScenePayload payload;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    ASSERT_TRUE(node->hasPersistentMessage());
}
