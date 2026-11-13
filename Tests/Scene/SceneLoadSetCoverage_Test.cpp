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

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "SceneTestAuthoring.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDEditPrim.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDReference.h"
#include "Engine/Nodes/Scene/USDSetAttribute.h"
#include "Engine/Nodes/Scene/USDVariant.h"
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
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));

        EXPECT_TRUE(bool(node));
        if (node) {
            KnobFilePtr file = std::dynamic_pointer_cast<KnobFile>(node->getKnobByName(kUSDReadParamFile));
            EXPECT_TRUE(bool(file));
            if (file) {
                file->setValue(city.rootPath());
            }
        }

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
};

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
