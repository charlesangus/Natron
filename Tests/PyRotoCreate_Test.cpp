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

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/PyRoto.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// An eight-point shape named "animated", keyed at frames 1 and 11.
const char* const kAnimatedShape = "c = r.createBezier(60., 40., 1.)\n"
                                   "for (x, y) in [(54., 54.), (40., 60.), (26., 54.), (20., 40.), (26., 26.), (40., 20.), (54., 26.)]:\n"
                                   "    c.addControlPoint(x, y)\n"
                                   "c.setCurveFinished(True)\n"
                                   "assert c.setScriptName(\"animated\")\n"
                                   "for i in range(8):\n"
                                   "    p = c.getControlPointPosition(i, 1.)\n"
                                   "    c.setPointAtIndex(i, 11., p[0] + 5., p[1] + 3., p[2] + 5., p[3] + 3., p[4] + 5., p[5] + 3.)\n"
                                   "keys = c.getKeyframes()\n"
                                   "assert 1. in keys and 11. in keys, keys\n";

} // namespace

class PyRotoCreateTest
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

    std::string pythonName(const NodePtr& node)
    {
        return getApp()->getAppIDString() + "." + node->getFullyQualifiedName();
    }
};

// A script that creates a finished shape must see it rendered, as after setCurveFinished(true).
TEST_F(PyRotoCreateTest, FinishedShapesAreEvaluated)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(node));
    RotoCurveContextPtr ctx = node->getCurveContext();
    ASSERT_TRUE(bool(ctx));
    NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::Roto roto(ctx);

    U64 age = ctx->getAge();
    std::unique_ptr<NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::BezierCurve> square(roto.createRectangle(760., 340., 400., 1.));
    ASSERT_TRUE(square != nullptr);
    EXPECT_GT(ctx->getAge(), age);

    age = ctx->getAge();
    std::unique_ptr<NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::BezierCurve> ellipse(roto.createEllipse(960., 540., 200., true, 1.));
    ASSERT_TRUE(ellipse != nullptr);
    EXPECT_GT(ctx->getAge(), age);
}

// A script's setPointAtIndex at an unkeyed frame, run while the node renders, keeps the values it
// set once the frame is auto-keyed.
TEST_F(PyRotoCreateTest, SetPointAtIndexDuringARenderKeepsTheAutoKey)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(node));
    RotoCurveContextPtr ctx = node->getCurveContext();
    ASSERT_TRUE(bool(ctx));
    ctx->setAutoKeyingEnabled(true);
    ctx->setRippleEditEnabled(false);
    const std::string context = "r = " + pythonName(node) + ".getRotoContext()\n";

    std::string error, output;
    const std::string build = context + kAnimatedShape;
    ASSERT_TRUE(NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::interpretPythonScript(build, &error, &output)) << error << "\n"
                                                                                                          << build;

    const std::string edit = context + "c = r.getItemByName(\"animated\")\n"
                                       "c.setPointAtIndex(2, 6., 41., 63., 40., 61., 42., 65.)\n"
                                       "p = c.getControlPointPosition(2, 6.)\n"
                                       "assert tuple(p) == (41., 63., 40., 61., 42., 65.), p\n"
                                       "assert 6. in c.getKeyframes(), c.getKeyframes()\n";
    {
        RenderingFlagSetter rendering(node);
        ASSERT_TRUE(node->isNodeRendering());
        EXPECT_TRUE(NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::interpretPythonScript(edit, &error, &output)) << error << "\n"
                                                                                                             << edit;
    }
    BezierPtr shape = std::dynamic_pointer_cast<Bezier>(ctx->getItemByName("animated"));
    ASSERT_TRUE(bool(shape));
    shape->dequeueGuiActions();

    const BezierCPPtr point = shape->getControlPointAtIndex(2);
    ASSERT_TRUE(bool(point));
    for (int gui = 0; gui < 2; ++gui) {
        double x = 0., y = 0., lx = 0., ly = 0., rx = 0., ry = 0.;
        point->getPositionAtTime(gui != 0, 6., ViewIdx(0), &x, &y);
        point->getLeftBezierPointAtTime(gui != 0, 6., ViewIdx(0), &lx, &ly);
        point->getRightBezierPointAtTime(gui != 0, 6., ViewIdx(0), &rx, &ry);
        EXPECT_EQ(41., x) << "gui " << gui;
        EXPECT_EQ(63., y) << "gui " << gui;
        EXPECT_EQ(40., lx) << "gui " << gui;
        EXPECT_EQ(61., ly) << "gui " << gui;
        EXPECT_EQ(42., rx) << "gui " << gui;
        EXPECT_EQ(65., ry) << "gui " << gui;
    }
}

