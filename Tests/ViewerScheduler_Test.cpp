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

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardPluginID = "net.sf.openfx.CheckerBoardPlugin";

const int kFormatSize = 256;

const std::vector<int>&
poolSizes()
{
    static const std::vector<int> sizes { 1, 4, 16 };

    return sizes;
}

// Effects read the plug-in's render scale preference when they are constructed, so the nodes
// created while this guard lives do not support render scale and later ones do again.
class RenderScaleDisabledGuard {
public:
    explicit RenderScaleDisabledGuard(const char* pluginID)
        : _plugin(appPTR->getPluginBinary(QString::fromUtf8(pluginID), -1, -1, false))
        , _saved(_plugin ? _plugin->isRenderScaleEnabled() : true)
    {
        if (_plugin) {
            _plugin->setRenderScaleEnabled(false);
        }
    }

    ~RenderScaleDisabledGuard()
    {
        if (_plugin) {
            _plugin->setRenderScaleEnabled(_saved);
        }
    }

    RenderScaleDisabledGuard(const RenderScaleDisabledGuard&) = delete;
    RenderScaleDisabledGuard& operator=(const RenderScaleDisabledGuard&) = delete;

    bool valid() const
    {
        return _plugin != NULL;
    }

private:
    Plugin* _plugin;
    bool _saved;
};

// Everything a viewer frame needs to be rendered by the RenderScheduler.
ViewerInstance::SchedulerEligibility
eligibleFrame()
{
    ViewerInstance::SchedulerEligibility eligibility;

    eligibility.hasFrameArgs = true;
    eligibility.inputHasFrameArgs = true;

    return eligibility;
}

std::string
reasonOf(const ViewerInstance::SchedulerEligibility& eligibility)
{
    const char* reason = 0;

    EXPECT_FALSE(ViewerInstance::isFrameEligibleForScheduler(eligibility, &reason));

    return reason ? std::string(reason) : std::string();
}

} // namespace

class ViewerScheduler
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        Format format(0, 0, kFormatSize, kFormatSize, "viewerSchedulerFormat", 1.);
        getApp()->getProject()->setOrAddProjectFormat(format);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createChecker()
    {
        NodePtr checker = createNode(QString::fromUtf8(kCheckerBoardPluginID));
        EXPECT_TRUE(bool(checker));

        return checker;
    }

    NodePtr createGrade(const NodePtr& source,
                        double factor)
    {
        NodePtr grade = createNode(QString::fromUtf8(PLUGINID_OFX_GRADE));
        EXPECT_TRUE(bool(grade));
        if (!grade) {
            return grade;
        }
        KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
        EXPECT_TRUE(multiply != NULL);
        if (multiply) {
            multiply->setValues(factor, factor, factor, 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        }
        if (source) {
            connectNodes(source, grade, 0, true);
        }

        return grade;
    }
};

// What the viewer asks of its input: the input's planes at the viewer's mipmap level, across a
// chain where one node renders at full scale and takes its input at level 0.
TEST_F(ViewerScheduler, ViewerInputAcrossAGradeChainWithoutRenderScale)
{
    NodePtr upstream = createChecker();
    ASSERT_TRUE(bool(upstream));
    for (int i = 0; i < 20; ++i) {
        const double factor = (i % 2) ? 1.07 : 0.95;
        if (i == 10) {
            RenderScaleDisabledGuard noRenderScale(PLUGINID_OFX_GRADE);
            ASSERT_TRUE(noRenderScale.valid());
            upstream = createGrade(upstream, factor);
            ASSERT_TRUE(bool(upstream));
            ASSERT_EQ(EffectInstance::eSupportsNo, upstream->getEffectInstance()->supportsRenderScaleMaybe());
        } else {
            upstream = createGrade(upstream, factor);
            ASSERT_TRUE(bool(upstream));
        }
    }

    const unsigned mipmapLevels[] = { 0, 2 };
    for (unsigned mipmapLevel : mipmapLevels) {
        const int size = kFormatSize >> mipmapLevel;
        const RenderMismatch m = renderBothWaysDirect(upstream, 1., ViewIdx(0), mipmapLevel, RectI(0, 0, size, size), poolSizes());
        EXPECT_FALSE(m.any) << "mipmap level " << mipmapLevel << ": " << describe(m);
    }
}

