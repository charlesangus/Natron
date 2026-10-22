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
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QThreadPool>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"
#include "GlHostWrapTestEffect.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GPUContextPool.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kNoOpenGLMessage = "OpenGL did not load, so this test cannot exercise GL rendering. Run it under Xvfb with GLX and Mesa llvmpipe, as "
                                     "Tests/gui/run-gl-tests.sh does.";
const char* const kGLRenderingDisabledMessage = "OpenGL loaded, but OpenGL rendering stayed disabled in the GPU context pool after the test enabled it.";

const double kTime = 1.;

// Without OpenGL the tests print a skip line and pass, so that ctest runs without a display stay green;
// NATRON_TEST_REQUIRE_GL=1 turns that into a failure, so a run meant to exercise GL cannot pass vacuously.
bool
skipWithoutOpenGL(const char* testName,
                  bool available,
                  const char* failureMessage = kNoOpenGLMessage)
{
    if (available) {
        return false;
    }
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << failureMessage;

        return true;
    }
    std::cout << "[  SKIPPED ] GlHostWrap." << testName
              << ": OpenGL not available; run under Tests/gui/run-gl-tests.sh with NATRON_TEST_REQUIRE_GL=1"
              << std::endl;

    return true;
}

// Forces GL rendering on in the GPU context pool and in the project, and raises the thread pool to at least 2 threads,
// restoring all of it however the test exits. Under Mesa llvmpipe the pool reports GL rendering disabled, which
// leaves nothing for these tests to exercise.
class OpenGLRenderingGuard {
public:
    explicit OpenGLRenderingGuard(const ProjectPtr& project)
        : _projectKnob(project->getKnobByNameAndType<KnobChoice>("gpuRendering"))
        , _savedProject(_projectKnob ? _projectKnob->getValue() : 0)
        , _savedPoolSize(QThreadPool::globalInstance()->maxThreadCount())
    {
        appPTR->getGPUContextPool()->setRenderingEnabledForTests(true);
        if (_projectKnob) {
            _projectKnob->setValue(0);
        }
        if (_savedPoolSize < 2) {
            QThreadPool::globalInstance()->setMaxThreadCount(2);
        }
    }

    ~OpenGLRenderingGuard()
    {
        QThreadPool::globalInstance()->setMaxThreadCount(_savedPoolSize);
        if (_projectKnob) {
            _projectKnob->setValue(_savedProject);
        }
        appPTR->getGPUContextPool()->setRenderingEnabledForTests(std::nullopt);
    }

    bool isValid() const
    {
        return bool(_projectKnob);
    }

    OpenGLRenderingGuard(const OpenGLRenderingGuard&) = delete;
    OpenGLRenderingGuard& operator=(const OpenGLRenderingGuard&) = delete;

private:
    KnobChoicePtr _projectKnob;
    int _savedProject;
    int _savedPoolSize;
};

class SchedulerModeRestorer {
public:
    SchedulerModeRestorer()
        : _saved(appPTR->getRenderSchedulerMode())
    {
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeTaskGraph);
    }

    ~SchedulerModeRestorer()
    {
        appPTR->setRenderSchedulerMode(_saved);
        appPTR->clearAllCaches();
    }

    SchedulerModeRestorer(const SchedulerModeRestorer&) = delete;
    SchedulerModeRestorer& operator=(const SchedulerModeRestorer&) = delete;

private:
    RenderSchedulerModeEnum _saved;
};

struct Rendered {
    bool ok = false;
    std::string error;
    StorageModeEnum storage = eStorageModeNone;
    std::vector<float> pixels;
};

