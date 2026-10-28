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
#include <cmath>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QPointF>
#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

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
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/UndoCommand.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

SplineWarpContextPtr
contextOf(const NodePtr& node)
{
    return node ? std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext()) : SplineWarpContextPtr();
}

SplineWarpInteractPtr
interactOf(const NodePtr& node)
{
    SplineWarp* effect = node ? dynamic_cast<SplineWarp*>(node->getEffectInstance().get()) : 0;

    return effect ? effect->getInteract() : SplineWarpInteractPtr();
}

BezierPtr
makeCurve(const SplineWarpContextPtr& ctx,
          const std::vector<std::pair<double, double>>& points,
          bool closed)
{
    BezierPtr bezier = ctx->makeBezier(points[0].first, points[0].second, kRotoBezierBaseName, kTime, !closed);
    if (!bezier) {
        return bezier;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        bezier->addControlPoint(points[i].first, points[i].second, kTime);
    }
    bezier->setCurveFinished(true);

    return bezier;
}

std::vector<std::pair<double, double>>
quadPoints(double x,
           double y,
           double size,
           double dx = 0.,
           double dy = 0.)
{
    std::vector<std::pair<double, double>> points;
    points.push_back(std::make_pair(x + dx, y + dy));
    points.push_back(std::make_pair(x + size + dx, y + dy));
    points.push_back(std::make_pair(x + size * 1.2 + dx, y + size + dy));
    points.push_back(std::make_pair(x - size * 0.1 + dx, y + size * 0.8 + dy));

    return points;
}

BezierPtr
makeQuad(const SplineWarpContextPtr& ctx,
         double x,
         double y,
         double size,
         double dx = 0.,
         double dy = 0.)
{
    return makeCurve(ctx, quadPoints(x, y, size, dx, dy), true);
}

BezierPtr
makeOpenLine(const SplineWarpContextPtr& ctx,
             double x,
             double y)
{
    std::vector<std::pair<double, double>> points;
    points.push_back(std::make_pair(x, y));
    points.push_back(std::make_pair(x + 10., y + 4.));
    points.push_back(std::make_pair(x + 20., y - 2.));

    return makeCurve(ctx, points, false);
}

void
select(const SplineWarpInteractPtr& ui,
       const std::list<BezierPtr>& curves)
{
    std::list<RotoDrawableItemPtr> items(curves.begin(), curves.end());

    ui->setSelection(items, std::list<std::pair<BezierCPPtr, BezierCPPtr>>());
}

void
select(const SplineWarpInteractPtr& ui,
       const BezierPtr& curve)
{
    select(ui, std::list<BezierPtr>(1, curve));
}

QPointF
pointOf(const BezierPtr& curve,
        double u,
        double time = kTime)
{
    const Point p = SplineWarpCurveShape::fromBezier(*curve, time).pointAt(u);

    return QPointF(p.x, p.y);
}

bool
renderRGBA(const NodePtr& node,
           RenderedPlane* plane)
{
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

double
maxDiff(const RenderedPlane& a,
        const RenderedPlane& b)
{
    EXPECT_EQ(a.pixels.size(), b.pixels.size());
    double diff = 0.;
    const std::size_t n = std::min(a.pixels.size(), b.pixels.size());
    for (std::size_t i = 0; i < n; ++i) {
        diff = std::max(diff, (double)std::abs(a.pixels[i] - b.pixels[i]));
    }

    return diff;
}

void
correspondenceAt(const SplineWarpLinkPtr& link,
                 int index,
                 double time,
                 double* s,
                 double* d)
{
    *s = -1.;
    *d = -1.;
    EXPECT_TRUE(link->getCorrespondence(index, time, s, d)) << index;
}

} // namespace

class SplineWarpToolsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "splineWarpTools64x48", 1.));
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    NodePtr makeSplineWarp(const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);

        EXPECT_TRUE(bool(node));
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }
};

