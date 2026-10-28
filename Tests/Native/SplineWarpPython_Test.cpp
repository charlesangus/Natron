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

#include <list>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoItem.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The same quadrilateral the Python snippet draws.
BezierPtr
makeQuad(const SplineWarpContextPtr& ctx,
         double x,
         double y,
         double size,
         double dx = 0.,
         double dy = 0.)
{
    const double points[4][2] = { { x, y }, { x + size, y }, { x + size * 1.2, y + size }, { x - size * 0.1, y + size * 0.8 } };
    BezierPtr bezier = ctx->makeBezier(points[0][0] + dx, points[0][1] + dy, kRotoBezierBaseName, kTime, false);

    if (!bezier) {
        return bezier;
    }
    for (int i = 1; i < 4; ++i) {
        bezier->addControlPoint(points[i][0] + dx, points[i][1] + dy, kTime);
    }
    bezier->setCurveFinished(true);

    return bezier;
}

BezierPtr
makeRectangle(const SplineWarpContextPtr& ctx,
              double x1,
              double y1,
              double x2,
              double y2)
{
    BezierPtr bezier = ctx->makeBezier(x1, y1, kRotoBezierBaseName, kTime, false);

    if (!bezier) {
        return bezier;
    }
    bezier->addControlPoint(x2, y1, kTime);
    bezier->addControlPoint(x2, y2, kTime);
    bezier->addControlPoint(x1, y2, kTime);
    bezier->setCurveFinished(true);

    return bezier;
}

const char* const kPythonHelpers = "def quad(roto, x, y, size, dx=0., dy=0.):\n"
                                   "    pts = [(x, y), (x + size, y), (x + size * 1.2, y + size), (x - size * 0.1, y + size * 0.8)]\n"
                                   "    c = roto.createBezier(pts[0][0] + dx, pts[0][1] + dy, 1.)\n"
                                   "    for px, py in pts[1:]:\n"
                                   "        c.addControlPoint(px + dx, py + dy)\n"
                                   "    c.setCurveFinished(True)\n"
                                   "    return c\n"
                                   "def rectangle(roto, x1, y1, x2, y2):\n"
                                   "    c = roto.createBezier(x1, y1, 1.)\n"
                                   "    c.addControlPoint(x2, y1)\n"
                                   "    c.addControlPoint(x2, y2)\n"
                                   "    c.addControlPoint(x1, y2)\n"
                                   "    c.setCurveFinished(True)\n"
                                   "    return c\n"
                                   "def raises(f):\n"
                                   "    try:\n"
                                   "        f()\n"
                                   "    except ValueError:\n"
                                   "        return True\n"
                                   "    return False\n";

} // namespace

class SplineWarpPythonTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "splineWarpPython64x48", 1.));
    }

    NodePtr makeSplineWarp(const NodePtr& input)
    {
        CreateNodeArgs args(PLUGINID_NATRON_SPLINEWARP, getApp()->getProject());

        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, PLUGIN_MAJOR_NATRON_SPLINEWARP, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);
        args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        NodePtr node = getApp()->createNode(args);

        EXPECT_TRUE(bool(node));
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    std::string pythonName(const NodePtr& node)
    {
        return getApp()->getAppIDString() + "." + node->getFullyQualifiedName();
    }

    void expectScript(const std::string& body)
    {
        std::string error, output;

        EXPECT_TRUE(interpretPythonScript(body, &error, &output)) << error << "\n"
                                                                  << body;
    }
};

