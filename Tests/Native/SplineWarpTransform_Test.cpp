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

#include <Python.h>

#include "Global/Macros.h"

#include <cmath>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCurve.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpPanelHook.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpUndo.h"
#include "Engine/Project.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/RotoCurvePanelHook.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/Transform.h"
#include "Engine/UndoCommand.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const double kTime = 1.;

BezierPtr
makeQuad(const SplineWarpContextPtr& ctx,
         double x,
         double y,
         double size)
{
    BezierPtr bezier = ctx->makeBezier(x, y, kRotoBezierBaseName, kTime, false);
    if (!bezier) {
        return bezier;
    }
    bezier->addControlPoint(x + size, y, kTime);
    bezier->addControlPoint(x + size * 1.2, y + size, kTime);
    bezier->addControlPoint(x - size * 0.1, y + size * 0.8, kTime);
    bezier->setCurveFinished(true);

    return bezier;
}

Point
positionAt(const BezierPtr& curve,
           double u)
{
    return SplineWarpCurveShape::fromBezier(*curve, kTime).pointAt(u);
}

KnobDoublePtr
doubleKnob(const NodePtr& node,
           const char* name)
{
    return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));
}

KnobDoublePtr
itemKnob(const BezierPtr& curve,
         const char* name)
{
    return std::dynamic_pointer_cast<KnobDouble>(curve->getKnobByName(name));
}

void
setTranslate(const KnobDoublePtr& knob,
             double x,
             double y)
{
    knob->setValue(x, ViewSpec::all(), 0);
    knob->setValue(y, ViewSpec::all(), 1);
}

// The transform rotating by degrees about the origin.
SplineWarpTransform
rotationAboutOrigin(double degrees)
{
    SplineWarpTransform t;
    int dims = 0;
    t.value(SplineWarpTransform::indexOf("rotate", &dims)).setValue(degrees, kTime);
    const int center = SplineWarpTransform::indexOf("center", &dims);
    t.value(center).setValue(0., kTime);
    t.value(center + 1).setValue(0., kTime);

    return t;
}
} // anonymous namespace

class SplineWarpTransformTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "splineWarpTransform64x48", 1.));
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr makeSplineWarp()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);

        EXPECT_TRUE(bool(node));

        return node;
    }

    static SplineWarpContextPtr contextOf(const NodePtr& node)
    {
        return node ? std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext()) : SplineWarpContextPtr();
    }

    static SplineWarp* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<SplineWarp*>(node->getEffectInstance().get()) : 0;
    }
};

TEST_F(SplineWarpTransformTest, TheTransformPageHasRotosKnobs)
{
    NodePtr node = makeSplineWarp();
    ASSERT_TRUE(bool(node));
    const char* const names[] = {
        "translate",
        "rotate",
        "scale",
        "uniform",
        "skewx",
        "skewy",
        "skewOrder",
        "center",
        "extraMatrix",
    };
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        EXPECT_TRUE(bool(node->getKnobByName(names[i]))) << names[i];
    }
    const std::vector<std::string>& modelNames = SplineWarpTransform::knobNames();
    EXPECT_EQ(sizeof(names) / sizeof(names[0]), modelNames.size());
}

TEST_F(SplineWarpTransformTest, ANewCurveIsCentredOnTheSourceFormat)
{
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr node = makeSplineWarp();
    ASSERT_TRUE(bool(node));
    connectNodes(source, node, 0, true);
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(bool(ctx));

    BezierPtr curve = makeQuad(ctx, 10., 10., 10.);
    ASSERT_TRUE(bool(curve));
    KnobDoublePtr center = itemKnob(curve, "center");
    ASSERT_TRUE(bool(center));
    EXPECT_DOUBLE_EQ(kParitySourceWidth / 2., center->getValue(0));
    EXPECT_DOUBLE_EQ(kParitySourceHeight / 2., center->getValue(1));
}