TEST_F(SplineWarpToolsTest, TheToolbarHoldsTheCurveAndCorrespondenceTools)
{
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));
    SplineWarpInteractPtr ui = interactOf(node);
    ASSERT_TRUE(bool(ui));
    EXPECT_EQ(eRotoToolSelectAll, ui->getSelectedTool());

    KnobPagePtr toolbar = std::dynamic_pointer_cast<KnobPage>(node->getKnobByName(kRotoUIParamToolbar));
    ASSERT_TRUE(bool(toolbar));
    EXPECT_TRUE(toolbar->getIsToolBar());
    std::vector<KnobIPtr> groups = toolbar->getChildren();
    const char* const groupNames[] = {
        kRotoUIParamSelectionToolButton,
        kRotoUIParamEditPointsToolButton,
        kRotoUIParamBezierEditionToolButton,
        kSplineWarpUIParamCorrespondenceTools,
    };
    ASSERT_EQ(sizeof(groupNames) / sizeof(groupNames[0]), groups.size());
    for (std::size_t i = 0; i < groups.size(); ++i) {
        EXPECT_EQ(std::string(groupNames[i]), groups[i]->getName());
    }

    KnobGroupPtr shapes = std::dynamic_pointer_cast<KnobGroup>(groups[2]);
    ASSERT_TRUE(bool(shapes));
    std::vector<std::string> shapeTools;
    std::vector<KnobIPtr> children = shapes->getChildren();
    for (std::size_t i = 0; i < children.size(); ++i) {
        shapeTools.push_back(children[i]->getName());
    }
    EXPECT_NE(shapeTools.end(), std::find(shapeTools.begin(), shapeTools.end(), std::string(kRotoUIParamOpenBezierToolButtonAction)));
    EXPECT_NE(shapeTools.end(), std::find(shapeTools.begin(), shapeTools.end(), std::string(kRotoUIParamPointToolButtonAction)));

    KnobGroupPtr correspondences = std::dynamic_pointer_cast<KnobGroup>(groups[3]);
    ASSERT_TRUE(bool(correspondences));
    EXPECT_EQ(std::string(kSplineWarpUIParamCorrespondenceToolsLabel), correspondences->getLabel());
    children = correspondences->getChildren();
    ASSERT_EQ(std::size_t(1), children.size());
    EXPECT_EQ(std::string(kSplineWarpUIParamCorrespondenceTool), children[0]->getName());
    EXPECT_EQ(std::string(kSplineWarpUIParamCorrespondenceToolLabel), children[0]->getLabel());

    KnobButtonPtr tool = std::dynamic_pointer_cast<KnobButton>(children[0]);
    ASSERT_TRUE(bool(tool));
    RotoToolEnum mapped = eRotoToolSelectAll;
    EXPECT_TRUE(ui->getToolForAction(tool, &mapped));
    EXPECT_EQ(eRotoToolCorrespondence, mapped);
    RotoRoleEnum role = eRotoRoleSelection;
    EXPECT_TRUE(ui->getRoleForGroup(correspondences, &role));
    EXPECT_EQ(eRotoRoleCorrespondence, role);
}

TEST_F(SplineWarpToolsTest, ThePointToolMakesAOnePointOpenCurve)
{
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));
    SplineWarpInteractPtr ui = interactOf(node);
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(ui && ctx);

    BezierPtr point = ui->makePointCurve(12., 34., kTime);
    ASSERT_TRUE(bool(point));
    EXPECT_EQ(1, point->getControlPointsCount());
    EXPECT_TRUE(point->isOpenBezier());
    EXPECT_TRUE(point->isCurveFinished());
    EXPECT_FALSE(SplineWarpCurveShape::isClosedCurve(*point));
    EXPECT_EQ(point, std::dynamic_pointer_cast<Bezier>(ctx->getItemByName(point->getScriptName())));
    const QPointF at = pointOf(point, 0.);
    EXPECT_DOUBLE_EQ(12., at.x());
    EXPECT_DOUBLE_EQ(34., at.y());
    ASSERT_EQ(std::size_t(1), ui->selectedItems.size());
    EXPECT_EQ(point, std::dynamic_pointer_cast<Bezier>(ui->selectedItems.front()));

    // A single point links to a curve like any other.
    BezierPtr quad = makeQuad(ctx, 10., 10., 16.);
    ASSERT_TRUE(bool(quad));
    select(ui, std::list<BezierPtr> { point, quad });
    EXPECT_EQ(std::string(), ui->linkSelected());
    EXPECT_TRUE(bool(ctx->getLink(point)));
}

