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

#include "Engine/Nodes/Scene/SceneRaster.h"

#include "Global/Macros.h"

#include <chrono>
#include <functional>
#include <list>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <QThread>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/TexturePaint.h"
#include "Engine/Nodes/Scene/TexturePaintInteract.h"
#include "Engine/Nodes/Scene/TexturePaintView.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoStrokeItem.h"

NATRON_NAMESPACE_USING

namespace {
const int kViewWidth = 1600;
const int kViewHeight = 900;

// A perspective camera at (0, 0, distance) looking down -Z, with the aspect of the test view.
SceneRaster::ExplicitCamera
cameraAt(double distance)
{
    SceneRaster::ExplicitCamera camera;

    camera.cameraToWorld[14] = distance;
    camera.viewHeight = 0.8;
    camera.aspect = (double)kViewWidth / (double)kViewHeight;
    camera.orthographic = false;
    camera.nearClip = 0.1;
    camera.farClip = 1000.;

    return camera;
}

bool
pumpUntil(const std::function<bool()>& done,
          int timeoutMs)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }

    return true;
}

// Lets the previous stroke's neat render land, so that the next stroke reaches the engine at once.
void
landNeatRender()
{
    for (int i = 0; i < 50; ++i) {
        QCoreApplication::processEvents();
    }
}

std::list<RotoStrokeItemPtr>
strokesIn(const RotoLayerPtr& session)
{
    std::list<RotoStrokeItemPtr> strokes;

    for (const RotoItemPtr& item : session->getItems_mt_safe()) {
        if (RotoStrokeItemPtr stroke = std::dynamic_pointer_cast<RotoStrokeItem>(item)) {
            strokes.push_back(stroke);
        }
    }

    return strokes;
}

// Multi-Stroke, on by default, puts strokes painted with the same brush into one item, so the strokes
// a session holds are counted across its items.
std::size_t
subStrokesIn(const RotoLayerPtr& session)
{
    std::size_t count = 0;

    for (const RotoStrokeItemPtr& stroke : strokesIn(session)) {
        count += stroke->getXControlPoints().size();
    }

    return count;
}
} // anonymous namespace

class TexturePaintInteractTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        _node = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
        _effect = _node ? dynamic_cast<TexturePaint*>(_node->getEffectInstance().get()) : nullptr;
        if (_effect) {
            pickTool(_effect->getPaintInteract()->brushAction.lock());
        }
    }

    virtual void TearDown() OVERRIDE
    {
        if (_effect) {
            (void)pumpUntil([this]() { return !_effect->isBaking(); }, 60000);
        }
        _effect = nullptr;
        _node.reset();
        getApp()->getProject()->reset(false, true);
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    void pickTool(const KnobButtonPtr& tool)
    {
        const RotoPaintInteractPtr paint = _effect->getPaintInteract();

        paint->setCurrentTool(tool);
        paint->onRoleChangedInternal(paint->paintBrushToolGroup.lock());
        paint->onToolChangedInternal(tool);
    }

    double time() const
    {
        return _node->getRotoContext()->getTimelineCurrentTime();
    }

    // A stroke across (u, v) to (u + 0.05, v) through camera.
    void stroke(const SceneRaster::ExplicitCamera& camera,
                double u,
                double v)
    {
        TexturePaintInteract* view = _effect->getViewInteract();

        ASSERT_TRUE(view->penDown(camera, kViewWidth, kViewHeight, u, v, 1., 1., time()));
        EXPECT_TRUE(view->isPenDown());
        EXPECT_TRUE(view->penMotion(u + 0.05, v, 1., 2.));
        EXPECT_TRUE(view->penUp());
        EXPECT_FALSE(view->isPenDown());
        landNeatRender();
    }

    NodePtr _node;
    TexturePaint* _effect = nullptr;
};