// Two links (one with a correspondence and half its amount) inside a boundary, built from Python,
// render as the same model built in C++.
TEST_F(SplineWarpPythonTest, PythonBuildMatchesCpp)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr fromPython = makeSplineWarp(source);
    NodePtr fromCpp = makeSplineWarp(source);
    ASSERT_TRUE(fromPython && fromCpp);

    expectScript(std::string(kPythonHelpers) + "n = " + pythonName(fromPython) + "\n"
                                                                                 "roto = n.getRotoContext()\n"
                                                                                 "sw = n.getSplineWarp()\n"
                                                                                 "assert sw is not None\n"
                                                                                 "a = quad(roto, 20., 15., 10.)\n"
                                                                                 "b = quad(roto, 20., 15., 10., 3., -2.)\n"
                                                                                 "l1 = sw.link(a, b)\n"
                                                                                 "c = quad(roto, 36., 20., 8.)\n"
                                                                                 "d = quad(roto, 36., 20., 8., -2., 1.)\n"
                                                                                 "l2 = sw.link(c, d)\n"
                                                                                 "assert l2.addCorrespondence(0.5, 1.) == 0\n"
                                                                                 "l2.setAmount(0.5, 1.)\n"
                                                                                 "bound = rectangle(roto, 8., 6., 56., 42.)\n"
                                                                                 "sw.setBoundary(bound, True)\n"
                                                                                 "assert sw.isBoundary(bound)\n"
                                                                                 "assert not sw.isBoundary(a)\n"
                                                                                 "assert len(sw.getLinks()) == 2\n"
                                                                                 "assert sw.getLink(c).getCorrespondences(1.) == [(0.5, 1.)]\n"
                                                                                 "assert l2.getCorrespondenceCount() == 1\n"
                                                                                 "assert abs(l2.getAmount(1.) - 0.5) < 1e-12\n"
                                                                                 "assert l1.getSource().getScriptName() == a.getScriptName()\n"
                                                                                 "assert l1.getDestination().getScriptName() == b.getScriptName()\n");

    SplineWarpContextPtr ctx = std::dynamic_pointer_cast<SplineWarpContext>(fromCpp->getCurveContext());
    ASSERT_TRUE(bool(ctx));
    SplineWarpLinkPtr l1 = ctx->link(makeQuad(ctx, 20., 15., 10.), makeQuad(ctx, 20., 15., 10., 3., -2.));
    SplineWarpLinkPtr l2 = ctx->link(makeQuad(ctx, 36., 20., 8.), makeQuad(ctx, 36., 20., 8., -2., 1.));
    ASSERT_TRUE(l1 && l2);
    EXPECT_EQ(0, ctx->addCorrespondence(l2, 0.5, 1.));
    ctx->setLinkAmount(l2, 0.5, kTime);
    BezierPtr boundary = makeRectangle(ctx, 8., 6., 56., 42.);
    ASSERT_TRUE(bool(boundary));
    ctx->setBoundary(boundary, true);

    SplineWarpContextPtr pyCtx = std::dynamic_pointer_cast<SplineWarpContext>(fromPython->getCurveContext());
    ASSERT_TRUE(bool(pyCtx));
    ASSERT_EQ(2u, pyCtx->getLinks().size());
    EXPECT_EQ(1u, pyCtx->getBoundaries().size());

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> pyPlanes, cppPlanes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(fromPython, kTime, ViewIdx(0), 0, window, layers, &pyPlanes, &error)) << error;
    ASSERT_TRUE(renderNodePlanesDirect(fromCpp, kTime, ViewIdx(0), 0, window, layers, &cppPlanes, &error)) << error;
    ASSERT_EQ(1u, pyPlanes.size());
    ASSERT_EQ(1u, cppPlanes.size());
    ASSERT_EQ(cppPlanes[0].pixels.size(), pyPlanes[0].pixels.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < cppPlanes[0].pixels.size(); ++i) {
        if (cppPlanes[0].pixels[i] != pyPlanes[0].pixels[i]) {
            ++differing;
        }
    }
    EXPECT_EQ(0u, differing);

    // The warp moves something, so the comparison is not between two copies of the Source.
    std::vector<RenderedPlane> sourcePlanes;
    ASSERT_TRUE(renderNodePlanesDirect(source, kTime, ViewIdx(0), 0, window, layers, &sourcePlanes, &error)) << error;
    ASSERT_EQ(1u, sourcePlanes.size());
    EXPECT_NE(sourcePlanes[0].pixels, cppPlanes[0].pixels);
}

