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

#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QObject>
#include <QString>

#include "BaseTest.h"
#include "ViewerInstanceTestAccess.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Project.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_USING

namespace {

class ViewerStaleStateTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_VIEWER));
        ASSERT_TRUE(bool(node));
        viewer = dynamic_cast<ViewerInstance*>(node->getEffectInstance().get());
        ASSERT_TRUE(viewer != 0);
        QObject::connect(viewer, &ViewerInstance::staleChanged, viewer, [this](bool stale) {
            transitions.push_back(stale);
        });
    }

    void drain()
    {
        QCoreApplication::processEvents();
    }

    ViewerInstance* viewer = 0;
    std::vector<bool> transitions;
};

} // namespace

TEST_F(ViewerStaleStateTest, CurrentUntilARenderIsRequested)
{
    EXPECT_FALSE(viewer->isStale());
    drain();
    EXPECT_TRUE(transitions.empty());
}

TEST_F(ViewerStaleStateTest, StaleFromRequestUntilShown)
{
    const U64 age = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    EXPECT_TRUE(viewer->isStale());
    ViewerInstanceTestAccess::renderShown(viewer, 0, age);
    EXPECT_FALSE(viewer->isStale());
    drain();
    ASSERT_EQ(2u, transitions.size());
    EXPECT_TRUE(transitions[0]);
    EXPECT_FALSE(transitions[1]);
}

TEST_F(ViewerStaleStateTest, AbortedRenderStaysStaleUntilTheNewerOneLands)
{
    const U64 aborted = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    const U64 newer = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    (void)aborted;
    EXPECT_TRUE(viewer->isStale());
    ViewerInstanceTestAccess::renderShown(viewer, 0, newer);
    EXPECT_FALSE(viewer->isStale());
    drain();
    EXPECT_EQ(2u, transitions.size());
}

TEST_F(ViewerStaleStateTest, LateOlderRenderDoesNotMakeItStaleAgain)
{
    const U64 older = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    const U64 newer = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    ViewerInstanceTestAccess::renderShown(viewer, 0, newer);
    ViewerInstanceTestAccess::renderShown(viewer, 0, older);
    EXPECT_FALSE(viewer->isStale());
    drain();
    EXPECT_EQ(2u, transitions.size());
}

TEST_F(ViewerStaleStateTest, FailedRenderWithNothingNewerIsCurrent)
{
    // A failed render reports through the same call as a shown one.
    const U64 age = ViewerInstanceTestAccess::requestRender(viewer, 0, false, false);
    EXPECT_TRUE(viewer->isStale());
    ViewerInstanceTestAccess::renderShown(viewer, 0, age);
    EXPECT_FALSE(viewer->isStale());
}

TEST_F(ViewerStaleStateTest, WipeIsStaleIfEitherInputIs)
{
    const U64 a = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    const U64 b = ViewerInstanceTestAccess::requestRender(viewer, 1, true, false);
    ViewerInstanceTestAccess::renderShown(viewer, 0, a);
    EXPECT_TRUE(viewer->isStale());
    ViewerInstanceTestAccess::renderShown(viewer, 1, b);
    EXPECT_FALSE(viewer->isStale());
    drain();
    // One transition each way: input B kept the viewer stale while A landed.
    ASSERT_EQ(2u, transitions.size());
    EXPECT_TRUE(transitions[0]);
    EXPECT_FALSE(transitions[1]);
}

TEST_F(ViewerStaleStateTest, RenderThreadTransitionIsDeliveredOnTheMainThread)
{
    std::thread::id receiver;
    QObject::connect(viewer, &ViewerInstance::staleChanged, viewer, [&receiver](bool) {
        receiver = std::this_thread::get_id();
    });
    std::thread worker([this]() {
        const U64 age = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
        ViewerInstanceTestAccess::renderShown(viewer, 0, age);
    });
    worker.join();
    EXPECT_TRUE(transitions.empty());
    drain();
    EXPECT_EQ(std::this_thread::get_id(), receiver);
    ASSERT_EQ(2u, transitions.size());
}