TEST_F(SplineWarpToolsTest, DuplicateAndLinkStartsAtIdentityAndUndoes)
{
    NodePtr source = makeSource();
    NodePtr node = makeSplineWarp(source);
    ASSERT_TRUE(source && node);
    SplineWarpInteractPtr ui = interactOf(node);
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(ui && ctx);

    BezierPtr original = makeQuad(ctx, 20., 15., 16.);
    ASSERT_TRUE(bool(original));
    const U64 hashBefore = ctx->getContentHash();
    select(ui, original);
    UndoCommandPtr command;
    ASSERT_EQ(std::string(), ui->duplicateAndLink(&command));
    ASSERT_TRUE(bool(command));

    SplineWarpLinkPtr link = ctx->getLink(original);
    ASSERT_TRUE(bool(link));
    EXPECT_EQ(original, link->getSource());
    BezierPtr copy = link->getDestination();
    ASSERT_TRUE(bool(copy));
    EXPECT_NE(original, copy);
    EXPECT_EQ(0., link->getStartOffset());
    EXPECT_FALSE(link->isReversed());
    EXPECT_EQ(0, link->getCorrespondenceCount());
    ASSERT_EQ(std::size_t(1), ui->selectedItems.size());
    EXPECT_EQ(copy, std::dynamic_pointer_cast<Bezier>(ui->selectedItems.front()));
    // Just above the original in its layer.
    RotoLayerPtr layer = original->getParentLayer();
    ASSERT_TRUE(bool(layer));
    EXPECT_EQ(layer, copy->getParentLayer());
    EXPECT_EQ(layer->getChildIndex(original) - 1, layer->getChildIndex(copy));
    for (int i = 0; i < 4; ++i) {
        const QPointF a = pointOf(original, i);
        const QPointF b = pointOf(copy, i);
        EXPECT_EQ(a.x(), b.x()) << i;
        EXPECT_EQ(a.y(), b.y()) << i;
    }

    RenderedPlane warped, plain;
    ASSERT_TRUE(renderRGBA(node, &warped));
    ASSERT_TRUE(renderRGBA(source, &plain));
    EXPECT_EQ(0., maxDiff(warped, plain));

    const U64 hashAfter = ctx->getContentHash();
    EXPECT_NE(hashBefore, hashAfter);
    const std::string copyName = copy->getScriptName();
    command->undo();
    EXPECT_TRUE(ctx->getLinks().empty());
    EXPECT_FALSE(bool(ctx->getItemByName(copyName)));
    EXPECT_EQ(hashBefore, ctx->getContentHash());
    command->redo();
    ASSERT_EQ(std::size_t(1), ctx->getLinks().size());
    EXPECT_EQ(link, ctx->getLinks()[0]);
    EXPECT_EQ(copy, link->getDestination());
    EXPECT_EQ(copy, std::dynamic_pointer_cast<Bezier>(ctx->getItemByName(copyName)));
    EXPECT_EQ(hashAfter, ctx->getContentHash());
}

TEST_F(SplineWarpToolsTest, LinkAlignsARestartedCopyAndUndoes)
{
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));
    SplineWarpInteractPtr ui = interactOf(node);
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(ui && ctx);

    const std::vector<std::pair<double, double>> points = quadPoints(10., 10., 20.);
    std::vector<std::pair<double, double>> restarted;
    for (int i = 0; i < 4; ++i) {
        restarted.push_back(std::make_pair(points[(i + 2) % 4].first + 6., points[(i + 2) % 4].second - 3.));
    }
    BezierPtr source = makeCurve(ctx, points, true);
    BezierPtr destination = makeCurve(ctx, restarted, true);
    ASSERT_TRUE(source && destination);
    const U64 hashBefore = ctx->getContentHash();

    // Selection order makes the source, whatever the panel order.
    select(ui, std::list<BezierPtr> { source, destination });
    UndoCommandPtr command;
    ASSERT_EQ(std::string(), ui->linkSelected(&command));
    ASSERT_TRUE(bool(command));
    SplineWarpLinkPtr link = ctx->getLink(source);
    ASSERT_TRUE(bool(link));
    EXPECT_EQ(source, link->getSource());
    EXPECT_EQ(destination, link->getDestination());
    EXPECT_FALSE(link->isReversed());
    // Source point 0 is destination point 2.
    EXPECT_NEAR(2., link->getStartOffset(), 0.01);
    EXPECT_EQ(0, link->getCorrespondenceCount());

    const U64 hashAfter = ctx->getContentHash();
    command->undo();
    EXPECT_FALSE(bool(ctx->getLink(source)));
    EXPECT_EQ(hashBefore, ctx->getContentHash());
    command->redo();
    EXPECT_EQ(link, ctx->getLink(source));
    EXPECT_EQ(hashAfter, ctx->getContentHash());

    // Unlink, and its undo.
    select(ui, destination);
    ASSERT_EQ(std::string(), ui->unlinkSelected(&command));
    EXPECT_TRUE(ctx->getLinks().empty());
    command->undo();
    EXPECT_EQ(link, ctx->getLink(destination));
    EXPECT_EQ(hashAfter, ctx->getContentHash());
    command->redo();
    EXPECT_TRUE(ctx->getLinks().empty());
}