// Baking a track link from a script while the node renders keeps a point edit the script made
// just before.
TEST_F(PyRotoCreateTest, UnlinkBakeDuringARenderKeepsAPointEdit)
{
    NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
    ASSERT_TRUE(tracker && tracker->getTrackerContext());
    TrackMarkerPtr marker = tracker->getTrackerContext()->createMarker();
    ASSERT_TRUE(bool(marker));
    for (int f = 1; f <= 10; ++f) {
        marker->getCenterKnob()->setValueAtTime(f, 50. + 2. * f, ViewSpec::all(), 0);
        marker->getCenterKnob()->setValueAtTime(f, 30. + f, ViewSpec::all(), 1);
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(node));
    RotoCurveContextPtr ctx = node->getCurveContext();
    ASSERT_TRUE(bool(ctx));
    ctx->setAutoKeyingEnabled(true);
    ctx->setRippleEditEnabled(false);
    const std::string context = "r = " + pythonName(node) + ".getRotoContext()\n"
                                                            "t = "
        + pythonName(tracker) + ".getTrackerContext().getTrackByName(\"" + marker->getScriptName_mt_safe() + "\")\n"
                                                                                                             "assert t is not None\n";

    std::string error, output;
    const std::string build = context + kAnimatedShape + "c.linkPointToTrack(5, t)\n";
    ASSERT_TRUE(NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::interpretPythonScript(build, &error, &output)) << error << "\n"
                                                                                                          << build;

    const std::string edit = context + "c = r.getItemByName(\"animated\")\n"
                                       "c.setPointAtIndex(2, 11., 41., 63., 40., 61., 42., 65.)\n"
                                       "c.unlinkPoint(5, True)\n"
                                       "p = c.getControlPointPosition(2, 11.)\n"
                                       "assert tuple(p) == (41., 63., 40., 61., 42., 65.), p\n";
    {
        RenderingFlagSetter rendering(node);
        ASSERT_TRUE(node->isNodeRendering());
        EXPECT_TRUE(NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::interpretPythonScript(edit, &error, &output)) << error << "\n"
                                                                                                             << edit;
    }
    BezierPtr shape = std::dynamic_pointer_cast<Bezier>(ctx->getItemByName("animated"));
    ASSERT_TRUE(bool(shape));
    shape->dequeueGuiActions();
    EXPECT_FALSE(shape->getControlPointAtIndex(5)->isLinkedToTrack());

    const BezierCPPtr point = shape->getControlPointAtIndex(2);
    ASSERT_TRUE(bool(point));
    for (int gui = 0; gui < 2; ++gui) {
        double x = 0., y = 0.;
        point->getPositionAtTime(gui != 0, 11., ViewIdx(0), &x, &y);
        EXPECT_EQ(41., x) << "gui " << gui;
        EXPECT_EQ(63., y) << "gui " << gui;
    }
}

// A layer's children are wrapped as their own types, so a child layer, a Bezier and a stroke each
// offer their type's methods to the script.
TEST_F(PyRotoCreateTest, LayerChildrenHaveTheirOwnTypes)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(node));
    RotoContextPtr ctx = node->getRotoContext();
    ASSERT_TRUE(bool(ctx));
    ASSERT_FALSE(ctx->getLayers().empty());
    RotoLayerPtr base = ctx->getLayers().front();

    BezierPtr shape = ctx->makeSquare(0., 0., 10., ctx->getTimelineCurrentTime());
    ASSERT_TRUE(bool(shape));
    ASSERT_TRUE(shape->setScriptName("childShape"));
    RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, ctx, "childStroke", RotoLayerPtr()));
    stroke->createNodes();
    ctx->addItem(base, 0, stroke, RotoItem::eSelectionReasonOther);
    for (int i = 0; i < 8; ++i) {
        stroke->appendPoint(i == 0, RotoPoint(4. + 2. * i, 20., 0.5, 1.));
    }
    stroke->setStrokeFinished();
    ctx->clearSelection(RotoItem::eSelectionReasonOther);
    RotoLayerPtr layer = ctx->addLayer();
    ASSERT_TRUE(bool(layer));
    ASSERT_TRUE(layer->setScriptName("childLayer"));
    ctx->clearSelection(RotoItem::eSelectionReasonOther);
    ASSERT_EQ(base, shape->getParentLayer());
    ASSERT_EQ(base, stroke->getParentLayer());
    ASSERT_EQ(base, layer->getParentLayer());

    const std::string script = "import NatronEngine\n"
                               "r = "
        + pythonName(node) + ".getRotoContext()\n"
                             "children = dict((c.getScriptName(), c) for c in r.getBaseLayer().getChildren())\n"
                             "assert sorted(children.keys()) == [\"childLayer\", \"childShape\", \"childStroke\"], children.keys()\n"
                             "l = children[\"childLayer\"]\n"
                             "assert isinstance(l, NatronEngine.Layer), type(l)\n"
                             "assert l.getChildren() == [], l.getChildren()\n"
                             "b = children[\"childShape\"]\n"
                             "assert isinstance(b, NatronEngine.BezierCurve), type(b)\n"
                             "assert b.isCurveFinished()\n"
                             "assert b.getNumControlPoints() == 4, b.getNumControlPoints()\n"
                             "s = children[\"childStroke\"]\n"
                             "assert type(s) is NatronEngine.ItemBase, type(s)\n"
                             "assert s.getParam(\"opacity\") is not None\n";
    std::string error, output;
    EXPECT_TRUE(NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &error, &output)) << error << "\n"
                                                                                                           << script;
}
