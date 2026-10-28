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
#include <set>
#include <string>

#include <gtest/gtest.h>

#include <QPointF>
#include <QString>

#include "BaseTest.h"
#include "Engine/Nodes/Test/OverlayTestNode.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/OverlaySupport.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// One screen pixel is one canonical unit, and nothing is drawn.
class FakeViewport
    : public OverlaySupport {
public:
    int redraws = 0;
    int listDraws = 0;

    virtual void swapOpenGLBuffers() OVERRIDE FINAL { }

    virtual void redraw() OVERRIDE FINAL { ++redraws; }

    virtual void getViewportSize(double& width, double& height) const OVERRIDE FINAL
    {
        width = 1000.;
        height = 1000.;
    }

    virtual void getPixelScale(double& xScale, double& yScale) const OVERRIDE FINAL
    {
        xScale = 1.;
        yScale = 1.;
    }

#ifdef OFX_EXTENSIONS_NATRON
    virtual double getScreenPixelRatio() const OVERRIDE FINAL { return 1.; }
#endif

    virtual void getBackgroundColour(double& r, double& g, double& b) const OVERRIDE FINAL
    {
        r = g = b = 0.;
    }

    virtual void saveOpenGLContext() OVERRIDE FINAL { }

    virtual void restoreOpenGLContext() OVERRIDE FINAL { }

    virtual RectD getViewportRect() const OVERRIDE FINAL { return RectD(0., 0., 1000., 1000.); }

    virtual void getCursorPosition(double& x, double& y) const OVERRIDE FINAL
    {
        x = y = 0.;
    }

    virtual void toWidgetCoordinates(double* /*x*/, double* /*y*/) const OVERRIDE FINAL { }

    virtual void toCanonicalCoordinates(double* /*x*/, double* /*y*/) const OVERRIDE FINAL { }

    virtual void drawOverlayDrawList(const OverlayDrawList& /*list*/, const OverlayDrawState& /*state*/) OVERRIDE FINAL { ++listDraws; }

    virtual int getWidgetFontHeight() const OVERRIDE FINAL { return 10; }

    virtual int getStringWidthForCurrentFont(const std::string& string) const OVERRIDE FINAL { return (int)string.size() * 6; }
};

class OverlayTestNodeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    bool penDown(const EffectInstancePtr& effect, double x, double y)
    {
        return effect->onOverlayPenDown_public(1., RenderScale::identity, ViewIdx(0), QPointF(x, y), QPointF(x, y), 1., 0., ePenTypeLMB);
    }

    bool penMotion(const EffectInstancePtr& effect, double x, double y)
    {
        return effect->onOverlayPenMotion_public(1., RenderScale::identity, ViewIdx(0), QPointF(x, y), QPointF(x, y), 1., 0.);
    }

    bool penUp(const EffectInstancePtr& effect, double x, double y)
    {
        return effect->onOverlayPenUp_public(1., RenderScale::identity, ViewIdx(0), QPointF(x, y), QPointF(x, y), 1., 0.);
    }
};

} // namespace

TEST_F(OverlayTestNodeTest, ListHoldsEveryPrimitiveType)
{
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDOverlay));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_TRUE(bool(effect));
    EXPECT_TRUE(node->hasOverlay());

    const OverlayDrawList& list = effect->buildOverlay_public(1., RenderScale::identity, ViewIdx(0));
    std::set<int> types;
    for (const OverlayDrawList::Primitive& p : list.primitives()) {
        types.insert((int)p.type);
    }
    EXPECT_EQ(7u, types.size());

    double x = 0., y = 0.;
    ASSERT_TRUE(list.getHandlePosition(OverlayTestNode::eHandleCenter, &x, &y));
    EXPECT_DOUBLE_EQ(100., x);
    EXPECT_DOUBLE_EQ(100., y);
}

TEST_F(OverlayTestNodeTest, DrawingGoesThroughTheViewport)
{
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDOverlay));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    FakeViewport viewport;
    effect->setCurrentViewportForOverlays_public(&viewport);
    effect->drawOverlay_public(1., RenderScale::identity, ViewIdx(0));
    EXPECT_EQ(1, viewport.listDraws);
    EXPECT_FALSE(effect->getOverlayDrawList().empty());
    effect->setCurrentViewportForOverlays_public(NULL);
}

TEST_F(OverlayTestNodeTest, DraggingTheHandleMovesTheKnob)
{
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDOverlay));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_TRUE(bool(effect));
    std::shared_ptr<OverlayTestNode> overlayNode = std::dynamic_pointer_cast<OverlayTestNode>(effect);
    ASSERT_TRUE(bool(overlayNode));
    KnobDoublePtr center = overlayNode->getCenterKnob();
    ASSERT_TRUE(bool(center));

    FakeViewport viewport;
    effect->setCurrentViewportForOverlays_public(&viewport);
    effect->buildOverlay_public(1., RenderScale::identity, ViewIdx(0));

    EXPECT_FALSE(penDown(effect, 300., 300.));

    EXPECT_TRUE(penMotion(effect, 102., 99.));
    EXPECT_EQ(OverlayTestNode::eHandleCenter, effect->getOverlayDrawState().hoveredHandle);
    EXPECT_FALSE(penMotion(effect, 102., 98.));

    ASSERT_TRUE(penDown(effect, 102., 98.));
    EXPECT_EQ(OverlayTestNode::eHandleCenter, effect->getOverlayDrawState().pressedHandle);
    EXPECT_TRUE(penMotion(effect, 112., 93.));
    EXPECT_TRUE(penMotion(effect, 122., 88.));
    EXPECT_TRUE(penUp(effect, 122., 88.));
    EXPECT_EQ(OverlayDrawList::kNoHandle, effect->getOverlayDrawState().pressedHandle);

    EXPECT_DOUBLE_EQ(120., center->getValueAtTime(1., 0));
    EXPECT_DOUBLE_EQ(90., center->getValueAtTime(1., 1));

    effect->buildOverlay_public(1., RenderScale::identity, ViewIdx(0));
    double x = 0., y = 0.;
    ASSERT_TRUE(effect->getOverlayDrawList().getHandlePosition(OverlayTestNode::eHandleCenter, &x, &y));
    EXPECT_DOUBLE_EQ(120., x);
    EXPECT_DOUBLE_EQ(90., y);

    // The marker handle declines the press, so the pen falls through.
    EXPECT_FALSE(penDown(effect, 180., 180.));
    EXPECT_EQ(OverlayDrawList::kNoHandle, effect->getOverlayDrawState().pressedHandle);

    effect->setCurrentViewportForOverlays_public(NULL);
}