TEST_F(TexturePaintInteractTest, StrokesThroughOnePoseShareOneSession)
{
    ASSERT_TRUE(_effect != nullptr);
    TexturePaintInteract* view = _effect->getViewInteract();
    ASSERT_TRUE(view != nullptr);
    EXPECT_FALSE(bool(view->getCurrentSession()));

    const SceneRaster::ExplicitCamera camera = cameraAt(5.);
    stroke(camera, 0.25, 0.5);
    stroke(camera, 0.5, 0.5);
    stroke(camera, 0.75, 0.25);

    const std::list<RotoLayerPtr> sessions = _effect->getSessions();
    ASSERT_EQ(1u, sessions.size());
    EXPECT_EQ(sessions.front(), view->getCurrentSession());
    // A stroke begun before the previous one's neat render lands is painted once it does.
    EXPECT_TRUE(pumpUntil([&]() { return subStrokesIn(sessions.front()) >= 3u; }, 30000));
    EXPECT_EQ(3u, subStrokesIn(sessions.front()));
    SceneRaster::ExplicitCamera stored;
    ASSERT_TRUE(TexturePaint::getSessionCamera(sessions.front(), &stored));
    EXPECT_DOUBLE_EQ(5., stored.cameraToWorld[14]);
    EXPECT_DOUBLE_EQ(camera.aspect, stored.aspect);
    EXPECT_FALSE(_effect->isSessionBakePending(sessions.front())) << "an open session is not baked";
}

TEST_F(TexturePaintInteractTest, ACameraChangeEndsTheSessionAndQueuesItsBake)
{
    ASSERT_TRUE(_effect != nullptr);
    TexturePaintInteract* view = _effect->getViewInteract();

    stroke(cameraAt(5.), 0.5, 0.5);
    const RotoLayerPtr first = view->getCurrentSession();
    ASSERT_TRUE(bool(first));

    // Checked before the event loop runs, which is where a bake lands.
    ASSERT_TRUE(view->penDown(cameraAt(7.), kViewWidth, kViewHeight, 0.5, 0.5, 1., 1., time()));
    EXPECT_TRUE(_effect->isSessionBakePending(first)) << "the first session's bake is queued";
    const RotoLayerPtr second = view->getCurrentSession();
    ASSERT_TRUE(bool(second));
    EXPECT_NE(first, second);
    EXPECT_EQ(2u, _effect->getSessions().size());
    EXPECT_TRUE(view->penUp());
    landNeatRender();
    EXPECT_FALSE(_effect->isSessionBakePending(second));
    SceneRaster::ExplicitCamera stored;
    ASSERT_TRUE(TexturePaint::getSessionCamera(second, &stored));
    EXPECT_DOUBLE_EQ(7., stored.cameraToWorld[14]);

    // As the view does when it navigates or Bake Paint is pressed.
    EXPECT_TRUE(view->closeSession(time()));
    EXPECT_FALSE(bool(view->getCurrentSession()));
    EXPECT_TRUE(_effect->isSessionBakePending(second));
    EXPECT_FALSE(view->closeSession(time())) << "nothing is left to close";

    stroke(cameraAt(7.), 0.5, 0.5);
    EXPECT_EQ(3u, _effect->getSessions().size()) << "a stroke after a close starts a session, through the same camera too";

    ASSERT_TRUE(pumpUntil([this]() { return !_effect->isBaking(); }, 60000));
    EXPECT_FALSE(_effect->isSessionBakePending(first));
    EXPECT_FALSE(_effect->isSessionBakePending(second));
}

TEST_F(TexturePaintInteractTest, AStrokeOnAFrozenSessionStartsANewOneThroughTheSameCamera)
{
    ASSERT_TRUE(_effect != nullptr);
    TexturePaintInteract* view = _effect->getViewInteract();
    const SceneRaster::ExplicitCamera camera = cameraAt(5.);

    stroke(camera, 0.5, 0.5);
    const RotoLayerPtr frozen = view->getCurrentSession();
    ASSERT_TRUE(bool(frozen));
    TexturePaint::setSessionFrozen(frozen, true);
    EXPECT_FALSE(bool(view->getCurrentSession())) << "a frozen session takes no more strokes";

    stroke(camera, 0.25, 0.5);
    const RotoLayerPtr next = view->getCurrentSession();
    ASSERT_TRUE(bool(next));
    EXPECT_NE(frozen, next);
    EXPECT_EQ(2u, _effect->getSessions().size());
    EXPECT_EQ(1u, strokesIn(frozen).size());
    EXPECT_EQ(1u, strokesIn(next).size());
    EXPECT_FALSE(_effect->isSessionBakePending(frozen)) << "a frozen session is never projected again";
    SceneRaster::ExplicitCamera stored;
    ASSERT_TRUE(TexturePaint::getSessionCamera(next, &stored));
    for (int i = 0; i < 16; ++i) {
        EXPECT_DOUBLE_EQ(camera.cameraToWorld[i], stored.cameraToWorld[i]) << i;
    }
    EXPECT_DOUBLE_EQ(camera.viewHeight, stored.viewHeight);
}

