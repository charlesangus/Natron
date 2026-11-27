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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/OverlayDrawListPen.h"
#include "Engine/OverlaySupport.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// One screen pixel is one canonical unit. Draws are recorded, not rendered.
class RecordingViewport
    : public OverlaySupport {
public:
    std::vector<const OverlayDrawList*> drawn;

    virtual void swapOpenGLBuffers() OVERRIDE FINAL { }

    virtual void redraw() OVERRIDE FINAL { }

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

    virtual void drawOverlayDrawList(const OverlayDrawList& list, const OverlayDrawState& /*state*/) OVERRIDE FINAL { drawn.push_back(&list); }

    virtual int getWidgetFontHeight() const OVERRIDE FINAL { return 10; }

    virtual int getStringWidthForCurrentFont(const std::string& string) const OVERRIDE FINAL { return (int)string.size() * 6; }
};

} // namespace

// Three shapes in render order with the middle one selected: the curves draw as three lists, and
// an edit or a hover on one shape rebuilds only the list holding it.
class RotoCurvesOverlayCacheTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);

        _node = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        ASSERT_NE(_node, nullptr);
        _effect = _node->getEffectInstance();
        RotoPaint* roto = dynamic_cast<RotoPaint*>(_effect.get());
        ASSERT_NE(roto, nullptr);
        _ui = roto->getInteract();
        ASSERT_NE(_ui, nullptr);
        RotoContextPtr context = _node->getRotoContext();
        ASSERT_NE(context, nullptr);
        _time = context->getTimelineCurrentTime();
        for (int i = 0; i < 3; ++i) {
            BezierPtr shape = context->makeSquare(10. + 100. * i, 20., 40., _time);
            ASSERT_NE(shape, nullptr);
            _shapes.push_back(shape);
        }
        _ui->selectedItems.clear();
        _ui->selectedItems.push_back(_shapes[1]);
        _effect->setCurrentViewportForOverlays_public(&_viewport);
    }

    void TearDown() OVERRIDE
    {
        if (_effect) {
            _effect->setCurrentViewportForOverlays_public(NULL);
        }
        _ui.reset();
        _shapes.clear();
        _effect.reset();
        _node.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // The lists rebuilt by one redraw of the curves.
    unsigned long long redraw()
    {
        const unsigned long long before = _ui->curvesOverlayRebuildCount();

        _viewport.drawn.clear();
        _ui->drawCurvesOverlay(_time);

        return _ui->curvesOverlayRebuildCount() - before;
    }

    NodePtr _node;
    EffectInstancePtr _effect;
    RotoPaintInteractPtr _ui;
    std::vector<BezierPtr> _shapes;
    RecordingViewport _viewport;
    double _time = 0.;
};

TEST_F(RotoCurvesOverlayCacheTest, OneListPerSelectedShapeAndPerRunBetween)
{
    EXPECT_EQ(3u, redraw());
    ASSERT_EQ(3u, _viewport.drawn.size());
    for (std::size_t i = 0; i < _viewport.drawn.size(); ++i) {
        EXPECT_FALSE(_viewport.drawn[i]->empty()) << i;
    }

    _ui->selectedItems.clear();
    EXPECT_EQ(1u, redraw());
    EXPECT_EQ(1u, _viewport.drawn.size());
}

TEST_F(RotoCurvesOverlayCacheTest, UnchangedRedrawRebuildsNothing)
{
    redraw();
    const std::vector<const OverlayDrawList*> first = _viewport.drawn;
    std::vector<unsigned long long> ids;
    for (std::size_t i = 0; i < first.size(); ++i) {
        ids.push_back(first[i]->buildId());
    }

    EXPECT_EQ(0u, redraw());
    ASSERT_EQ(first, _viewport.drawn);
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(ids[i], first[i]->buildId()) << i;
    }
}

TEST_F(RotoCurvesOverlayCacheTest, MovingTheSelectedShapeRebuildsOnlyItsList)
{
    redraw();
    const std::vector<const OverlayDrawList*> lists = _viewport.drawn;
    ASSERT_EQ(3u, lists.size());
    const unsigned long long before[3] = { lists[0]->buildId(), lists[1]->buildId(), lists[2]->buildId() };

    for (int step = 1; step <= 3; ++step) {
        _shapes[1]->movePointByIndex(0, _time, 2., 1.);
        EXPECT_EQ(1u, redraw()) << step;
        ASSERT_EQ(lists, _viewport.drawn) << step;
    }
    EXPECT_EQ(before[0], lists[0]->buildId());
    EXPECT_NE(before[1], lists[1]->buildId());
    EXPECT_EQ(before[2], lists[2]->buildId());
}

TEST_F(RotoCurvesOverlayCacheTest, HoveringAFeatherBarRebuildsOnlyTheSelectedShape)
{
    redraw();
    _ui->featherBarBeingHovered = std::make_pair(_shapes[1]->getControlPointAtIndex(0), _shapes[1]->getFeatherPointAtIndex(0));
    EXPECT_EQ(1u, redraw());
    _ui->featherBarBeingHovered = std::make_pair(BezierCPPtr(), BezierCPPtr());
    EXPECT_EQ(1u, redraw());
}

TEST_F(RotoCurvesOverlayCacheTest, MovingAnUnselectedShapeRebuildsOnlyItsRun)
{
    redraw();
    _shapes[2]->movePointByIndex(0, _time, 2., 1.);
    EXPECT_EQ(1u, redraw());
}

TEST(OverlayDrawListSequence, ListsKeepTheirAddressAndOnlyChangedOnesRebuild)
{
    OverlayDrawListSequence sequence;

    sequence.restart();
    for (U64 signature = 1; signature <= 3; ++signature) {
        OverlayDrawList* list = sequence.next().rebuildFor(signature);
        ASSERT_NE(list, nullptr);
        list->addPoint(0., 0.);
    }
    sequence.trim();
    ASSERT_EQ(3u, sequence.size());
    const OverlayDrawList* addresses[3] = { &sequence.list(0), &sequence.list(1), &sequence.list(2) };
    const unsigned long long ids[3] = { sequence.list(0).buildId(), sequence.list(1).buildId(), sequence.list(2).buildId() };

    sequence.restart();
    EXPECT_TRUE(sequence.next().rebuildFor(1) == nullptr);
    EXPECT_TRUE(sequence.next().rebuildFor(20) != nullptr);
    EXPECT_TRUE(sequence.next().rebuildFor(3) == nullptr);
    sequence.trim();
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(addresses[i], &sequence.list(i)) << i;
    }
    EXPECT_EQ(ids[0], sequence.list(0).buildId());
    EXPECT_NE(ids[1], sequence.list(1).buildId());
    EXPECT_EQ(ids[2], sequence.list(2).buildId());

    sequence.restart();
    EXPECT_TRUE(sequence.next().rebuildFor(1) == nullptr);
    sequence.trim();
    EXPECT_EQ(1u, sequence.size());
    EXPECT_EQ(addresses[0], &sequence.list(0));
}