// Renders `window` of the node as a frame of the render scheduler, which is what gives a task the OpenGL context a
// GL render needs, and reads the colour plane back as RGBA.
Rendered
renderFrame(const NodePtr& node,
            const RectI& window)
{
    Rendered result;

    appPTR->clearAllCaches();
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    RenderStatsPtr stats = std::make_shared<RenderStats>(false);
    ParallelRenderArgsSetter frameRenderArgs(kTime,
                                             ViewIdx(0),
                                             false /*isRenderUserInteraction*/,
                                             false /*isSequential*/,
                                             abortInfo,
                                             node,
                                             0 /*textureIndex*/,
                                             node->getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             false /*isAnalysis*/,
                                             false /*draftMode*/,
                                             stats);
    const RectD canonicalWindow = window.toCanonical_noClipping(0, node->getEffectInstance()->getAspectRatio(-1));
    std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
    if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0, canonicalWindow, node, *request) == eStatusFailed) {
        result.error = "request pass failed";

        return result;
    }
    frameRenderArgs.updateNodesRequest(*request);

    FrameRenderContextPtr context = FrameRenderContext::createFromSetter(frameRenderArgs, abortInfo, stats, kTime, ViewIdx(0));
    context->setRequest(request);
    FrameGraph graph = RenderScheduler::buildGraph(context, node, kTime, ViewIdx(0), 0);
    if (graph.tasks.empty()) {
        result.error = "the scheduler has no task to run";

        return result;
    }
    FrameFuturePtr future = appPTR->getRenderScheduler()->submit(context, std::move(graph), RenderScheduler::Priority::Background);
    if (future->wait() != EffectInstance::eRenderRoIRetCodeOk) {
        result.error = "the scheduler did not complete the frame";

        return result;
    }

    const std::map<ImageLayerDesc, ImagePtr> planes = future->getRootPlanes();
    std::map<ImageLayerDesc, ImagePtr>::const_iterator rgba = planes.find(ImageLayerDesc::getRGBAComponents());
    if ((rgba == planes.end()) || !rgba->second) {
        result.error = "the render produced no RGBA plane";

        return result;
    }
    const ImagePtr& image = rgba->second;
    result.storage = image->getStorageMode();
    if (result.storage != eStorageModeRAM) {
        return result;
    }
    if ((image->getBitDepth() != eImageBitDepthFloat) || (image->getComponentsCount() != 4) || !image->getBounds().contains(window)) {
        result.error = "the RGBA plane is not float RGBA over the window";

        return result;
    }
    result.pixels.resize(static_cast<std::size_t>(window.width()) * window.height() * 4);
    Image::ReadAccess access = image->getReadRights();
    for (int y = window.y1; y < window.y2; ++y) {
        const float* row = reinterpret_cast<const float*>(access.pixelAt(window.x1, y));
        std::memcpy(&result.pixels[static_cast<std::size_t>(y - window.y1) * window.width() * 4], row, static_cast<std::size_t>(window.width()) * 4 * sizeof(float));
    }
    result.ok = true;

    return result;
}

} // namespace

