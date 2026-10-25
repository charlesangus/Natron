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

#include "Global/Macros.h"

#include <algorithm>
#include <list>
#include <string>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "CurveHostTestEffect.h"
#include "Serialization/ProjectBundleTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/ViewIdx.h"
#include "Serialization/RotoSerialization.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

// A geometry curve host (CurveHostTestEffect, registered by wmain.cpp) edits curves with the same
// context, items and tools as RotoPaint, but builds no render tree for them.
class CurveHostTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    std::size_t countNodes() const
    {
        NodesList nodes;

        getApp()->getProject()->getNodes_recursive(nodes, false);

        return nodes.size();
    }

    // A closed four-point Bezier, (0, 0) to (100, 100), keyed at frame 1.
    static BezierPtr makeQuad(const RotoCurveContextPtr& ctx)
    {
        BezierPtr bezier = ctx->makeBezier(0., 0., kRotoBezierBaseName, 1., false);
        if (!bezier) {
            return bezier;
        }
        bezier->addControlPoint(100., 0., 1.);
        bezier->addControlPoint(100., 100., 1.);
        bezier->addControlPoint(0., 100., 1.);
        bezier->setCurveFinished(true);

        return bezier;
    }
};

TEST_F(CurveHostTest, TheHostGetsAGeometryContext)
{
    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDCurveHost));
    ASSERT_TRUE(bool(host));

    EXPECT_EQ(EffectInstance::eCurveContextKindGeometry, host->getEffectInstance()->getCurveContextKind());
    EXPECT_TRUE(bool(host->getCurveContext()));
    EXPECT_FALSE(bool(host->getRotoContext()));
}

TEST_F(CurveHostTest, MakeBezierCreatesNoInternalNodes)
{
    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDCurveHost));
    ASSERT_TRUE(bool(host));
    RotoCurveContextPtr ctx = host->getCurveContext();
    ASSERT_TRUE(bool(ctx));

    const std::size_t nodesBefore = countNodes();
    BezierPtr bezier = makeQuad(ctx);
    ASSERT_TRUE(bool(bezier));

    EXPECT_EQ(nodesBefore, countNodes());
    EXPECT_FALSE(bool(bezier->getEffectNode()));
    EXPECT_FALSE(bool(bezier->getMergeNode()));
    EXPECT_EQ(4, bezier->getControlPointsCount());
}

// The curve panel's paste and duplicate copy drawables with makeDrawableCopy() and layers with
// RotoLayer::clone().
TEST_F(CurveHostTest, CopyingTheHostsCurvesCreatesNoInternalNodes)
{
    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDCurveHost));
    ASSERT_TRUE(bool(host));
    RotoCurveContextPtr ctx = host->getCurveContext();
    ASSERT_TRUE(bool(ctx));
    BezierPtr bezier = makeQuad(ctx);
    ASSERT_TRUE(bool(bezier));
    ASSERT_FALSE(ctx->getLayers().empty());
    RotoLayerPtr baseLayer = ctx->getLayers().front();

    const std::size_t nodesBefore = countNodes();
    BezierPtr copy = std::dynamic_pointer_cast<Bezier>(ctx->makeDrawableCopy(bezier, "QuadCopy"));
    ASSERT_TRUE(bool(copy));
    EXPECT_EQ(std::string("QuadCopy"), copy->getScriptName());
    EXPECT_EQ(4, copy->getControlPointsCount());
    EXPECT_FALSE(bool(copy->getEffectNode()));
    EXPECT_FALSE(bool(copy->getMergeNode()));

    RotoLayerPtr layerCopy(new RotoLayer(ctx, "LayerCopy", RotoLayerPtr()));
    layerCopy->clone(baseLayer.get());
    const std::list<RotoItemPtr> copiedItems = layerCopy->getItems_mt_safe();
    ASSERT_FALSE(copiedItems.empty());
    for (std::list<RotoItemPtr>::const_iterator it = copiedItems.begin(); it != copiedItems.end(); ++it) {
        RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(*it);
        ASSERT_TRUE(bool(drawable));
        EXPECT_FALSE(bool(drawable->getEffectNode()));
        EXPECT_FALSE(bool(drawable->getMergeNode()));
    }

    EXPECT_EQ(nodesBefore, countNodes());

    // Duplicating a non-empty layer, as the curve panel does.
    RotoLayerPtr duplicate;
    ASSERT_NO_THROW(duplicate = ctx->makeLayerCopy(baseLayer));
    ASSERT_TRUE(bool(duplicate));
    EXPECT_EQ(baseLayer->getItems_mt_safe().size(), duplicate->getItems_mt_safe().size());
    EXPECT_EQ(nodesBefore, countNodes());
}

