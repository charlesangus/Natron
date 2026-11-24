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

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/GPUContextPool.h"
#include "Engine/Node.h"
#include "Engine/OSGLContext.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardPluginID = "CheckerBoard";

// Without OpenGL the pool never hands out a context, so both setters would agree vacuously; a run meant to
// exercise GL sets NATRON_TEST_REQUIRE_GL=1 to turn the skip into a failure.
bool
skipWithoutOpenGL(const char* testName,
                  bool available,
                  const char* why)
{
    if (available) {
        return false;
    }
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << why;

        return true;
    }
    std::cout << "[  SKIPPED ] KnobChangeGLContext." << testName << ": " << why
              << "; run under Tests/gui/run-gl-tests.sh with NATRON_TEST_REQUIRE_GL=1" << std::endl;

    return true;
}

class RenderingEnabledGuard {
public:
    RenderingEnabledGuard()
    {
        appPTR->getGPUContextPool()->setRenderingEnabledForTests(true);
    }

    ~RenderingEnabledGuard()
    {
        appPTR->getGPUContextPool()->setRenderingEnabledForTests(std::nullopt);
    }

    RenderingEnabledGuard(const RenderingEnabledGuard&) = delete;
    RenderingEnabledGuard& operator=(const RenderingEnabledGuard&) = delete;
};

} // namespace

class KnobChangeGLContext
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

    std::shared_ptr<ParallelRenderArgsSetter> makeSetter(const NodePtr& node,
                                                         bool setUpstreamArgs)
    {
        return std::make_shared<ParallelRenderArgsSetter>(getApp()->getTimeLine()->currentFrame(),
                                                          ViewIdx(0),
                                                          true, // isRenderUserInteraction
                                                          false, // isSequential
                                                          AbortableRenderInfo::create(false, 0),
                                                          node,
                                                          0, // textureIndex
                                                          getApp()->getTimeLine().get(),
                                                          NodePtr(), // activeRotoPaintNode
                                                          true, // isAnalysis
                                                          false, // draftMode
                                                          RenderStatsPtr(),
                                                          setUpstreamArgs);
    }
};

// Knob and input changes install frame args with setUpstreamArgs false on the main thread; they must not take a
// render context from the pool, whose first creation stalls the UI. A rendering setter still gets one.
TEST_F(KnobChangeGLContext, NonRenderingSetterAttachesNoContext)
{
    if (skipWithoutOpenGL("NonRenderingSetterAttachesNoContext", appPTR->isOpenGLLoaded(), "OpenGL did not load")) {
        return;
    }
    RenderingEnabledGuard guard;
    if (skipWithoutOpenGL("NonRenderingSetterAttachesNoContext", appPTR->getGPUContextPool()->isRenderingEnabled(),
                          "OpenGL rendering stayed disabled in the GPU context pool")) {
        return;
    }

    NodePtr node = createNode(QString::fromUtf8(kCheckerBoardPluginID));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    {
        std::shared_ptr<ParallelRenderArgsSetter> setter = makeSetter(node, false);
        ParallelRenderArgsPtr args = effect->getParallelRenderArgsTLS();
        ASSERT_TRUE(bool(args));
        EXPECT_FALSE(bool(args->openGLContext.lock()));
        EXPECT_FALSE(bool(effect->getThreadLocalOpenGLContext()));
    }

    {
        std::shared_ptr<ParallelRenderArgsSetter> setter = makeSetter(node, true);
        ParallelRenderArgsPtr args = effect->getParallelRenderArgsTLS();
        ASSERT_TRUE(bool(args));
        EXPECT_TRUE(bool(args->openGLContext.lock()));
        EXPECT_TRUE(bool(effect->getThreadLocalOpenGLContext()));
    }
}