TEST_F(SplineWarpTransformTest, TranslatingADestinationMovesTheFieldAndItsKey)
{
    NodePtr node = makeSplineWarp();
    ASSERT_TRUE(bool(node));
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(bool(ctx));
    BezierPtr from = makeQuad(ctx, 10., 10., 12.);
    BezierPtr to = makeQuad(ctx, 14., 12., 12.);
    ASSERT_TRUE(from && to);
    ASSERT_TRUE(bool(ctx->linkWithAlignment(from, to, 0., false)));
    SplineWarpSamplingSettings settings;
    settings.time = kTime;

    const SplineWarpSampleSet before = sampleSplineWarp(*ctx, settings);
    ASSERT_FALSE(before.pairs.empty());
    const U64 hashBefore = ctx->getContentHash();

    // Through the panel knob, with the destination selected.
    KnobDoublePtr translate = doubleKnob(node, "translate");
    ASSERT_TRUE(bool(translate));
    ctx->select(to, RotoItem::eSelectionReasonOther);
    setTranslate(translate, 3., -2.);
    ctx->clearSelection(RotoItem::eSelectionReasonOther);

    const SplineWarpSampleSet after = sampleSplineWarp(*ctx, settings);
    ASSERT_EQ(before.pairs.size(), after.pairs.size());
    for (std::size_t i = 0; i < before.pairs.size(); ++i) {
        EXPECT_NEAR(before.pairs[i].destination.x + 3., after.pairs[i].destination.x, 1e-6) << i;
        EXPECT_NEAR(before.pairs[i].destination.y - 2., after.pairs[i].destination.y, 1e-6) << i;
        EXPECT_NEAR(before.pairs[i].source.x, after.pairs[i].source.x, 1e-6) << i;
        EXPECT_NEAR(before.pairs[i].source.y, after.pairs[i].source.y, 1e-6) << i;
    }
    EXPECT_NE(hashBefore, ctx->getContentHash());

    // The curve kept the value once deselected.
    KnobDoublePtr own = itemKnob(to, "translate");
    ASSERT_TRUE(bool(own));
    EXPECT_DOUBLE_EQ(3., own->getValue(0));
    EXPECT_DOUBLE_EQ(-2., own->getValue(1));
}

TEST_F(SplineWarpTransformTest, ASelectedCurvesKnobEditReachesThePanel)
{
    NodePtr node = makeSplineWarp();
    ASSERT_TRUE(bool(node));
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(bool(ctx));
    BezierPtr curve = makeQuad(ctx, 10., 10., 12.);
    ASSERT_TRUE(bool(curve));
    KnobDoublePtr translate = doubleKnob(node, "translate");
    KnobDoublePtr own = itemKnob(curve, "translate");
    ASSERT_TRUE(translate && own);

    ctx->select(curve, RotoItem::eSelectionReasonOther);
    setTranslate(own, 5., 7.);
    EXPECT_DOUBLE_EQ(5., translate->getValue(0));
    EXPECT_DOUBLE_EQ(7., translate->getValue(1));
}