TEST_F(SplineWarpPythonTest, ErrorsRaiseValueError)
{
    resetProject();
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));

    expectScript(std::string(kPythonHelpers) + "n = " + pythonName(node) + "\n"
                                                                           "roto = n.getRotoContext()\n"
                                                                           "sw = n.getSplineWarp()\n"
                                                                           "a = quad(roto, 20., 15., 10.)\n"
                                                                           "b = quad(roto, 20., 15., 10., 3., -2.)\n"
                                                                           "c = quad(roto, 4., 30., 6.)\n"
                                                                           "l = sw.link(a, b)\n"
                                                                           "opened = roto.createBezier(2., 2., 1.)\n"
                                                                           "opened.addControlPoint(6., 2.)\n"
                                                                           "opened.addControlPoint(6., 6.)\n"
                                                                           "assert raises(lambda: sw.setBoundary(opened, True)), 'open boundary'\n"
                                                                           "assert not sw.isBoundary(opened)\n"
                                                                           "assert raises(lambda: sw.link(a, c)), 'second link of a curve'\n"
                                                                           "assert raises(lambda: sw.link(c, c)), 'curve linked to itself'\n"
                                                                           "assert raises(lambda: sw.setBoundary(a, True)), 'linked curve as boundary'\n"
                                                                           "assert raises(lambda: l.addCorrespondence(9., 0.)), 'parameter out of range'\n"
                                                                           "assert raises(lambda: l.setCorrespondence(3, 0., 0., 1.)), 'index out of range'\n"
                                                                           "assert raises(lambda: l.removeCorrespondence(0)), 'no correspondence to remove'\n"
                                                                           "assert len(sw.getLinks()) == 1\n"
                                                                           "l.remove()\n"
                                                                           "assert len(sw.getLinks()) == 0\n"
                                                                           "assert sw.getLink(a) is None\n"
                                                                           "assert raises(lambda: l.setAmount(0.5, 1.)), 'edit of a removed link'\n"
                                                                           "l2 = sw.link(a, c)\n"
                                                                           "sw.unlink(c)\n"
                                                                           "assert len(sw.getLinks()) == 0\n");
}

// Soft boundaries and layer amounts, set from Python, reach the model.
TEST_F(SplineWarpPythonTest, SoftBoundariesAndLayerAmounts)
{
    resetProject();
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));

    expectScript(std::string(kPythonHelpers) + "n = " + pythonName(node) + "\n"
                                                                           "roto = n.getRotoContext()\n"
                                                                           "sw = n.getSplineWarp()\n"
                                                                           "a = quad(roto, 20., 15., 10.)\n"
                                                                           "b = quad(roto, 20., 15., 10., 3., -2.)\n"
                                                                           "l = sw.link(a, b)\n"
                                                                           "line = roto.createBezier(2., 2., 1.)\n"
                                                                           "line.addControlPoint(40., 2.)\n"
                                                                           "sw.setSoftBoundary(line, True)\n"
                                                                           "assert sw.isSoftBoundary(line)\n"
                                                                           "assert not sw.isBoundary(line)\n"
                                                                           "assert raises(lambda: sw.setSoftBoundary(a, True)), 'linked curve as soft boundary'\n"
                                                                           "assert raises(lambda: sw.link(line, quad(roto, 4., 30., 6.))), 'soft boundary linked'\n"
                                                                           "layer = roto.createLayer()\n"
                                                                           "layer.addItem(b)\n"
                                                                           "assert abs(sw.getLayerAmount(layer, 1.) - 1.) < 1e-12\n"
                                                                           "sw.setLayerAmount(layer, 0.5, 1.)\n"
                                                                           "assert abs(sw.getLayerAmount(layer, 1.) - 0.5) < 1e-12\n"
                                                                           "sw.setLayerAmountAtTime(layer, 10., 0.25)\n"
                                                                           "assert abs(sw.getLayerAmount(layer, 10.) - 0.25) < 1e-12\n"
                                                                           "assert raises(lambda: sw.setLayerAmount(None, 0.5, 1.)), 'no layer'\n");

    SplineWarpContextPtr ctx = std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext());
    ASSERT_TRUE(bool(ctx));
    EXPECT_EQ(1u, ctx->getSoftBoundaries().size());
    const std::vector<SplineWarpLinkPtr> links = ctx->getLinks();
    ASSERT_EQ(1u, links.size());
    ASSERT_TRUE(bool(links[0]->getDestination()));
    EXPECT_DOUBLE_EQ(0.25, ctx->getInheritedLayerAmount(links[0]->getDestination(), 10.));
    EXPECT_DOUBLE_EQ(1., ctx->getInheritedLayerAmount(links[0]->getSource(), 10.));
}

