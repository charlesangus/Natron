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

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Plugin.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#define kDirectOutputProbePluginID "test.natron.built-in.DirectOutputProbe"
#define kDirectOutputProbeViaTemporaryPluginID "test.natron.built-in.DirectOutputProbeViaTemporary"

NATRON_NAMESPACE_ENTER

const int kDirectOutputProbeWidth = 50;
const int kDirectOutputProbeHeight = 40;
const float kDirectOutputProbeAbortedValue = -7.f;

inline float
directOutputProbeValue(int x,
                       int y,
                       int channel)
{
    return (float)x + 128.f * (float)y + 0.25f * (float)channel;
}

// Render clones may run render(), so what it sees is kept here rather than on the instance.
class DirectOutputProbeLog {
public:
    static void reset()
    {
        std::lock_guard<std::mutex> lock(mutex());
        state() = State();
    }

    // The next render() call writes kDirectOutputProbeAbortedValue and then aborts this render.
    static void abortNextRender(const AbortableRenderInfoPtr& abortInfo)
    {
        std::lock_guard<std::mutex> lock(mutex());
        state().abortNext = abortInfo;
    }

    static AbortableRenderInfoPtr noteRender(bool outputUsesBitmap)
    {
        std::lock_guard<std::mutex> lock(mutex());
        State& s = state();

        ++s.renders;
        if (outputUsesBitmap) {
            ++s.bitmapRenders;
        }
        AbortableRenderInfoPtr abortInfo = s.abortNext;
        s.abortNext.reset();

        return abortInfo;
    }

    static int renders()
    {
        std::lock_guard<std::mutex> lock(mutex());

        return state().renders;
    }

    static int bitmapRenders()
    {
        std::lock_guard<std::mutex> lock(mutex());

        return state().bitmapRenders;
    }

private:
    struct State {
        int renders = 0;
        int bitmapRenders = 0;
        AbortableRenderInfoPtr abortNext;
    };

    static std::mutex& mutex()
    {
        static std::mutex m;

        return m;
    }

    static State& state()
    {
        static State s;

        return s;
    }
};

/**
 * @brief Writes directOutputProbeValue() over the render window of a fixed region of definition,
 * and records whether the image render() was handed carries a bitmap, which only the cached image
 * does.
 **/
class DirectOutputProbeBase
    : public NativeEffectBase {
public:
    explicit DirectOutputProbeBase(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        rod->x1 = 0.;
        rod->y1 = 0.;
        rod->x2 = kDirectOutputProbeWidth;
        rod->y2 = kDirectOutputProbeHeight;

        return eStatusOK;
    }

    virtual void addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImageLayerDesc>* comps) OVERRIDE FINAL
    {
        comps->push_back(ImageLayerDesc::getRGBAComponents());
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
    }

private:
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (args.outputLayers.empty() || !args.outputLayers.front().second) {
            return eStatusFailed;
        }
        const ImagePtr& image = args.outputLayers.front().second;
        const AbortableRenderInfoPtr abortInfo = DirectOutputProbeLog::noteRender(image->usesBitMap());

        if (image->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }
        const int numChannels = (int)image->getComponentsCount();
        {
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = (float*)access.pixelAt(x, y);
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    for (int c = 0; c < numChannels; ++c) {
                        pixel[c] = abortInfo ? kDirectOutputProbeAbortedValue : directOutputProbeValue(x, y, c);
                    }
                }
            }
        }
        if (abortInfo) {
            abortInfo->setAborted();
        }

        return eStatusOK;
    }
};

class DirectOutputProbe
    : public DirectOutputProbeBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DirectOutputProbe(n);
    }

    explicit DirectOutputProbe(NodePtr n)
        : DirectOutputProbeBase(n)
    {
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kDirectOutputProbePluginID;
        desc.label = "Test Direct Output Probe";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
        desc.outputKind = eDataKindImage;

        return desc;
    }
};