class GlHostWrap
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        Format format(0, 0, kParitySourceWidth, kParitySourceHeight, "glHostWrapFormat", 1.);
        getApp()->getProject()->setOrAddProjectFormat(format);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    struct Setup {
        NodePtr source;
        NodePtr mask;
        NodePtr effect;
    };

    void buildGraph(bool withMask,
                    Setup* setup)
    {
        setup->source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        ASSERT_TRUE(bool(setup->source));
        setup->effect = createNode(QString::fromUtf8(kTestPluginIDGlHostWrap));
        ASSERT_TRUE(bool(setup->effect));
        connectNodes(setup->source, setup->effect, 0, true);
        if (withMask) {
            setup->mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
            ASSERT_TRUE(bool(setup->mask));
            connectNodes(setup->mask, setup->effect, 1, true);
            ASSERT_TRUE(setKnobValues(setup->effect, "enableMask_Mask", std::vector<double>(1, 1.)));
        }
    }

    void setProcessChannels(const NodePtr& effect,
                            const std::vector<std::string>& channels)
    {
        KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(effect->getKnobByName(kNodeParamChannelSet));
        ASSERT_TRUE(bool(channelSet));
        channelSet->setLayer(0, kNatronColorViewRGBA, &channels);
    }

    void setGpuRendering(const NodePtr& effect,
                         bool enabled)
    {
        KnobChoicePtr knob = std::dynamic_pointer_cast<KnobChoice>(effect->getKnobByName("enableGPURendering"));
        ASSERT_TRUE(bool(knob));
        knob->setValueFromID(enabled ? "Enabled" : "Disabled", 0);
    }

    // Renders the graph on the GPU and again on the CPU and requires the same pixels. Returns the CPU render, for
    // the caller's checks of what the wrappers did.
    void renderBothAndCompare(const char* testName,
                              const Setup& setup,
                              std::vector<float>* cpuPixels)
    {
        if (skipWithoutOpenGL(testName, appPTR->isOpenGLLoaded())) {
            return;
        }
        OpenGLRenderingGuard guard(getApp()->getProject());
        ASSERT_TRUE(guard.isValid());
        if (skipWithoutOpenGL(testName, appPTR->getGPUContextPool()->isRenderingEnabled(), kGLRenderingDisabledMessage)) {
            return;
        }

        const RectI window = paritySourceWindow(setup.source, kTime, 0);
        DisableUnreachableRAMPurging noPurge;
        SchedulerModeRestorer mode;
        GpuPlacementOffGuard cpuPlacement;
        appPTR->getGPUContextPool()->clear();

        ASSERT_NO_FATAL_FAILURE(setGpuRendering(setup.effect, true));
        GlHostWrapTestEffect::glRenders() = 0;
        GlHostWrapTestEffect::cpuRenders() = 0;
        const Rendered gl = renderFrame(setup.effect, window);
        ASSERT_TRUE(gl.error.empty()) << "GL pass: " << gl.error;
        EXPECT_GT(GlHostWrapTestEffect::glRenders().load(), 0) << "the GL pass did not render through GL";
        EXPECT_EQ(0, GlHostWrapTestEffect::cpuRenders().load()) << "the GL pass also rendered on the CPU";
        ASSERT_EQ(eStorageModeRAM, gl.storage) << "the GL pass output is not in RAM";
        ASSERT_TRUE(gl.ok);

        ASSERT_NO_FATAL_FAILURE(setGpuRendering(setup.effect, false));
        GlHostWrapTestEffect::glRenders() = 0;
        const Rendered cpu = renderFrame(setup.effect, window);
        ASSERT_TRUE(cpu.error.empty()) << "CPU pass: " << cpu.error;
        ASSERT_TRUE(cpu.ok);
        EXPECT_EQ(0, GlHostWrapTestEffect::glRenders().load()) << "the CPU pass rendered through GL";

        ASSERT_EQ(cpu.pixels.size(), gl.pixels.size());
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < cpu.pixels.size(); ++i) {
            if (cpu.pixels[i] != gl.pixels[i]) {
                if (mismatches == 0) {
                    ADD_FAILURE() << "first difference at pixel " << (i / 4) << " channel " << (i % 4) << ": GL " << gl.pixels[i] << ", CPU " << cpu.pixels[i];
                }
                ++mismatches;
            }
        }
        EXPECT_EQ(0u, mismatches) << "GL and CPU renders differ";
        *cpuPixels = cpu.pixels;
    }

    // The value the parity source holds at pixel index `i` of the window, for channel `c`.
    static float sourceValue(const RectI& window,
                             std::size_t i,
                             int c)
    {
        const int x = static_cast<int>(i % window.width());
        const int y = static_cast<int>(i / window.width());

        return paritySourceColorValue(c, x, y, kTime);
    }

    // Whether any pixel of channel `c` differs from `reference`.
    static bool anyPixelDiffers(const std::vector<float>& pixels,
                                int c,
                                float reference)
    {
        for (std::size_t i = static_cast<std::size_t>(c); i < pixels.size(); i += 4) {
            if (pixels[i] != reference) {
                return true;
            }
        }

        return false;
    }
};