TEST_F(ViewerStaleStateTest, PausedViewerStaysStaleUntilARefreshLands)
{
    viewer->setViewerPaused(true, true);
    // A paused viewer still takes the request but never uploads its image, so no display age follows.
    const U64 pausedRequest = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    (void)pausedRequest;
    EXPECT_TRUE(viewer->isStale());
    viewer->setViewerPaused(false, true);
    EXPECT_TRUE(viewer->isStale());
    const U64 refresh = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    ViewerInstanceTestAccess::renderShown(viewer, 0, refresh);
    EXPECT_FALSE(viewer->isStale());
}

TEST_F(ViewerStaleStateTest, RenderEndedWithoutImageIsCurrentWhenNothingIsNewer)
{
    const U64 age = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    EXPECT_TRUE(viewer->isStale());
    viewer->renderEndedWithoutImage(0, age);
    EXPECT_FALSE(viewer->isStale());
}

TEST_F(ViewerStaleStateTest, RenderEndedWithoutImageStaysStaleWhileANewerOneIsQueued)
{
    const U64 aborted = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    const U64 newer = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    viewer->renderEndedWithoutImage(0, aborted);
    EXPECT_TRUE(viewer->isStale());
    viewer->renderEndedWithoutImage(0, newer);
    EXPECT_FALSE(viewer->isStale());
    drain();
    ASSERT_EQ(2u, transitions.size());
}

TEST_F(ViewerStaleStateTest, RenderEndedWithoutImageOnOneInputLeavesTheOtherStale)
{
    const U64 a = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    const U64 b = ViewerInstanceTestAccess::requestRender(viewer, 1, true, false);
    viewer->renderEndedWithoutImage(0, a);
    EXPECT_TRUE(viewer->isStale());
    viewer->renderEndedWithoutImage(1, b);
    EXPECT_FALSE(viewer->isStale());
}

TEST_F(ViewerStaleStateTest, RenderFailedOutsideTheRenderIsCurrent)
{
    const U64 age = ViewerInstanceTestAccess::requestRender(viewer, 0, false, false);
    viewer->renderFailed(0, age);
    EXPECT_FALSE(viewer->isStale());
}

TEST_F(ViewerStaleStateTest, SettlingTwiceKeepsTheViewerCurrent)
{
    const U64 age = ViewerInstanceTestAccess::requestRender(viewer, 0, true, false);
    viewer->renderEndedWithoutImage(0, age);
    viewer->renderFailed(0, age);
    EXPECT_FALSE(viewer->isStale());
    drain();
    EXPECT_EQ(2u, transitions.size());
}

namespace {

// Auto-proxy's default "4", at a fit-to-window zoom whose own level is 1.
const unsigned int kAutoProxyLevel = 2;
const unsigned int kOwnLevel = 1;
const double kFitZoom = 0.4;

void
resolveForEdits(const AppInstancePtr& app,
                const AdaptiveDraft& adaptive,
                bool* drafts,
                unsigned int* level)
{
    ViewerInstance::resolveDraft(true, app->draftAppliesToEdits(), true, adaptive, kOwnLevel, kFitZoom, kAutoProxyLevel, drafts, level);
}

} // namespace

TEST_F(ViewerStaleStateTest, DraggingAPointOpRendersAtTheViewersOwnLevel)
{
    const NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE);
    const NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
    ASSERT_TRUE(grade && transform);
    EXPECT_TRUE(grade->getEffectInstance()->isPointOp());
    EXPECT_FALSE(transform->getEffectInstance()->isPointOp());
    const AppInstancePtr app = getApp();
    const AdaptiveDraft adaptive;
    bool drafts = false;
    unsigned int level = 0;

    app->clearDraftEdits();
    app->noteDraftEdit(grade);
    resolveForEdits(app, adaptive, &drafts, &level);
    EXPECT_FALSE(drafts);
    EXPECT_EQ(kOwnLevel, level);

    app->clearDraftEdits();
    app->noteDraftEdit(transform);
    resolveForEdits(app, adaptive, &drafts, &level);
    EXPECT_TRUE(drafts);
    EXPECT_EQ(kAutoProxyLevel, level);

    // A link or an expression edits several nodes at once: any of them that is not a point op drafts.
    app->noteDraftEdit(grade);
    resolveForEdits(app, adaptive, &drafts, &level);
    EXPECT_TRUE(drafts);
    EXPECT_EQ(kAutoProxyLevel, level);

    // Scrubbing the timeline edits nothing, and drafts as it always has.
    app->clearDraftEdits();
    resolveForEdits(app, adaptive, &drafts, &level);
    EXPECT_TRUE(drafts);
    EXPECT_EQ(kAutoProxyLevel, level);

    ViewerInstance::resolveDraft(false, true, true, adaptive, kOwnLevel, kFitZoom, kAutoProxyLevel, &drafts, &level);
    EXPECT_FALSE(drafts);
    EXPECT_EQ(kOwnLevel, level);
}