class DirectOutputProbeViaTemporary
    : public DirectOutputProbeBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DirectOutputProbeViaTemporary(n);
    }

    explicit DirectOutputProbeViaTemporary(NodePtr n)
        : DirectOutputProbeBase(n)
    {
    }

    virtual bool rendersOnlyInsideRenderWindow() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kDirectOutputProbeViaTemporaryPluginID;
        desc.label = "Test Direct Output Probe Via Temporary";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
        desc.outputKind = eDataKindImage;

        return desc;
    }
};

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kRenderTimeoutSeconds = 120;

template <typename PLUGIN>
void
registerDirectOutputPlugin()
{
    EffectInstancePtr node(PLUGIN::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&PLUGIN::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), so the label-without-suffix the script name is made
    // from was never assigned.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));

    std::list<PluginActionShortcut> shortcuts;
    node->getPluginShortcuts(&shortcuts);
    p->setShorcuts(shortcuts);
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

void
registerDirectOutputPluginsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerDirectOutputPlugin<DirectOutputProbe>();
        registerDirectOutputPlugin<DirectOutputProbeViaTemporary>();
    });
}

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
setForceCaching(const NodePtr& node,
                bool enabled)
{
    KnobBool* knob = node ? dynamic_cast<KnobBool*>(node->getKnobByName("forceCaching").get()) : NULL;

    EXPECT_TRUE(knob != NULL) << "the node has no forceCaching knob";
    if (!knob) {
        return false;
    }
    knob->setValue(enabled);

    return node->isForceCachingEnabled() == enabled;
}

// The caches are emptied on entry and on exit, so every render under it reaches render() and no
// cached image outlives the test.
class LegacyModeGuard {
public:
    LegacyModeGuard()
        : _saved(appPTR->getRenderSchedulerMode())
    {
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeLegacy);
        appPTR->clearAllCaches();
    }

    ~LegacyModeGuard()
    {
        appPTR->setRenderSchedulerMode(_saved);
        appPTR->clearAllCaches();
    }

    LegacyModeGuard(const LegacyModeGuard&) = delete;
    LegacyModeGuard& operator=(const LegacyModeGuard&) = delete;

private:
    RenderSchedulerModeEnum _saved;
};

struct CachedRender {
    EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
    ImagePtr image;
    std::string error;
};

// Renders the RGBA plane of `node` over `roi` at mipmap 0 through renderRoI with the cache in use,
// on the calling thread. Uses no gtest assertion, so worker threads may call it.
CachedRender
renderCached(const NodePtr& node,
             const RectI& roi,
             const AbortableRenderInfoPtr& abortInfo)
{
    CachedRender result;
    ParallelRenderArgsSetter frameArgs(kTime,
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
                                       RenderStatsPtr());
    EffectInstancePtr effect = node->getEffectInstance();
    const RectD canonicalWindow = roi.toCanonical_noClipping(0, effect->getAspectRatio(-1));
    FrameRequestMap request;

    if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, canonicalWindow, node, request) == eStatusFailed) {
        result.error = "request pass failed";

        return result;
    }
    frameArgs.updateNodesRequest(request);

    std::list<ImageLayerDesc> components;
    components.push_back(ImageLayerDesc::getRGBAComponents());
    EffectInstance::RenderRoIArgs args(kTime,
                                       RenderScale::fromMipmapLevel(0),
                                       0 /*mipmapLevel*/,
                                       ViewIdx(0),
                                       false /*byPassCache*/,
                                       roi,
                                       RectD(),
                                       components,
                                       eImageBitDepthFloat,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       kTime);
    std::map<ImageLayerDesc, ImagePtr> planes;
    // renderRoI reports a failed render action by throwing rather than through its return code.
    try {
        result.code = effect->renderRoI(args, &planes);
    } catch (const std::exception& e) {
        result.code = EffectInstance::eRenderRoIRetCodeFailed;
        result.error = std::string("renderRoI threw: ") + e.what();

        return result;
    }
    if (result.code != EffectInstance::eRenderRoIRetCodeOk) {
        result.error = "renderRoI did not complete";

        return result;
    }
    std::map<ImageLayerDesc, ImagePtr>::const_iterator rgba = planes.find(ImageLayerDesc::getRGBAComponents());
    if ((rgba == planes.end()) || !rgba->second) {
        result.code = EffectInstance::eRenderRoIRetCodeFailed;
        result.error = "the render produced no RGBA plane";

        return result;
    }
    result.image = rgba->second;
    if (result.image->getBitDepth() != eImageBitDepthFloat) {
        result.code = EffectInstance::eRenderRoIRetCodeFailed;
        result.error = "the render did not produce a float image";
    }

    return result;
} // renderCached