TEST_F(GlHostWrap, MixHalfWithoutMask)
{
    Setup setup;
    ASSERT_NO_FATAL_FAILURE(buildGraph(false, &setup));
    ASSERT_TRUE(setKnobValues(setup.effect, "hostMix", std::vector<double>(1, 0.5)));

    std::vector<float> cpu;
    ASSERT_NO_FATAL_FAILURE(renderBothAndCompare("MixHalfWithoutMask", setup, &cpu));
    if (cpu.empty()) {
        return;
    }
    for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(anyPixelDiffers(cpu, c, kGlHostWrapOutput[c])) << "channel " << c << " shows only the effect's output, so the mix did nothing";
    }
}

TEST_F(GlHostWrap, MaskWithFullMix)
{
    Setup setup;
    ASSERT_NO_FATAL_FAILURE(buildGraph(true, &setup));

    std::vector<float> cpu;
    ASSERT_NO_FATAL_FAILURE(renderBothAndCompare("MaskWithFullMix", setup, &cpu));
    if (cpu.empty()) {
        return;
    }
    for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(anyPixelDiffers(cpu, c, kGlHostWrapOutput[c])) << "channel " << c << " shows only the effect's output, so the mask did nothing";
    }
}

TEST_F(GlHostWrap, ProcessRedAndAlphaOnly)
{
    Setup setup;
    ASSERT_NO_FATAL_FAILURE(buildGraph(false, &setup));
    ASSERT_NO_FATAL_FAILURE(setProcessChannels(setup.effect, std::vector<std::string>({ "R", "A" })));

    std::vector<float> cpu;
    ASSERT_NO_FATAL_FAILURE(renderBothAndCompare("ProcessRedAndAlphaOnly", setup, &cpu));
    if (cpu.empty()) {
        return;
    }
    const RectI window = paritySourceWindow(setup.source, kTime, 0);
    for (std::size_t i = 0; i < cpu.size() / 4; ++i) {
        ASSERT_EQ(kGlHostWrapOutput[0], cpu[i * 4]) << "pixel " << i;
        ASSERT_EQ(kGlHostWrapOutput[3], cpu[i * 4 + 3]) << "pixel " << i;
        ASSERT_EQ(sourceValue(window, i, 1), cpu[i * 4 + 1]) << "pixel " << i << " did not get green back from the source";
        ASSERT_EQ(sourceValue(window, i, 2), cpu[i * 4 + 2]) << "pixel " << i << " did not get blue back from the source";
    }
}

TEST_F(GlHostWrap, MixMaskAndChannelSubsetTogether)
{
    Setup setup;
    ASSERT_NO_FATAL_FAILURE(buildGraph(true, &setup));
    ASSERT_TRUE(setKnobValues(setup.effect, "hostMix", std::vector<double>(1, 0.5)));
    ASSERT_NO_FATAL_FAILURE(setProcessChannels(setup.effect, std::vector<std::string>({ "R", "A" })));

    std::vector<float> cpu;
    ASSERT_NO_FATAL_FAILURE(renderBothAndCompare("MixMaskAndChannelSubsetTogether", setup, &cpu));
    if (cpu.empty()) {
        return;
    }
    const RectI window = paritySourceWindow(setup.source, kTime, 0);
    for (std::size_t i = 0; i < cpu.size() / 4; ++i) {
        ASSERT_EQ(sourceValue(window, i, 1), cpu[i * 4 + 1]) << "pixel " << i << " did not get green back from the source";
        ASSERT_EQ(sourceValue(window, i, 2), cpu[i * 4 + 2]) << "pixel " << i << " did not get blue back from the source";
    }
    EXPECT_TRUE(anyPixelDiffers(cpu, 0, kGlHostWrapOutput[0])) << "red shows only the effect's output, so the mix and mask did nothing";
}