TEST_F(SplineWarpToolsTest, SwapMatchesInvert)
{
    NodePtr source = makeSource();
    NodePtr node = makeSplineWarp(source);
    ASSERT_TRUE(source && node);
    SplineWarpInteractPtr ui = interactOf(node);
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(ui && ctx);

    BezierPtr from = makeQuad(ctx, 20., 15., 16.);
    BezierPtr to = makeQuad(ctx, 20., 15., 16., 3., -2.);
    ASSERT_TRUE(from && to);
    SplineWarpLinkPtr link = ctx->linkWithAlignment(from, to, 0., false);
    ASSERT_TRUE(bool(link));
    ctx->addCorrespondence(link, 1.5, 1.5);

    ASSERT_TRUE(setKnobValues(node, kSplineWarpParamInvert, { 1. }));
    RenderedPlane inverted;
    ASSERT_TRUE(renderRGBA(node, &inverted));
    ASSERT_TRUE(setKnobValues(node, kSplineWarpParamInvert, { 0. }));

    const U64 hashBefore = ctx->getContentHash();
    select(ui, from);
    UndoCommandPtr command;
    ASSERT_EQ(std::string(), ui->swapSelected(&command));
    EXPECT_EQ(to, link->getSource());
    EXPECT_EQ(from, link->getDestination());
    EXPECT_FALSE(link->isReversed());
    // Both curves start at the same point, so the walk start is 0 (or the whole loop).
    EXPECT_NEAR(0., std::min(link->getStartOffset(), 4. - link->getStartOffset()), 1e-9) << link->getStartOffset();
    ASSERT_EQ(1, link->getCorrespondenceCount());

    RenderedPlane swapped;
    ASSERT_TRUE(renderRGBA(node, &swapped));
    EXPECT_LE(maxDiff(inverted, swapped), 1e-5);

    command->undo();
    EXPECT_EQ(from, link->getSource());
    EXPECT_EQ(to, link->getDestination());
    EXPECT_EQ(hashBefore, ctx->getContentHash());
    command->redo();
    EXPECT_EQ(to, link->getSource());
}

TEST_F(SplineWarpToolsTest, SwapKeepsTheCorrespondenceKeys)
{
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));
    SplineWarpInteractPtr ui = interactOf(node);
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(ui && ctx);

    BezierPtr from = makeQuad(ctx, 10., 10., 16.);
    BezierPtr to = makeQuad(ctx, 10., 10., 16., 5., 5.);
    ASSERT_TRUE(from && to);
    SplineWarpLinkPtr link = ctx->linkWithAlignment(from, to, 0., false);
    ASSERT_TRUE(bool(link));
    const int index = ctx->addCorrespondence(link, 1.25, 1.75);
    ctx->setCorrespondenceAtTime(link, index, 10., 1.5, 2.5);

    select(ui, to);
    ASSERT_EQ(std::string(), ui->swapSelected());
    std::vector<SplineWarpCorrespondence> swapped = link->getCorrespondences();
    ASSERT_EQ(std::size_t(1), swapped.size());
    EXPECT_TRUE(swapped[0].source.isAnimated());
    EXPECT_TRUE(swapped[0].destination.isAnimated());
    EXPECT_DOUBLE_EQ(2.5, swapped[0].source.getValueAt(10.));
    EXPECT_DOUBLE_EQ(1.5, swapped[0].destination.getValueAt(10.));
}