CachedRender
renderCachedOnWorker(const NodePtr& node,
                     const RectI& roi,
                     const AbortableRenderInfoPtr& abortInfo)
{
    CachedRender result = renderCached(node, roi, abortInfo);

    appPTR->getAppTLS()->cleanupTLSForThread();

    return result;
}

void
waitOrDie(std::future<CachedRender>& future,
          const std::string& what)
{
    if (future.wait_for(std::chrono::seconds(kRenderTimeoutSeconds)) != std::future_status::ready) {
        std::cerr << "[ FATAL ] " << what << " did not finish within " << kRenderTimeoutSeconds << " s: the concurrent cached renders deadlocked" << std::endl;
        // The future of std::async blocks in its destructor until the render returns, which a
        // deadlocked render never does, so failing the test normally would hang the binary.
        std::abort();
    }
}

// Row-major over window, interleaved in the image's channel order.
bool
readWindow(const ImagePtr& image,
           const RectI& window,
           std::vector<float>* out,
           std::string* error)
{
    if (!image->getBounds().contains(window)) {
        const RectI& b = image->getBounds();
        std::ostringstream ss;
        ss << "image (" << b.x1 << "," << b.y1 << ")-(" << b.x2 << "," << b.y2 << ") does not cover the window ("
           << window.x1 << "," << window.y1 << ")-(" << window.x2 << "," << window.y2 << ")";
        *error = ss.str();

        return false;
    }
    const std::size_t rowFloats = static_cast<std::size_t>(window.width()) * image->getComponentsCount();
    out->assign(rowFloats * window.height(), 0.f);
    Image::ReadAccess access = image->getReadRights();
    for (int y = window.y1; y < window.y2; ++y) {
        const float* row = reinterpret_cast<const float*>(access.pixelAt(window.x1, y));
        if (!row) {
            *error = "no pixel data at the start of row " + std::to_string(y);

            return false;
        }
        std::memcpy(&(*out)[static_cast<std::size_t>(y - window.y1) * rowFloats], row, rowFloats * sizeof(float));
    }

    return true;
}

// Empty when the two are identical bit for bit, otherwise where they first differ.
std::string
firstDifference(const std::vector<float>& expected,
                const std::vector<float>& actual,
                const RectI& window,
                std::size_t nComps)
{
    if (expected.size() != actual.size()) {
        return "sizes differ: expected " + std::to_string(expected.size()) + " floats, got " + std::to_string(actual.size());
    }
    if (std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(float)) == 0) {
        return std::string();
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (std::memcmp(&expected[i], &actual[i], sizeof(float)) != 0) {
            const std::size_t pixel = i / nComps;
            std::ostringstream ss;
            ss.precision(9);
            ss << "pixel (" << window.x1 + static_cast<int>(pixel % window.width()) << "," << window.y1 + static_cast<int>(pixel / window.width())
               << ") channel " << (i % nComps) << ": expected " << expected[i] << ", got " << actual[i];

            return ss.str();
        }
    }

    return std::string();
}

bool
bitmapMarksWholeWindow(const ImagePtr& image,
                       const RectI& window)
{
    std::list<RectI> rest;

    image->getRestToRender(window, rest);

    return rest.empty();
}