TEST_F(ViewerScheduler, EligibleFrame)
{
    const char* reason = "unset";

    EXPECT_TRUE(ViewerInstance::isFrameEligibleForScheduler(eligibleFrame(), &reason));
    EXPECT_TRUE(reason == 0);
    EXPECT_TRUE(ViewerInstance::isFrameEligibleForScheduler(eligibleFrame(), NULL));
}

TEST_F(ViewerScheduler, IneligibleFrames)
{
    ViewerInstance::SchedulerEligibility e;

    e = eligibleFrame();
    e.isDoingPartialUpdates = true;
    EXPECT_EQ("partial updates", reasonOf(e));

    e = eligibleFrame();
    e.deepUpstream = true;
    EXPECT_EQ("deep input", reasonOf(e));

    e = eligibleFrame();
    e.openGLRender = true;
    EXPECT_EQ("OpenGL render", reasonOf(e));

    e = eligibleFrame();
    e.paintStroke = true;
    EXPECT_EQ("paint stroke", reasonOf(e));

    e = eligibleFrame();
    e.forceRender = true;
    EXPECT_EQ("refresh", reasonOf(e));

    e = eligibleFrame();
    e.analysis = true;
    EXPECT_EQ("analysis", reasonOf(e));

    e = eligibleFrame();
    e.onPoolThread = true;
    EXPECT_EQ("render thread is a pool thread", reasonOf(e));

    e = eligibleFrame();
    e.onMainThread = true;
    EXPECT_EQ("render thread is the main thread", reasonOf(e));

    e = eligibleFrame();
    e.hasFrameArgs = false;
    EXPECT_EQ("no frame args", reasonOf(e));

    e = eligibleFrame();
    e.inputHasFrameArgs = false;
    EXPECT_EQ("input outside the frame args", reasonOf(e));
}

TEST_F(ViewerScheduler, FrameArgsScan)
{
    NodePtr first = createChecker();
    NodePtr second = createChecker();
    ASSERT_TRUE(first && second);

    std::map<NodePtr, ParallelRenderArgsPtr> args;
    args[first] = std::make_shared<ParallelRenderArgs>();
    args[second] = std::make_shared<ParallelRenderArgs>();

    {
        ViewerInstance::SchedulerEligibility e = eligibleFrame();
        ViewerInstance::scanFrameArgsForScheduler(args, &e);
        EXPECT_FALSE(e.openGLRender);
        EXPECT_FALSE(e.paintStroke);
        EXPECT_FALSE(e.analysis);
        EXPECT_TRUE(ViewerInstance::isFrameEligibleForScheduler(e, NULL));
    }

    // OpenGL support alone, without a context attached to the frame, renders on the CPU.
    args[first]->currentOpenglSupport = ePluginOpenGLRenderSupportYes;
    {
        ViewerInstance::SchedulerEligibility e = eligibleFrame();
        ViewerInstance::scanFrameArgsForScheduler(args, &e);
        EXPECT_FALSE(e.openGLRender);
    }

    args[second]->isDuringPaintStrokeCreation = true;
    {
        ViewerInstance::SchedulerEligibility e = eligibleFrame();
        ViewerInstance::scanFrameArgsForScheduler(args, &e);
        EXPECT_TRUE(e.paintStroke);
        EXPECT_FALSE(e.analysis);
        EXPECT_EQ("paint stroke", reasonOf(e));
    }

    args[second]->isDuringPaintStrokeCreation = false;
    args[first]->isAnalysis = true;
    {
        ViewerInstance::SchedulerEligibility e = eligibleFrame();
        ViewerInstance::scanFrameArgsForScheduler(args, &e);
        EXPECT_FALSE(e.paintStroke);
        EXPECT_TRUE(e.analysis);
        EXPECT_EQ("analysis", reasonOf(e));
    }

    std::map<NodePtr, ParallelRenderArgsPtr> withNullArgs;
    withNullArgs[first] = ParallelRenderArgsPtr();
    {
        ViewerInstance::SchedulerEligibility e = eligibleFrame();
        ViewerInstance::scanFrameArgsForScheduler(withNullArgs, &e);
        EXPECT_TRUE(ViewerInstance::isFrameEligibleForScheduler(e, NULL));
    }
}
