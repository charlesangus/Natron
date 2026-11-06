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

#include "Engine/AppInstance.h"
#include "Engine/Node.h"
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
    const U64 age = viewer->requestRenderForTests(0, true, false);
    EXPECT_TRUE(viewer->isStale());
    viewer->renderShownForTests(0, age);
    EXPECT_FALSE(viewer->isStale());
    drain();
    ASSERT_EQ(2u, transitions.size());
    EXPECT_TRUE(transitions[0]);
    EXPECT_FALSE(transitions[1]);
}

TEST_F(ViewerStaleStateTest, AbortedRenderStaysStaleUntilTheNewerOneLands)
{
    const U64 aborted = viewer->requestRenderForTests(0, true, false);
    const U64 newer = viewer->requestRenderForTests(0, true, false);
    (void)aborted;
    EXPECT_TRUE(viewer->isStale());
    viewer->renderShownForTests(0, newer);
    EXPECT_FALSE(viewer->isStale());
    drain();
    EXPECT_EQ(2u, transitions.size());
}

TEST_F(ViewerStaleStateTest, LateOlderRenderDoesNotMakeItStaleAgain)
{
    const U64 older = viewer->requestRenderForTests(0, true, false);
    const U64 newer = viewer->requestRenderForTests(0, true, false);
    viewer->renderShownForTests(0, newer);
    viewer->renderShownForTests(0, older);
    EXPECT_FALSE(viewer->isStale());
    drain();
    EXPECT_EQ(2u, transitions.size());
}

TEST_F(ViewerStaleStateTest, FailedRenderWithNothingNewerIsCurrent)
{
    // A failed render reports through the same call as a shown one.
    const U64 age = viewer->requestRenderForTests(0, false, false);
    EXPECT_TRUE(viewer->isStale());
    viewer->renderShownForTests(0, age);
    EXPECT_FALSE(viewer->isStale());
}

TEST_F(ViewerStaleStateTest, WipeIsStaleIfEitherInputIs)
{
    const U64 a = viewer->requestRenderForTests(0, true, false);
    const U64 b = viewer->requestRenderForTests(1, true, false);
    viewer->renderShownForTests(0, a);
    EXPECT_TRUE(viewer->isStale());
    viewer->renderShownForTests(1, b);
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
        const U64 age = viewer->requestRenderForTests(0, true, false);
        viewer->renderShownForTests(0, age);
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
    const U64 pausedRequest = viewer->requestRenderForTests(0, true, false);
    (void)pausedRequest;
    EXPECT_TRUE(viewer->isStale());
    viewer->setViewerPaused(false, true);
    EXPECT_TRUE(viewer->isStale());
    const U64 refresh = viewer->requestRenderForTests(0, true, false);
    viewer->renderShownForTests(0, refresh);
    EXPECT_FALSE(viewer->isStale());
}