void
expectCachedImage(const CachedRender& render,
                  const RectI& window,
                  const std::vector<float>& expected,
                  const std::string& what)
{
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, render.code) << what << ": " << render.error;
    ASSERT_TRUE(bool(render.image)) << what;

    std::vector<float> actual;
    std::string error;
    ASSERT_TRUE(readWindow(render.image, window, &actual, &error)) << what << ": " << error;
    const std::string difference = firstDifference(expected, actual, window, render.image->getComponentsCount());
    EXPECT_TRUE(difference.empty()) << what << ": " << difference;

    EXPECT_TRUE(render.image->usesBitMap()) << what << ": the forced-cached render did not return the cached image";
    if (render.image->usesBitMap()) {
        EXPECT_TRUE(bitmapMarksWholeWindow(render.image, window)) << what << ": the cached image is not marked rendered over the whole window";
    }
}

std::vector<float>
probePixels(const RectI& window,
            int nComps)
{
    std::vector<float> pixels;

    pixels.reserve(static_cast<std::size_t>(window.width()) * window.height() * nComps);
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            for (int c = 0; c < nComps; ++c) {
                pixels.push_back(directOutputProbeValue(x, y, c));
            }
        }
    }

    return pixels;
}

} // namespace

class NativeDirectOutputTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerDirectOutputPluginsOnce();
        BaseTest::SetUp();
        DirectOutputProbeLog::reset();
    }

    // A project whose format is the parity source's 64x48, with the source at the origin, so a
    // render window inside it holds non-uniform pixels.
    NodePtr createSource()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeDirectOutput64x48", 1.));

        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    NodePtr createNativeOn(const NodePtr& source,
                           const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(isNative(node)) << pluginID;
        if (source && node) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    // The reference is an uncached render, where the plug-in writes into a bitmap-less image, and
    // the cached renders then go through the cached image. Rendering the grown window second makes
    // the cached image grow and render only what the first window did not cover.
    void expectCachedRendersMatchUncached(const NodePtr& node,
                                          const std::string& caseName)
    {
        const RectI window(5, 3, 52, 40);
        const RectI grown(2, 1, 61, 46);
        std::list<ImageLayerDesc> layers;
        layers.push_back(ImageLayerDesc::getRGBAComponents());

        ASSERT_TRUE(setForceCaching(node, false)) << caseName;
        std::vector<RenderedPlane> reference;
        std::vector<RenderedPlane> grownReference;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &reference, &error)) << caseName << ": " << error;
        ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, grown, layers, &grownReference, &error)) << caseName << ": " << error;
        ASSERT_EQ(1u, reference.size()) << caseName;
        ASSERT_EQ(1u, grownReference.size()) << caseName;

        ASSERT_TRUE(setForceCaching(node, true)) << caseName;
        {
            DisableUnreachableRAMPurging noPurge;
            LegacyModeGuard legacy;

            const CachedRender first = renderCached(node, window, AbortableRenderInfo::create(false, 0));
            expectCachedImage(first, window, reference[0].pixels, caseName + ", first window");
            const CachedRender second = renderCached(node, grown, AbortableRenderInfo::create(false, 0));
            expectCachedImage(second, grown, grownReference[0].pixels, caseName + ", grown window");
        }

        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        poolSizes.push_back(16);
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), 0, window, poolSizes);
        EXPECT_FALSE(m.any) << caseName << ": " << describe(m);

        EXPECT_TRUE(setForceCaching(node, false)) << caseName;
    }
};

TEST_F(NativeDirectOutputTest, CachedRenderHandsTheEffectTheCachedImage)
{
    NodePtr probe = createNode(QString::fromUtf8(kDirectOutputProbePluginID));
    ASSERT_TRUE(bool(probe));
    ASSERT_TRUE(setForceCaching(probe, true));

    const RectI window(7, 5, 44, 34);
    DisableUnreachableRAMPurging noPurge;
    LegacyModeGuard legacy;
    DirectOutputProbeLog::reset();

    const CachedRender render = renderCached(probe, window, AbortableRenderInfo::create(false, 0));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, render.code) << render.error;
    EXPECT_EQ(1, DirectOutputProbeLog::renders());
    EXPECT_EQ(1, DirectOutputProbeLog::bitmapRenders()) << "render() was handed a private temporary rather than the cached image";
    expectCachedImage(render, window, probePixels(window, (int)render.image->getComponentsCount()), "probe");
}