TEST_F(CurveHostTest, CopyingRotoPaintCurvesStillCreatesTheirNodes)
{
    NodePtr paintNode = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(paintNode));
    RotoContextPtr paint = paintNode->getRotoContext();
    ASSERT_TRUE(bool(paint));
    BezierPtr square = paint->makeSquare(0., 100., 100., 1.);
    ASSERT_TRUE(bool(square));
    ASSERT_FALSE(paint->getLayers().empty());

    // A stroke built the way the paint tool builds one: nodes first, then inserted into its layer.
    const int strokeSamples = 12;
    RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, paint, "Stroke", RotoLayerPtr()));
    stroke->createNodes();
    paint->addItem(paint->getLayers().front(), 0, stroke, RotoItem::eSelectionReasonOther);
    for (int i = 0; i < strokeSamples; ++i) {
        stroke->appendPoint(i == 0, RotoPoint(4. + i, 4. + 2. * i, 0.5, 1. + 0.1 * i));
    }
    stroke->setStrokeFinished();
    Serialization::RotoStrokeItemSerialization sourceStroke;
    stroke->toSerialization(&sourceStroke);
    ASSERT_FALSE(sourceStroke.strokes.empty());

    RotoDrawableItemPtr copy = paint->makeDrawableCopy(square, "SquareCopy");
    ASSERT_TRUE(bool(copy));
    EXPECT_TRUE(bool(copy->getEffectNode()));
    EXPECT_TRUE(bool(copy->getMergeNode()));
    EXPECT_NE(square->getEffectNode(), copy->getEffectNode());

    RotoLayerPtr layerCopy(new RotoLayer(paint, "LayerCopy", RotoLayerPtr()));
    layerCopy->clone(paint->getLayers().front().get());
    const std::list<RotoItemPtr> copiedItems = layerCopy->getItems_mt_safe();
    ASSERT_FALSE(copiedItems.empty());
    for (std::list<RotoItemPtr>::const_iterator it = copiedItems.begin(); it != copiedItems.end(); ++it) {
        RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(*it);
        ASSERT_TRUE(bool(drawable));
        EXPECT_TRUE(bool(drawable->getEffectNode()));
        EXPECT_TRUE(bool(drawable->getMergeNode()));
    }

    // Duplicating a non-empty layer, as the curve panel does.
    RotoLayerPtr duplicate;
    ASSERT_NO_THROW(duplicate = paint->makeLayerCopy(paint->getLayers().front()));
    ASSERT_TRUE(bool(duplicate));
    const std::list<RotoItemPtr> duplicatedItems = duplicate->getItems_mt_safe();
    ASSERT_FALSE(duplicatedItems.empty());
    bool foundStroke = false;
    for (std::list<RotoItemPtr>::const_iterator it = duplicatedItems.begin(); it != duplicatedItems.end(); ++it) {
        RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(*it);
        ASSERT_TRUE(bool(drawable));
        EXPECT_TRUE(bool(drawable->getEffectNode()));
        EXPECT_TRUE(bool(drawable->getMergeNode()));
        RotoStrokeItemPtr duplicatedStroke = std::dynamic_pointer_cast<RotoStrokeItem>(*it);
        if (duplicatedStroke) {
            foundStroke = true;
            EXPECT_EQ("Stroke", duplicatedStroke->getScriptName());
            Serialization::RotoStrokeItemSerialization copied;
            duplicatedStroke->toSerialization(&copied);
            ASSERT_EQ(sourceStroke.strokes.size(), copied.strokes.size());
            for (std::size_t i = 0; i < copied.strokes.size(); ++i) {
                auto source = sourceStroke.strokes.begin();
                auto dup = copied.strokes.begin();
                std::advance(source, i);
                std::advance(dup, i);
                EXPECT_EQ(source->xCurve.keyframes.size(), dup->xCurve.keyframes.size());
                EXPECT_EQ(source->yCurve.keyframes.size(), dup->yCurve.keyframes.size());
                EXPECT_GT(dup->xCurve.keyframes.size(), (std::size_t)1);
            }
        }
    }
    EXPECT_TRUE(foundStroke);
}