TEST_F(SplineWarpToolsTest, ActionsRefuseWhatTheyCannotDo)
{
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));
    SplineWarpInteractPtr ui = interactOf(node);
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(ui && ctx);

    BezierPtr a = makeQuad(ctx, 10., 10., 10.);
    BezierPtr b = makeQuad(ctx, 30., 10., 10.);
    BezierPtr c = makeQuad(ctx, 10., 30., 10.);
    BezierPtr boundary = makeQuad(ctx, 0., 0., 60.);
    BezierPtr open = makeOpenLine(ctx, 5., 40.);
    ASSERT_TRUE(a && b && c && boundary && open);
    ASSERT_TRUE(bool(ctx->linkWithAlignment(a, b, 0., false)));
    ctx->setBoundary(boundary, true);
    const U64 hash = ctx->getContentHash();
    UndoCommandPtr command;

    // Duplicate and Link: nothing selected, two selected, a linked curve, a boundary.
    select(ui, std::list<BezierPtr>());
    EXPECT_NE(std::string(), ui->duplicateAndLink(&command));
    select(ui, std::list<BezierPtr> { c, open });
    EXPECT_NE(std::string(), ui->duplicateAndLink(&command));
    select(ui, a);
    EXPECT_NE(std::string(), ui->duplicateAndLink(&command));
    select(ui, boundary);
    EXPECT_NE(std::string(), ui->duplicateAndLink(&command));

    // Link: one curve, three curves, a linked curve, a boundary.
    select(ui, c);
    EXPECT_NE(std::string(), ui->linkSelected(&command));
    select(ui, std::list<BezierPtr> { c, open, a });
    EXPECT_NE(std::string(), ui->linkSelected(&command));
    select(ui, std::list<BezierPtr> { c, b });
    EXPECT_NE(std::string(), ui->linkSelected(&command));
    select(ui, std::list<BezierPtr> { boundary, c });
    EXPECT_NE(std::string(), ui->linkSelected(&command));

    // Unlink and Swap: no linked curve selected.
    select(ui, c);
    EXPECT_NE(std::string(), ui->unlinkSelected(&command));
    EXPECT_NE(std::string(), ui->swapSelected(&command));

    // Toggle Boundary: an open curve, a linked curve, nothing.
    select(ui, open);
    EXPECT_NE(std::string(), ui->canToggleBoundary());
    EXPECT_NE(std::string(), ui->toggleBoundarySelected(&command));
    select(ui, std::list<BezierPtr> { c, a });
    EXPECT_NE(std::string(), ui->canToggleBoundary());
    EXPECT_NE(std::string(), ui->toggleBoundarySelected(&command));
    select(ui, std::list<BezierPtr>());
    EXPECT_NE(std::string(), ui->canToggleBoundary());

    EXPECT_FALSE(bool(command));
    EXPECT_EQ(hash, ctx->getContentHash());
    EXPECT_EQ(std::size_t(1), ctx->getLinks().size());
    EXPECT_FALSE(ctx->isBoundary(c));
    EXPECT_EQ(std::size_t(1), ctx->getBoundaries().size());
}

TEST_F(SplineWarpToolsTest, ToggleBoundaryAndUndo)
{
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));
    SplineWarpInteractPtr ui = interactOf(node);
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(ui && ctx);

    BezierPtr a = makeQuad(ctx, 10., 10., 10.);
    BezierPtr b = makeQuad(ctx, 30., 10., 10.);
    ASSERT_TRUE(a && b);
    ctx->setBoundary(a, true);
    const U64 hashBefore = ctx->getContentHash();

    // Mixed: all become boundaries.
    select(ui, std::list<BezierPtr> { a, b });
    EXPECT_EQ(std::string(), ui->canToggleBoundary());
    UndoCommandPtr command;
    ASSERT_EQ(std::string(), ui->toggleBoundarySelected(&command));
    EXPECT_TRUE(ctx->isBoundary(a));
    EXPECT_TRUE(ctx->isBoundary(b));
    const U64 hashAfter = ctx->getContentHash();
    command->undo();
    EXPECT_TRUE(ctx->isBoundary(a));
    EXPECT_FALSE(ctx->isBoundary(b));
    EXPECT_EQ(hashBefore, ctx->getContentHash());
    command->redo();
    EXPECT_TRUE(ctx->isBoundary(b));
    EXPECT_EQ(hashAfter, ctx->getContentHash());

    // All boundaries: none stay.
    ASSERT_EQ(std::string(), ui->toggleBoundarySelected());
    EXPECT_FALSE(ctx->isBoundary(a));
    EXPECT_FALSE(ctx->isBoundary(b));
}