TEST_F(SplineWarpTransformTest, ALayerRotateMovesAllItsCurves)
{
    NodePtr node = makeSplineWarp();
    ASSERT_TRUE(bool(node));
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 10., 10., 12.);
    BezierPtr b = makeQuad(ctx, 30., 5., 8.);
    BezierPtr outside = makeQuad(ctx, 40., 30., 6.);
    RotoLayerPtr layer = ctx->addLayer();
    ASSERT_TRUE(a && b && outside && layer);
    layer->addItem(a);
    layer->addItem(b);

    const BezierPtr curves[] = { a, b, outside };
    std::vector<Point> before;
    for (int i = 0; i < 3; ++i) {
        before.push_back(positionAt(curves[i], 0.));
    }
    const U64 hashBefore = ctx->getContentHash();

    ctx->setLayerTransform(layer, rotationAboutOrigin(90.));
    EXPECT_NE(hashBefore, ctx->getContentHash());

    Transform::Matrix3x3 rotation;
    rotationAboutOrigin(90.).getMatrixAt(kTime, &rotation);
    for (int i = 0; i < 2; ++i) {
        double x = before[i].x, y = before[i].y, z = 1.;
        Transform::matApply(rotation, &x, &y, &z);
        const Point after = positionAt(curves[i], 0.);
        EXPECT_NEAR(x / z, after.x, 1e-6) << i;
        EXPECT_NEAR(y / z, after.y, 1e-6) << i;
        EXPECT_NEAR(std::hypot(before[i].x, before[i].y), std::hypot(after.x, after.y), 1e-6) << i;
        EXPECT_GT(std::hypot(after.x - before[i].x, after.y - before[i].y), 1.) << i;
    }
    const Point outsideAfter = positionAt(outside, 0.);
    EXPECT_DOUBLE_EQ(before[2].x, outsideAfter.x);
    EXPECT_DOUBLE_EQ(before[2].y, outsideAfter.y);
}

TEST_F(SplineWarpTransformTest, TheLayerShowsInThePanelKnobsWhenSelected)
{
    NodePtr node = makeSplineWarp();
    ASSERT_TRUE(bool(node));
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 10., 10., 12.);
    RotoLayerPtr layer = ctx->addLayer();
    ASSERT_TRUE(a && layer);
    layer->addItem(a);
    KnobDoublePtr translate = doubleKnob(node, "translate");
    ASSERT_TRUE(bool(translate));
    const Point before = positionAt(a, 0.);

    ctx->clearSelection(RotoItem::eSelectionReasonOther);
    ctx->select(layer, RotoItem::eSelectionReasonOther);
    setTranslate(translate, 4., 6.);
    int dims = 0;
    const int index = SplineWarpTransform::indexOf("translate", &dims);
    ASSERT_EQ(2, dims);
    EXPECT_DOUBLE_EQ(4., ctx->getLayerTransform(layer).value(index).getValueAt(kTime));
    EXPECT_DOUBLE_EQ(6., ctx->getLayerTransform(layer).value(index + 1).getValueAt(kTime));

    // The curve inside moves with the layer only, not by the value a second time.
    const Point after = positionAt(a, 0.);
    EXPECT_NEAR(before.x + 4., after.x, 1e-6);
    EXPECT_NEAR(before.y + 6., after.y, 1e-6);
    KnobDoublePtr own = itemKnob(a, "translate");
    ASSERT_TRUE(bool(own));
    EXPECT_DOUBLE_EQ(0., own->getValue(0));
}