TEST_F(TexturePaintInteractTest, AToolSwitchEndsTheSession)
{
    ASSERT_TRUE(_effect != nullptr);
    TexturePaintInteract* view = _effect->getViewInteract();
    const RotoPaintInteractPtr paint = _effect->getPaintInteract();
    const SceneRaster::ExplicitCamera camera = cameraAt(5.);

    stroke(camera, 0.5, 0.5);
    const RotoLayerPtr first = view->getCurrentSession();
    ASSERT_TRUE(bool(first));

    pickTool(paint->eraserAction.lock());
    view->onToolChanged(time());
    EXPECT_FALSE(bool(view->getCurrentSession()));
    EXPECT_TRUE(_effect->isSessionBakePending(first));

    stroke(camera, 0.5, 0.5);
    const RotoLayerPtr eraser = view->getCurrentSession();
    ASSERT_TRUE(bool(eraser));
    EXPECT_NE(first, eraser);

    // A switch the view was not told of still ends the session at the next stroke.
    pickTool(paint->brushAction.lock());
    stroke(camera, 0.5, 0.5);
    EXPECT_NE(eraser, view->getCurrentSession());
    EXPECT_EQ(3u, _effect->getSessions().size());
}

TEST_F(TexturePaintInteractTest, BrushSizeAndPositionMapIntoPaintSpace)
{
    ASSERT_TRUE(_effect != nullptr);
    TexturePaintInteract* view = _effect->getViewInteract();
    const RotoPaintInteractPtr paint = _effect->getPaintInteract();
    const KnobDoublePtr size = paint->sizeSpinbox.lock();
    ASSERT_TRUE(bool(size));
    size->setValue(20.);

    EXPECT_DOUBLE_EQ(8192. / 1600., TexturePaintInteract::brushSizeScale(1600, 900));
    EXPECT_DOUBLE_EQ(8192. / 1200., TexturePaintInteract::brushSizeScale(800, 1200));
    EXPECT_DOUBLE_EQ(8192., TexturePaintInteract::brushSizeScale(0, 0));

    const SceneRaster::ExplicitCamera camera = cameraAt(5.);
    stroke(camera, 0.25, 0.75);
    const RotoLayerPtr session = view->getCurrentSession();
    ASSERT_TRUE(bool(session));
    const std::list<RotoStrokeItemPtr> strokes = strokesIn(session);
    ASSERT_EQ(1u, strokes.size());
    const RotoStrokeItemPtr painted = strokes.front();
    EXPECT_NEAR(20. * (double)TexturePaintView::kPaintSpaceSize / (double)kViewWidth, painted->getBrushSizeKnob()->getValue(), 1e-9)
        << "the knob's pixels of the view, in paint-space units";
    EXPECT_DOUBLE_EQ(20., size->getValue()) << "the toolbar keeps its value";

    double x = 0.;
    double y = 0.;
    TexturePaintView::viewToPaintSpace(camera.aspect, 0.25, 0.75, &x, &y);
    std::list<std::list<std::pair<Point, double>>> points;
    painted->evaluateStroke(0, time(), &points);
    ASSERT_FALSE(points.empty());
    ASSERT_FALSE(points.front().empty());
    EXPECT_NEAR(x, points.front().front().first.x, 1e-3);
    EXPECT_NEAR(y, points.front().front().first.y, 1e-3);
}