TEST_F(SplineWarpToolsTest, CorrespondencesAreMadeMovedAndDeleted)
{
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));
    SplineWarpInteractPtr ui = interactOf(node);
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(ui && ctx);
    ctx->onAutoKeyingChanged(false);

    BezierPtr from = makeQuad(ctx, 10., 10., 20.);
    BezierPtr to = makeQuad(ctx, 10., 10., 20., 12., 6.);
    BezierPtr loose = makeQuad(ctx, 40., 30., 6.);
    ASSERT_TRUE(from && to && loose);
    SplineWarpLinkPtr link = ctx->linkWithAlignment(from, to, 0., false);
    ASSERT_TRUE(bool(link));
    const double tolerance = 1.;
    const U64 hashBefore = ctx->getContentHash();

    // An unlinked curve, and empty space, are no-ops.
    EXPECT_FALSE(ui->pressCorrespondence(pointOf(loose, 0.5), kTime, tolerance));
    EXPECT_FALSE(ui->pressCorrespondence(QPointF(-50., -50.), kTime, tolerance));
    EXPECT_FALSE(ui->hasSelectedCorrespondence());
    EXPECT_EQ(0, link->getCorrespondenceCount());

    // On the source: the partner is the same point of the translated destination.
    ASSERT_TRUE(ui->pressCorrespondence(pointOf(from, 0.5), kTime, tolerance));
    UndoCommandPtr command;
    ASSERT_TRUE(ui->releaseCorrespondence(&command));
    ASSERT_TRUE(bool(command));
    ASSERT_EQ(1, link->getCorrespondenceCount());
    double s, d;
    correspondenceAt(link, 0, kTime, &s, &d);
    EXPECT_NEAR(0.5, s, 1e-3);
    EXPECT_NEAR(0.5, d, 1e-3);
    std::vector<SplineWarpCorrespondence> values = link->getCorrespondences();
    EXPECT_FALSE(values[0].source.isAnimated());
    EXPECT_FALSE(values[0].destination.isAnimated());
    const U64 hashOne = ctx->getContentHash();
    command->undo();
    EXPECT_EQ(0, link->getCorrespondenceCount());
    EXPECT_EQ(hashBefore, ctx->getContentHash());
    command->redo();
    EXPECT_EQ(1, link->getCorrespondenceCount());
    EXPECT_EQ(hashOne, ctx->getContentHash());

    // On the destination.
    ASSERT_TRUE(ui->pressCorrespondence(pointOf(to, 2.5), kTime, tolerance));
    ASSERT_TRUE(ui->releaseCorrespondence());
    ASSERT_EQ(2, link->getCorrespondenceCount());
    correspondenceAt(link, 1, kTime, &s, &d);
    EXPECT_NEAR(2.5, s, 1e-3);
    EXPECT_NEAR(2.5, d, 1e-3);

    // Dragging the second's source end past the first is held just after it.
    ASSERT_TRUE(ui->pressCorrespondence(pointOf(from, 2.5), kTime, tolerance));
    SplineWarpLinkPtr selectedLink;
    int selectedIndex = -1;
    ASSERT_TRUE(ui->getSelectedCorrespondence(&selectedLink, &selectedIndex));
    EXPECT_EQ(link, selectedLink);
    EXPECT_EQ(1, selectedIndex);
    const U64 hashTwo = ctx->getContentHash();
    ASSERT_TRUE(ui->dragCorrespondence(pointOf(from, 0.2), kTime));
    ASSERT_TRUE(ui->releaseCorrespondence(&command));
    correspondenceAt(link, 1, kTime, &s, &d);
    EXPECT_GT(s, 0.5);
    EXPECT_LT(s, 0.52);
    EXPECT_NEAR(2.5, d, 1e-3);
    command->undo();
    correspondenceAt(link, 1, kTime, &s, &d);
    EXPECT_NEAR(2.5, s, 1e-3);
    EXPECT_EQ(hashTwo, ctx->getContentHash());
    command->redo();
    correspondenceAt(link, 1, kTime, &s, &d);
    EXPECT_GT(s, 0.5);

    // A drag along the curve stays on it.
    ASSERT_TRUE(ui->pressCorrespondence(pointOf(to, 2.5), kTime, tolerance));
    ASSERT_TRUE(ui->dragCorrespondence(pointOf(to, 3.2) + QPointF(0.3, 0.3), kTime));
    ASSERT_TRUE(ui->releaseCorrespondence());
    correspondenceAt(link, 1, kTime, &s, &d);
    EXPECT_NEAR(3.2, d, 0.05);

    // Delete, and its undo.
    ASSERT_TRUE(ui->pressCorrespondence(pointOf(to, 0.5), kTime, tolerance));
    EXPECT_FALSE(ui->releaseCorrespondence()) << "a press that moves nothing is no undo step";
    ASSERT_TRUE(ui->hasSelectedCorrespondence());
    const U64 hashThree = ctx->getContentHash();
    ASSERT_TRUE(ui->deleteSelectedCorrespondence(&command));
    EXPECT_FALSE(ui->hasSelectedCorrespondence());
    ASSERT_EQ(1, link->getCorrespondenceCount());
    correspondenceAt(link, 0, kTime, &s, &d);
    EXPECT_NEAR(3.2, d, 0.05);
    command->undo();
    EXPECT_EQ(2, link->getCorrespondenceCount());
    EXPECT_EQ(hashThree, ctx->getContentHash());
    command->redo();
    EXPECT_EQ(1, link->getCorrespondenceCount());
}