TEST_F(NativeDirectOutputTest, EffectThatMayWriteOutsideTheWindowGetsATemporary)
{
    NodePtr probe = createNode(QString::fromUtf8(kDirectOutputProbeViaTemporaryPluginID));
    ASSERT_TRUE(bool(probe));
    ASSERT_TRUE(setForceCaching(probe, true));

    const RectI window(7, 5, 44, 34);
    DisableUnreachableRAMPurging noPurge;
    LegacyModeGuard legacy;
    DirectOutputProbeLog::reset();

    const CachedRender render = renderCached(probe, window, AbortableRenderInfo::create(false, 0));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, render.code) << render.error;
    EXPECT_EQ(1, DirectOutputProbeLog::renders());
    EXPECT_EQ(0, DirectOutputProbeLog::bitmapRenders()) << "render() was handed the cached image despite not promising to stay inside the window";
    expectCachedImage(render, window, probePixels(window, (int)render.image->getComponentsCount()), "probe via temporary");
}

// The aborted render leaves its garbage in the cached image itself, so only a cleared bitmap
// keeps the next render from returning it.
TEST_F(NativeDirectOutputTest, AbortedDirectRenderIsRenderedAgain)
{
    NodePtr probe = createNode(QString::fromUtf8(kDirectOutputProbePluginID));
    ASSERT_TRUE(bool(probe));
    ASSERT_TRUE(setForceCaching(probe, true));

    const RectI window(7, 5, 44, 34);
    DisableUnreachableRAMPurging noPurge;
    LegacyModeGuard legacy;
    DirectOutputProbeLog::reset();

    AbortableRenderInfoPtr abortedRender = AbortableRenderInfo::create(true, 0);
    DirectOutputProbeLog::abortNextRender(abortedRender);
    const CachedRender aborted = renderCached(probe, window, abortedRender);
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, aborted.code) << "the render aborted from inside render() completed";
    EXPECT_TRUE(abortedRender->isAborted());
    ASSERT_EQ(1, DirectOutputProbeLog::renders());
    EXPECT_EQ(1, DirectOutputProbeLog::bitmapRenders());

    const CachedRender again = renderCached(probe, window, AbortableRenderInfo::create(false, 0));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, again.code) << again.error;
    EXPECT_EQ(2, DirectOutputProbeLog::renders()) << "the aborted window was served from the cache instead of being rendered again";
    EXPECT_EQ(2, DirectOutputProbeLog::bitmapRenders());
    expectCachedImage(again, window, probePixels(window, (int)again.image->getComponentsCount()), "after abort");
}

TEST_F(NativeDirectOutputTest, GradeCachedMatchesUncached)
{
    NodePtr source = createSource();
    ASSERT_TRUE(bool(source));
    NodePtr grade = createNativeOn(source, PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamMultiply, { 1.5, 0.75, 2., 1.25 }));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamOffset, { 0.1, -0.05, 0.2, 0. }));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamGamma, { 0.8 }));
    expectCachedRendersMatchUncached(grade, "grade");

    ASSERT_TRUE(setKnobValues(grade, kOfxMixParamName, { 0.5 }));
    expectCachedRendersMatchUncached(grade, "grade, mix 0.5");
}