TEST_F(SplineWarpTransformTest, ALinkedCurveFollowsItsSourceAndCyclesAreRefused)
{
    NodePtr node = makeSplineWarp();
    ASSERT_TRUE(bool(node));
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(bool(ctx));
    BezierPtr a = makeQuad(ctx, 10., 10., 12.);
    BezierPtr b = makeQuad(ctx, 30., 5., 8.);
    BezierPtr c = makeQuad(ctx, 40., 30., 6.);
    ASSERT_TRUE(a && b && c);
    const Point aBefore = positionAt(a, 0.);

    ctx->linkTransform(a, b);
    EXPECT_EQ(RotoItemPtr(b), ctx->getTransformLink(a));
    KnobDoublePtr bTranslate = itemKnob(b, "translate");
    ASSERT_TRUE(bool(bTranslate));
    const U64 hashBefore = ctx->getContentHash();
    setTranslate(bTranslate, 2., 5.);
    EXPECT_NE(hashBefore, ctx->getContentHash());
    const Point aAfter = positionAt(a, 0.);
    EXPECT_NEAR(aBefore.x + 2., aAfter.x, 1e-6);
    EXPECT_NEAR(aBefore.y + 5., aAfter.y, 1e-6);

    // A chain follows its end; a link back is refused.
    ctx->linkTransform(b, c);
    EXPECT_EQ(RotoItemPtr(c), ctx->getTransformSource(a));
    EXPECT_THROW(ctx->linkTransform(c, a), std::invalid_argument);
    EXPECT_THROW(ctx->linkTransform(a, a), std::invalid_argument);
    EXPECT_EQ(RotoItemPtr(), ctx->getTransformLink(c));

    // A curve cannot follow the layer it is in.
    RotoLayerPtr layer = ctx->addLayer();
    ASSERT_TRUE(bool(layer));
    layer->addItem(c);
    EXPECT_THROW(ctx->linkTransform(c, layer), std::invalid_argument);

    // Unlinking keeps the curve where it was.
    ctx->unlinkTransform(b);
    ctx->unlinkTransform(a);
    EXPECT_EQ(RotoItemPtr(), ctx->getTransformLink(a));
    const Point aUnlinked = positionAt(a, 0.);
    EXPECT_NEAR(aAfter.x, aUnlinked.x, 1e-6);
    EXPECT_NEAR(aAfter.y, aUnlinked.y, 1e-6);

    // Removing the target drops the link.
    ctx->linkTransform(a, b);
    ctx->removeItem(b, RotoItem::eSelectionReasonOther);
    EXPECT_EQ(RotoItemPtr(), ctx->getTransformLink(a));
}

TEST_F(SplineWarpTransformTest, ThePanelLinksTransformsAndUndoRestoresThem)
{
    NodePtr node = makeSplineWarp();
    ASSERT_TRUE(bool(node));
    SplineWarpContextPtr ctx = contextOf(node);
    SplineWarp* effect = effectOf(node);
    ASSERT_TRUE(ctx && effect);
    SplineWarpInteractPtr ui = effect->getInteract();
    ASSERT_TRUE(bool(ui));
    RotoCurvePanelHook* hook = node->getEffectInstance()->getCurvePanelHook();
    ASSERT_TRUE(hook != nullptr);
    BezierPtr target = makeQuad(ctx, 10., 10., 12.);
    BezierPtr follower = makeQuad(ctx, 30., 5., 8.);
    RotoLayerPtr layer = ctx->addLayer();
    ASSERT_TRUE(target && follower && layer);
    target->setLabel("Head");

    const std::vector<RotoCurvePanelHook::MenuAction> actions = hook->getItemMenuActions(std::list<RotoItemPtr> { target, follower, layer });
    bool sawLink = false;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].id == "linkTransform") {
            sawLink = true;
            EXPECT_TRUE(actions[i].enabled);
        } else if (actions[i].id == "unlinkTransform") {
            EXPECT_FALSE(actions[i].enabled);
        }
    }
    EXPECT_TRUE(sawLink);

    const U64 hashBefore = ctx->getContentHash();
    UndoCommandPtr command;
    ASSERT_EQ(std::string(), ui->linkTransform(std::list<RotoItemPtr> { target, follower, layer }, &command));
    ASSERT_TRUE(bool(command));
    EXPECT_EQ(RotoItemPtr(target), ctx->getTransformLink(follower));
    EXPECT_EQ(RotoItemPtr(target), ctx->getTransformLink(layer));
    EXPECT_EQ(std::string("Head"), hook->getItemText(follower, SplineWarpPanelHook::eColumnTransformLink));
    EXPECT_EQ(std::string("Head"), hook->getItemText(layer, SplineWarpPanelHook::eColumnTransformLink));
    EXPECT_EQ(std::string(), hook->getItemText(target, SplineWarpPanelHook::eColumnTransformLink));
    EXPECT_NE(hashBefore, ctx->getContentHash());

    // The reverse link would be a cycle.
    EXPECT_FALSE(ui->linkTransform(std::list<RotoItemPtr> { follower, target }).empty());

    command->undo();
    EXPECT_EQ(RotoItemPtr(), ctx->getTransformLink(follower));
    EXPECT_EQ(RotoItemPtr(), ctx->getTransformLink(layer));
    EXPECT_EQ(hashBefore, ctx->getContentHash());
    command->redo();
    EXPECT_EQ(RotoItemPtr(target), ctx->getTransformLink(follower));

    hook->onItemMenuActionTriggered("unlinkTransform", std::list<RotoItemPtr> { follower });
    EXPECT_EQ(RotoItemPtr(), ctx->getTransformLink(follower));
    EXPECT_EQ(RotoItemPtr(target), ctx->getTransformLink(layer));
}