TEST_F(SplineWarpToolsTest, CorrespondencesFollowAutoKeying)
{
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));
    SplineWarpInteractPtr ui = interactOf(node);
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(ui && ctx);

    BezierPtr from = makeQuad(ctx, 10., 10., 20.);
    BezierPtr to = makeQuad(ctx, 10., 10., 20., 12., 6.);
    ASSERT_TRUE(from && to);
    SplineWarpLinkPtr link = ctx->linkWithAlignment(from, to, 0., false);
    ASSERT_TRUE(bool(link));
    const double tolerance = 1.;

    // Auto-key off: static values, moved without keys.
    ctx->onAutoKeyingChanged(false);
    ASSERT_TRUE(ui->pressCorrespondence(pointOf(from, 0.5), kTime, tolerance));
    ASSERT_TRUE(ui->dragCorrespondence(pointOf(from, 0.7), kTime));
    ASSERT_TRUE(ui->releaseCorrespondence());
    std::vector<SplineWarpCorrespondence> values = link->getCorrespondences();
    ASSERT_EQ(std::size_t(1), values.size());
    EXPECT_FALSE(values[0].source.isAnimated());
    EXPECT_FALSE(values[0].destination.isAnimated());
    EXPECT_NEAR(0.7, values[0].source.getStaticValue(), 1e-3);

    // Auto-key on: a new one is keyed where it is made, and a drag keys its time.
    ctx->onAutoKeyingChanged(true);
    ASSERT_TRUE(ui->pressCorrespondence(pointOf(from, 2.5), kTime, tolerance));
    ASSERT_TRUE(ui->releaseCorrespondence());
    values = link->getCorrespondences();
    ASSERT_EQ(std::size_t(2), values.size());
    EXPECT_TRUE(values[1].source.isAnimated());
    EXPECT_EQ(std::vector<double>(1, kTime), values[1].source.getKeyFrameTimes());

    const double later = 5.;
    ASSERT_TRUE(ui->pressCorrespondence(pointOf(to, 0.5, later), later, tolerance));
    ASSERT_TRUE(ui->dragCorrespondence(pointOf(to, 1.2, later), later));
    ASSERT_TRUE(ui->releaseCorrespondence());
    values = link->getCorrespondences();
    EXPECT_TRUE(values[0].destination.isAnimated());
    EXPECT_EQ(std::vector<double>(1, later), values[0].destination.getKeyFrameTimes());
    EXPECT_NEAR(1.2, values[0].destination.getValueAt(later), 1e-3);

    // Auto-key off again: an animated value is keyed at the drag's time, as a knob is.
    ctx->onAutoKeyingChanged(false);
    const double third = 9.;
    ASSERT_TRUE(ui->pressCorrespondence(pointOf(to, 1.2, third), third, tolerance));
    ASSERT_TRUE(ui->dragCorrespondence(pointOf(to, 1.4, third), third));
    ASSERT_TRUE(ui->releaseCorrespondence());
    values = link->getCorrespondences();
    std::vector<double> expected;
    expected.push_back(later);
    expected.push_back(third);
    EXPECT_EQ(expected, values[0].destination.getKeyFrameTimes());
    EXPECT_NEAR(1.4, values[0].destination.getValueAt(third), 1e-3);
}