TEST_F(CurveHostTest, MovingAControlPointChangesTheContentHash)
{
    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDCurveHost));
    ASSERT_TRUE(bool(host));
    RotoCurveContextPtr ctx = host->getCurveContext();
    ASSERT_TRUE(bool(ctx));
    BezierPtr bezier = makeQuad(ctx);
    ASSERT_TRUE(bool(bezier));

    const U64 contextBefore = ctx->getContentHash();
    const U64 nodeBefore = host->getContentHash();
    bezier->movePointByIndex(0, 1., 10., 5.);

    EXPECT_NE(contextBefore, ctx->getContentHash());
    EXPECT_NE(nodeBefore, host->getContentHash());
}

TEST_F(CurveHostTest, TheBezierHasFeatherPoints)
{
    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDCurveHost));
    ASSERT_TRUE(bool(host));
    BezierPtr bezier = makeQuad(host->getCurveContext());
    ASSERT_TRUE(bool(bezier));

    std::list<BezierCPPtr> points = bezier->getControlPoints_mt_safe();
    std::list<BezierCPPtr> feather = bezier->getFeatherPoints_mt_safe();
    EXPECT_EQ(std::size_t(4), points.size());
    EXPECT_EQ(points.size(), feather.size());
    for (std::list<BezierCPPtr>::const_iterator it = feather.begin(); it != feather.end(); ++it) {
        EXPECT_TRUE((*it)->isFeatherPoint());
    }
}

TEST_F(CurveHostTest, TheHostHasTheCurveViewerAndMenuKnobs)
{
    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDCurveHost));
    ASSERT_TRUE(bool(host));
    EffectInstancePtr effect = host->getEffectInstance();

    const char* viewerButtons[] = {
        kRotoUIParamAutoKeyingEnabled,
        kRotoUIParamFeatherLinkEnabled,
        kRotoUIParamDisplayFeather,
        kRotoUIParamStickySelectionEnabled,
        kRotoUIParamStickyBbox,
        kRotoUIParamRippleEdit,
        kRotoUIParamAddKeyFrame,
        kRotoUIParamRemoveKeyframe,
        kRotoUIParamShowTransform,
    };
    for (std::size_t i = 0; i < sizeof(viewerButtons) / sizeof(viewerButtons[0]); ++i) {
        KnobIPtr knob = effect->getKnobByName(viewerButtons[i]);
        ASSERT_TRUE(bool(knob)) << viewerButtons[i];
        EXPECT_TRUE(effect->isInViewerUIKnob(knob)) << viewerButtons[i];
    }

    const char* menuKnobs[] = {
        kRotoUIParamRightClickMenu,
        kRotoUIParamRightClickMenuActionRemoveItems,
        kRotoUIParamRightClickMenuActionCuspItems,
        kRotoUIParamRightClickMenuActionSmoothItems,
        kRotoUIParamRightClickMenuActionRemoveItemsFeather,
        kRotoUIParamRightClickMenuActionNudgeBottom,
        kRotoUIParamRightClickMenuActionNudgeLeft,
        kRotoUIParamRightClickMenuActionNudgeTop,
        kRotoUIParamRightClickMenuActionNudgeRight,
        kRotoUIParamRightClickMenuActionSelectAll,
        kRotoUIParamRightClickMenuActionOpenClose,
        kRotoUIParamRightClickMenuActionLockShapes,
    };
    for (std::size_t i = 0; i < sizeof(menuKnobs) / sizeof(menuKnobs[0]); ++i) {
        EXPECT_TRUE(bool(effect->getKnobByName(menuKnobs[i]))) << menuKnobs[i];
    }

    EXPECT_FALSE(bool(effect->getKnobByName(kRotoUIParamMultiStrokeEnabled)));
    EXPECT_FALSE(bool(effect->getKnobByName(kRotoUIParamBlendingOp)));
}

TEST_F(CurveHostTest, ASelectionChangeRedrawsTheOverlay)
{
    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDCurveHost));
    ASSERT_TRUE(bool(host));
    CurveHostTestEffect* effect = dynamic_cast<CurveHostTestEffect*>(host->getEffectInstance().get());
    ASSERT_TRUE(effect);
    RotoCurveContextPtr ctx = host->getCurveContext();
    BezierPtr bezier = makeQuad(ctx);
    ASSERT_TRUE(bool(bezier));
    ctx->clearSelection(RotoItem::eSelectionReasonOther);

    const int redrawsBefore = effect->getRedrawRequests();
    ctx->select(bezier, RotoItem::eSelectionReasonSettingsPanel);

    EXPECT_GT(effect->getRedrawRequests(), redrawsBefore);
    const SelectedItems& selected = effect->getInteract()->selectedItems;
    EXPECT_NE(selected.end(), std::find(selected.begin(), selected.end(), bezier));
}