TEST_F(NativeDirectOutputTest, InvertCachedMatchesUncached)
{
    NodePtr source = createSource();
    ASSERT_TRUE(bool(source));
    NodePtr invert = createNativeOn(source, PLUGINID_NATRON_INVERT);
    ASSERT_TRUE(bool(invert));
    expectCachedRendersMatchUncached(invert, "invert");

    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(invert->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    std::vector<std::string> names;
    names.push_back("R");
    names.push_back("B");
    channels->setChannels(0, names);
    expectCachedRendersMatchUncached(invert, "invert, red and blue only");
}

TEST_F(NativeDirectOutputTest, BlurCachedMatchesUncached)
{
    NodePtr source = createSource();
    ASSERT_TRUE(bool(source));
    NodePtr blur = createNativeOn(source, PLUGINID_NATRON_BLUR);
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 9., 4. }));
    ASSERT_TRUE(setKnobValues(blur, kBlurParamCropToFormat, { 0. }));
    expectCachedRendersMatchUncached(blur, "blur");
}

TEST_F(NativeDirectOutputTest, CachedChainRendersTheSameInBothSchedulers)
{
    getApp()->getProject()->reset(false, true);
    getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, 77, 53, "nativeDirectOutputChainFormat", 1.));

    NodePtr checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD));
    ASSERT_TRUE(bool(checker));
    NodePtr grade = createNativeOn(checker, PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamMultiply, { 1.5, 0.75, 2., 1. }));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamOffset, { 0.1, -0.05, 0.2, 0. }));
    NodePtr invert = createNativeOn(grade, PLUGINID_NATRON_INVERT);
    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(setForceCaching(grade, true));
    ASSERT_TRUE(setForceCaching(invert, true));

    NodePtr writer = createNativeWriter();
    ASSERT_TRUE(bool(writer));
    connectNodes(invert, writer, 0, true);

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    poolSizes.push_back(16);
    const RenderMismatch m = renderBothWays(writer, 1, 2, poolSizes);
    EXPECT_FALSE(m.any) << describe(m);
}

// Each thread renders into the same cached image, so each one's kernel writes while the other
// may be growing that image to fit its own window.
TEST_F(NativeDirectOutputTest, ConcurrentDisjointWindowsOfOneCachedNode)
{
    NodePtr source = createSource();
    ASSERT_TRUE(bool(source));
    NodePtr grade = createNativeOn(source, PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamMultiply, { 1.5, 0.75, 2., 1.25 }));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamGamma, { 0.8 }));

    const RectI left(3, 2, 31, 45);
    const RectI right(31, 2, 61, 45);
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> leftReference;
    std::vector<RenderedPlane> rightReference;
    std::string error;
    ASSERT_TRUE(setForceCaching(grade, false));
    ASSERT_TRUE(renderNodePlanesDirect(grade, kTime, ViewIdx(0), 0, left, layers, &leftReference, &error)) << error;
    ASSERT_TRUE(renderNodePlanesDirect(grade, kTime, ViewIdx(0), 0, right, layers, &rightReference, &error)) << error;
    ASSERT_EQ(1u, leftReference.size());
    ASSERT_EQ(1u, rightReference.size());

    ASSERT_TRUE(setForceCaching(grade, true));
    DisableUnreachableRAMPurging noPurge;
    LegacyModeGuard legacy;

    for (int iteration = 0; iteration < 8; ++iteration) {
        appPTR->clearAllCaches();
        const AbortableRenderInfoPtr leftInfo = AbortableRenderInfo::create(false, 0);
        const AbortableRenderInfoPtr rightInfo = AbortableRenderInfo::create(false, 0);

        std::future<CachedRender> leftFuture = std::async(std::launch::async, renderCachedOnWorker, grade, left, leftInfo);
        std::future<CachedRender> rightFuture = std::async(std::launch::async, renderCachedOnWorker, grade, right, rightInfo);
        waitOrDie(leftFuture, "the left window render");
        waitOrDie(rightFuture, "the right window render");
        const CachedRender leftRender = leftFuture.get();
        const CachedRender rightRender = rightFuture.get();

        const std::string suffix = ", iteration " + std::to_string(iteration);
        expectCachedImage(leftRender, left, leftReference[0].pixels, "left window" + suffix);
        expectCachedImage(rightRender, right, rightReference[0].pixels, "right window" + suffix);
        if (HasFatalFailure() || HasNonfatalFailure()) {
            break;
        }
    }
}