// Transform links made from Python reach the model; a cycle raises ValueError.
TEST_F(SplineWarpPythonTest, TransformLinks)
{
    resetProject();
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));

    expectScript(std::string(kPythonHelpers) + "n = " + pythonName(node) + "\n"
                                                                           "roto = n.getRotoContext()\n"
                                                                           "sw = n.getSplineWarp()\n"
                                                                           "a = quad(roto, 20., 15., 10.)\n"
                                                                           "b = quad(roto, 30., 5., 8.)\n"
                                                                           "layer = roto.createLayer()\n"
                                                                           "assert sw.getTransformLink(a) is None\n"
                                                                           "sw.linkTransform(a, b)\n"
                                                                           "sw.linkTransform(layer, b)\n"
                                                                           "assert sw.getTransformLink(a).getScriptName() == b.getScriptName()\n"
                                                                           "assert raises(lambda: sw.linkTransform(b, a)), 'cycle'\n"
                                                                           "assert raises(lambda: sw.linkTransform(a, a)), 'self'\n"
                                                                           "b.getParam('translate').set(2., 5.)\n"
                                                                           "sw.unlinkTransform(layer)\n"
                                                                           "assert sw.getTransformLink(layer) is None\n");

    SplineWarpContextPtr ctx = std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext());
    ASSERT_TRUE(bool(ctx));
    const std::list<RotoDrawableItemPtr> curves = ctx->getCurvesByRenderOrder(false);
    int linked = 0;
    for (std::list<RotoDrawableItemPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        RotoItemPtr target = ctx->getTransformLink(*it);
        if (target) {
            ++linked;
            KnobDoublePtr translate = std::dynamic_pointer_cast<KnobDouble>(std::dynamic_pointer_cast<RotoDrawableItem>(target)->getKnobByName("translate"));
            ASSERT_TRUE(bool(translate));
            EXPECT_DOUBLE_EQ(2., translate->getValue(0));
            EXPECT_DOUBLE_EQ(5., translate->getValue(1));
        }
    }
    EXPECT_EQ(1, linked);
}

// Cookie cutters, set from Python, reach the model; an open curve is refused.
TEST_F(SplineWarpPythonTest, CookieCutters)
{
    resetProject();
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));

    expectScript(std::string(kPythonHelpers) + "n = " + pythonName(node) + "\n"
                                                                           "roto = n.getRotoContext()\n"
                                                                           "sw = n.getSplineWarp()\n"
                                                                           "a = quad(roto, 20., 15., 10.)\n"
                                                                           "b = quad(roto, 20., 15., 10., 3., -2.)\n"
                                                                           "l = sw.link(a, b)\n"
                                                                           "sw.setCookieCutter(a, True)\n"
                                                                           "assert sw.isCookieCutter(a)\n"
                                                                           "assert not sw.isCookieCutter(b)\n"
                                                                           "line = roto.createBezier(2., 2., 1.)\n"
                                                                           "line.addControlPoint(40., 2.)\n"
                                                                           "assert raises(lambda: sw.setCookieCutter(line, True)), 'open cookie cutter'\n"
                                                                           "assert not sw.isCookieCutter(line)\n");

    SplineWarpContextPtr ctx = std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext());
    ASSERT_TRUE(bool(ctx));
    EXPECT_EQ(1u, ctx->getCookieCutters().size());
    EXPECT_EQ(1u, ctx->getLinks().size());
}

TEST_F(SplineWarpPythonTest, OtherNodesHaveNoSplineWarp)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    expectScript("assert " + pythonName(source) + ".getSplineWarp() is None\n");
}

// Layer.removeItem goes through the context, so the warp model drops the curve's link.
TEST_F(SplineWarpPythonTest, RemovingALinkedCurveRemovesTheLink)
{
    resetProject();
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));

    expectScript(std::string(kPythonHelpers) + "n = " + pythonName(node) + "\n"
                                                                           "roto = n.getRotoContext()\n"
                                                                           "sw = n.getSplineWarp()\n"
                                                                           "a = quad(roto, 20., 15., 10.)\n"
                                                                           "b = quad(roto, 20., 15., 10., 3., -2.)\n"
                                                                           "sw.link(a, b)\n"
                                                                           "assert len(sw.getLinks()) == 1\n"
                                                                           "roto.getBaseLayer().removeItem(b)\n"
                                                                           "assert len(sw.getLinks()) == 0\n");
}