TEST_F(CurveHostTest, SaveAndLoadRoundTripsTheCurves)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDCurveHost));
    ASSERT_TRUE(bool(host));
    RotoCurveContextPtr ctx = host->getCurveContext();
    ASSERT_TRUE(bool(ctx));
    BezierPtr bezier = makeQuad(ctx);
    ASSERT_TRUE(bool(bezier));
    bezier->movePointByIndex(2, 1., 7., -3.);

    const std::string hostName = host->getScriptName();
    const std::string bezierName = bezier->getScriptName();
    const U64 savedHash = ctx->getContentHash();
    double savedX = 0., savedY = 0.;
    ASSERT_TRUE(bezier->getControlPointAtIndex(2)->getPositionAtTime(false, 1., ViewIdx(0), &savedX, &savedY));

    std::string yaml;
    ProjectPtr project = getApp()->getProject();
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("curvehost.ntpz"), &yaml));
    host.reset();
    bezier.reset();
    ctx.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("curvehost.ntpz")));

    NodePtr loaded = project->getNodeByName(hostName);
    ASSERT_TRUE(bool(loaded));
    RotoCurveContextPtr loadedCtx = loaded->getCurveContext();
    ASSERT_TRUE(bool(loadedCtx));
    EXPECT_FALSE(bool(loaded->getRotoContext()));
    BezierPtr loadedBezier = std::dynamic_pointer_cast<Bezier>(loadedCtx->getItemByName(bezierName));
    ASSERT_TRUE(bool(loadedBezier));

    EXPECT_EQ(4, loadedBezier->getControlPointsCount());
    EXPECT_EQ(std::size_t(4), loadedBezier->getFeatherPoints_mt_safe().size());
    EXPECT_FALSE(bool(loadedBezier->getEffectNode()));
    double x = 0., y = 0.;
    ASSERT_TRUE(loadedBezier->getControlPointAtIndex(2)->getPositionAtTime(false, 1., ViewIdx(0), &x, &y));
    EXPECT_EQ(savedX, x);
    EXPECT_EQ(savedY, y);
    EXPECT_EQ(savedHash, loadedCtx->getContentHash());
}

TEST_F(CurveHostTest, RotoResolvesFromPython)
{
    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDCurveHost));
    ASSERT_TRUE(bool(host));
    BezierPtr bezier = makeQuad(host->getCurveContext());
    ASSERT_TRUE(bool(bezier));

    // A failed assert raises, so interpretPythonScript() reports it.
    const std::string node = getApp()->getAppIDString() + "." + host->getFullyQualifiedName();
    const std::string script = "assert " + node + ".roto is not None\n"
                                                  "assert "
        + node + ".roto.getBaseLayer() is not None\n"
                 "assert "
        + node + ".roto.getItemByName(\"" + bezier->getScriptName() + "\").getScriptName() == \"" + bezier->getScriptName() + "\"\n";
    std::string error, output;
    EXPECT_TRUE(interpretPythonScript(script, &error, &output)) << error;
}

TEST_F(CurveHostTest, RotoPaintInTheSameProjectStillBuildsItsTree)
{
    NodePtr host = createNode(QString::fromUtf8(kTestPluginIDCurveHost));
    ASSERT_TRUE(bool(host));
    ASSERT_TRUE(bool(makeQuad(host->getCurveContext())));

    NodePtr paintNode = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(paintNode));
    RotoContextPtr paint = paintNode->getRotoContext();
    ASSERT_TRUE(bool(paint));
    BezierPtr square = paint->makeSquare(0., 100., 100., 1.);
    ASSERT_TRUE(bool(square));

    NodePtr effectNode = square->getEffectNode();
    NodePtr mergeNode = square->getMergeNode();
    ASSERT_TRUE(bool(effectNode));
    ASSERT_TRUE(bool(mergeNode));
    NodesList tree;
    paint->getRotoPaintTreeNodes(&tree);
    EXPECT_NE(tree.end(), std::find(tree.begin(), tree.end(), effectNode));
    EXPECT_NE(tree.end(), std::find(tree.begin(), tree.end(), mergeNode));
}