TEST(AdaptiveDraftTest, CheapEditsSwitchToFullResolution)
{
    AdaptiveDraft adaptive;
    EXPECT_TRUE(adaptive.drafts());
    // 1 ms at two levels below the viewer's own predicts 16 ms there, inside the budget.
    adaptive.noteRender(1., kAutoProxyLevel + kOwnLevel, kOwnLevel);
    EXPECT_FALSE(adaptive.drafts());
    for (int i = 0; i < 20; ++i) {
        adaptive.noteRender(16., kOwnLevel, kOwnLevel);
        EXPECT_FALSE(adaptive.drafts()) << "update " << i;
    }
}

TEST(AdaptiveDraftTest, ExpensiveEditsStayInDraft)
{
    AdaptiveDraft adaptive;
    // 10 ms at two levels below predicts 160 ms at the viewer's own level.
    for (int i = 0; i < 20; ++i) {
        adaptive.noteRender(10., kAutoProxyLevel + kOwnLevel, kOwnLevel);
        EXPECT_TRUE(adaptive.drafts()) << "update " << i;
    }
}

TEST(AdaptiveDraftTest, CostsNearTheBudgetDoNotOscillate)
{
    AdaptiveDraft adaptive;
    adaptive.noteRender(1., 2, 0);
    ASSERT_FALSE(adaptive.drafts());
    // Full-resolution updates alternating around the budget stay inside the hysteresis band.
    const double costs[] = { 30., 38., 28., 40., 33., 36. };
    for (int round = 0; round < 4; ++round) {
        for (double ms : costs) {
            adaptive.noteRender(ms, 0, 0);
            EXPECT_FALSE(adaptive.drafts()) << ms << " ms";
        }
    }
    // A sustained overrun goes back to draft, and then the drafts at a quarter of the pixels, still predicting an
    // overrun, keep it there.
    for (int i = 0; i < 4; ++i) {
        adaptive.noteRender(80., 0, 0);
    }
    EXPECT_TRUE(adaptive.drafts());
    for (int i = 0; i < 8; ++i) {
        adaptive.noteRender(9., 1, 0);
        EXPECT_TRUE(adaptive.drafts()) << "update " << i;
    }

    adaptive.reset();
    EXPECT_TRUE(adaptive.drafts());
    EXPECT_LT(adaptive.predictedFullMs(), 0.);
}

TEST(ViewerMipRoundingTest, FinerAndNearest)
{
    EXPECT_EQ(1u, ViewerInstance::mipmapLevelForZoom(0.33, false));
    EXPECT_EQ(2u, ViewerInstance::mipmapLevelForZoom(0.33, true));
    EXPECT_EQ(1u, ViewerInstance::mipmapLevelForZoom(0.48, false));
    EXPECT_EQ(0u, ViewerInstance::mipmapLevelForZoom(0.51, false));
    EXPECT_EQ(1u, ViewerInstance::mipmapLevelForZoom(0.51, true));
    EXPECT_EQ(1u, ViewerInstance::mipmapLevelForZoom(0.5, false));
    EXPECT_EQ(1u, ViewerInstance::mipmapLevelForZoom(0.5, true));
    EXPECT_EQ(0u, ViewerInstance::mipmapLevelForZoom(1., true));
    EXPECT_EQ(0u, ViewerInstance::mipmapLevelForZoom(4., false));
}