TEST_F(SplineWarpTransformTest, UndoRestoresALayerTransform)
{
    NodePtr node = makeSplineWarp();
    ASSERT_TRUE(bool(node));
    SplineWarpContextPtr ctx = contextOf(node);
    SplineWarp* effect = effectOf(node);
    ASSERT_TRUE(ctx && effect);
    SplineWarpInteractPtr ui = effect->getInteract();
    ASSERT_TRUE(bool(ui));
    BezierPtr a = makeQuad(ctx, 10., 10., 12.);
    RotoLayerPtr layer = ctx->addLayer();
    ASSERT_TRUE(a && layer);
    layer->addItem(a);
    const Point before = positionAt(a, 0.);
    const U64 hashBefore = ctx->getContentHash();

    std::shared_ptr<SplineWarpStateUndoCommand> command = std::make_shared<SplineWarpStateUndoCommand>(ui, "Rotate", [ctx, layer]() {
        ctx->setLayerTransform(layer, rotationAboutOrigin(45.));
    });
    command->redo();
    const Point rotated = positionAt(a, 0.);
    EXPECT_GT(std::hypot(rotated.x - before.x, rotated.y - before.y), 1.);

    command->undo();
    const Point restored = positionAt(a, 0.);
    EXPECT_DOUBLE_EQ(before.x, restored.x);
    EXPECT_DOUBLE_EQ(before.y, restored.y);
    EXPECT_EQ(hashBefore, ctx->getContentHash());
}

TEST_F(SplineWarpTransformTest, TheOverlayShowsForOneCurveOrOneLayer)
{
    NodePtr node = makeSplineWarp();
    ASSERT_TRUE(bool(node));
    SplineWarpContextPtr ctx = contextOf(node);
    SplineWarp* effect = effectOf(node);
    ASSERT_TRUE(ctx && effect);
    SplineWarpInteractPtr ui = effect->getInteract();
    ASSERT_TRUE(bool(ui));
    BezierPtr a = makeQuad(ctx, 10., 10., 12.);
    BezierPtr b = makeQuad(ctx, 30., 5., 8.);
    RotoLayerPtr layer = ctx->addLayer();
    ASSERT_TRUE(a && b && layer);
    layer->addItem(a);
    ASSERT_EQ(eRotoToolSelectAll, ui->getSelectedTool());

    ctx->clearSelection(RotoItem::eSelectionReasonOther);
    EXPECT_FALSE(effect->shouldDrawHostOverlay());
    ctx->select(b, RotoItem::eSelectionReasonOther);
    EXPECT_TRUE(effect->shouldDrawHostOverlay());
    ctx->select(a, RotoItem::eSelectionReasonOther);
    EXPECT_FALSE(effect->shouldDrawHostOverlay());

    // The layer selects the curve inside it too.
    ctx->clearSelection(RotoItem::eSelectionReasonOther);
    ctx->select(layer, RotoItem::eSelectionReasonOther);
    EXPECT_TRUE(effect->shouldDrawHostOverlay());

    // Not with a drawing tool.
    KnobButtonPtr draw = std::dynamic_pointer_cast<KnobButton>(node->getKnobByName(kRotoUIParamDrawBezierToolButtonAction));
    ASSERT_TRUE(bool(draw));
    ui->setCurrentTool(draw);
    EXPECT_FALSE(effect->shouldDrawHostOverlay());
}
