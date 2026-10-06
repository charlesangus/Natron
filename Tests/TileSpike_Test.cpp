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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QThread>
#include <QThreadPool>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OpenMPThreads.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardPluginID = "net.sf.openfx.CheckerBoardPlugin";

const int kWidth = 1920;
const int kHeight = 1080;
const int kFirstKeyFrame = 1;
const int kLastKeyFrame = 5;
const double kTime = 3.;
const int kWarmRuns = 1;
const int kTimedRuns = 5;
const int kCostRuns = 31;
const int kCostTallRows = 64;

const std::vector<int>&
chainLengths()
{
    static const std::vector<int> lengths { 30, 100 };

    return lengths;
}

const std::vector<int>&
stripHeights()
{
    static const std::vector<int> heights { 8, 16, 32, 64, 128, 270 };

    return heights;
}

bool
spikeEnabled(const char* testName)
{
    const char* enabled = std::getenv("NATRON_TILE_SPIKE");
    if (enabled && (std::strcmp(enabled, "1") == 0)) {
        return true;
    }
    std::cout << "[  SKIPPED ] TileSpike." << testName
              << ": a timing spike; run with NATRON_TILE_SPIKE=1, preferably on the release Tests binary on a quiet host"
              << std::endl;

    return false;
}

double
median(std::vector<double> samples)
{
    if (samples.empty()) {
        return 0.;
    }
    std::sort(samples.begin(), samples.end());

    return samples[samples.size() / 2];
}

double
millisecondsSince(const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

ImagePtr
rgbaPlane(const std::map<ImageLayerDesc, ImagePtr>& layers)
{
    if (layers.empty()) {
        return ImagePtr();
    }
    std::map<ImageLayerDesc, ImagePtr>::const_iterator rgba = layers.find(ImageLayerDesc::getRGBAComponents());

    return (rgba != layers.end()) ? rgba->second : layers.begin()->second;
}

// Copies the rows of window from image into frame, a row-major RGBA float buffer covering frameRect.
bool
copyRows(const ImagePtr& image,
         const RectI& window,
         const RectI& frameRect,
         std::vector<float>* frame,
         std::string* error)
{
    if (!image || (image->getBitDepth() != eImageBitDepthFloat) || (image->getComponentsCount() != 4)) {
        *error = "the render did not produce an RGBA float image";

        return false;
    }
    if (!image->getBounds().contains(window)) {
        const RectI& b = image->getBounds();
        std::ostringstream ss;
        ss << "image (" << b.x1 << "," << b.y1 << ")-(" << b.x2 << "," << b.y2 << ") does not cover ("
           << window.x1 << "," << window.y1 << ")-(" << window.x2 << "," << window.y2 << ")";
        *error = ss.str();

        return false;
    }
    const std::size_t rowFloats = static_cast<std::size_t>(window.width()) * 4;
    const std::size_t frameRowFloats = static_cast<std::size_t>(frameRect.width()) * 4;
    Image::ReadAccess access = image->getReadRights();
    for (int y = window.y1; y < window.y2; ++y) {
        const float* row = reinterpret_cast<const float*>(access.pixelAt(window.x1, y));
        float* dst = &(*frame)[static_cast<std::size_t>(y - frameRect.y1) * frameRowFloats + static_cast<std::size_t>(window.x1 - frameRect.x1) * 4];
        std::memcpy(dst, row, rowFloats * sizeof(float));
    }

    return true;
}

std::string
firstDifference(const std::vector<float>& expected,
                const std::vector<float>& actual,
                const RectI& frameRect)
{
    if (expected.size() != actual.size()) {
        return "buffer sizes differ";
    }
    if (std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(float)) == 0) {
        return std::string();
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (std::memcmp(&expected[i], &actual[i], sizeof(float)) != 0) {
            const std::size_t pixel = i / 4;
            std::ostringstream ss;
            ss.precision(9);
            ss << "pixel (" << frameRect.x1 + static_cast<int>(pixel % frameRect.width()) << ","
               << frameRect.y1 + static_cast<int>(pixel / frameRect.width()) << ") channel " << (i % 4)
               << " whole=" << expected[i] << " strip=" << actual[i];

            return ss.str();
        }
    }

    return std::string();
}

std::list<ImageLayerDesc>
rgbaComponents()
{
    std::list<ImageLayerDesc> components;
    components.push_back(ImageLayerDesc::getRGBAComponents());

    return components;
}

ParallelRenderArgsSetter*
newFrameArgs(const NodePtr& tail,
             const AbortableRenderInfoPtr& abortInfo,
             const RenderStatsPtr& stats)
{
    return new ParallelRenderArgsSetter(kTime,
                                        ViewIdx(0),
                                        false /*isRenderUserInteraction*/,
                                        false /*isSequential*/,
                                        abortInfo,
                                        tail,
                                        0 /*textureIndex*/,
                                        tail->getApp()->getTimeLine().get(),
                                        NodePtr(),
                                        false /*isAnalysis*/,
                                        false /*draftMode*/,
                                        stats);
}

bool
regionOfDefinition(const NodePtr& tail,
                   RectI* pixelRoD,
                   std::string* error)
{
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    std::unique_ptr<ParallelRenderArgsSetter> frameArgs(newFrameArgs(tail, abortInfo, RenderStatsPtr()));
    EffectInstancePtr effect = tail->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    if (effect->getRegionOfDefinition_public(tail->getHashValue(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) {
        *error = "getRegionOfDefinition failed";

        return false;
    }
    *pixelRoD = rod.toPixelEnclosing(0, effect->getAspectRatio(-1));
    if (pixelRoD->isNull()) {
        *error = "empty region of definition";

        return false;
    }

    return true;
}

// The whole frame as a writer renders it in Task graph mode: request pass on this thread, then the frame's tasks on
// the global pool. pixels, when given, receives the tail's RGBA over rod after the timed part.
bool
renderWhole(const NodePtr& tail,
            const RectI& rod,
            double* milliseconds,
            std::vector<float>* pixels,
            int* tasksRun,
            std::string* error)
{
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    RenderStatsPtr stats = std::make_shared<RenderStats>(false);
    std::unique_ptr<ParallelRenderArgsSetter> frameArgs(newFrameArgs(tail, abortInfo, stats));
    EffectInstancePtr effect = tail->getEffectInstance();
    const RectD canonicalWindow = rod.toCanonical_noClipping(0, effect->getAspectRatio(-1));

    std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
    if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0, canonicalWindow, tail, *request) == eStatusFailed) {
        *error = "request pass failed";

        return false;
    }
    frameArgs->updateNodesRequest(*request);

    FrameRenderContextPtr context = FrameRenderContext::createFromSetter(*frameArgs, abortInfo, stats, kTime, ViewIdx(0));
    context->setRequest(request);
    FrameGraph graph = RenderScheduler::buildGraph(context, tail, kTime, ViewIdx(0), 0);
    if (graph.tasks.empty()) {
        *error = "the scheduler built no task";

        return false;
    }
    FrameFuturePtr future = appPTR->getRenderScheduler()->submit(context, std::move(graph), RenderScheduler::Priority::Background);
    if (future->wait() != EffectInstance::eRenderRoIRetCodeOk) {
        *error = "the scheduler did not complete the frame";

        return false;
    }
    const std::map<ImageLayerDesc, ImagePtr> layers = future->getRootPlanes();
    *milliseconds = millisecondsSince(start);
    *tasksRun = stats->getTasksRun();

    if (pixels) {
        pixels->assign(static_cast<std::size_t>(rod.width()) * rod.height() * 4, 0.f);

        return copyRows(rgbaPlane(layers), rod, rod, pixels, error);
    }

    return true;
}

// One strip as a tile task would pull it: its own frame args and a request pass over the strip alone, so every
// interior node's request RoI is the strip, then the tail's renderRoI over the strip. The thread budget stays at the
// caller's, so a budget of 1 keeps the host's frame threading and the multithread suite from splitting the strip.
bool
renderStrip(const NodePtr& tail,
            const RectI& strip,
            const RectI& frameRect,
            std::vector<float>* frame,
            std::string* error)
{
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    std::unique_ptr<ParallelRenderArgsSetter> frameArgs(newFrameArgs(tail, abortInfo, RenderStatsPtr()));
    EffectInstancePtr effect = tail->getEffectInstance();
    const RectD canonicalStrip = strip.toCanonical_noClipping(0, effect->getAspectRatio(-1));

    FrameRequestMap request;
    if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0, canonicalStrip, tail, request) == eStatusFailed) {
        *error = "request pass failed";

        return false;
    }
    frameArgs->updateNodesRequest(request);

    EffectInstance::RenderRoIArgs args(kTime,
                                       RenderScale::fromMipmapLevel(0),
                                       0 /*mipmapLevel*/,
                                       ViewIdx(0),
                                       true /*byPassCache*/,
                                       strip,
                                       RectD(),
                                       rgbaComponents(),
                                       eImageBitDepthFloat,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       kTime);
    std::map<ImageLayerDesc, ImagePtr> layers;
    if (effect->renderRoI(args, &layers) != EffectInstance::eRenderRoIRetCodeOk) {
        *error = "renderRoI failed";

        return false;
    }

    return copyRows(rgbaPlane(layers), strip, frameRect, frame, error);
}

std::vector<RectI>
splitIntoStrips(const RectI& rod,
                int rows)
{
    std::vector<RectI> strips;

    for (int y = rod.y1; y < rod.y2; y += rows) {
        strips.push_back(RectI(rod.x1, y, rod.x2, std::min(y + rows, rod.y2)));
    }

    return strips;
}

bool
renderStrips(const NodePtr& tail,
             const RectI& rod,
             const std::vector<RectI>& strips,
             double* milliseconds,
             std::vector<float>* frame,
             std::string* error)
{
    std::mutex errorMutex;
    std::string firstError;
    QThread* caller = QThread::currentThread();
    const int maxThreads = QThreadPool::globalInstance()->maxThreadCount();

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    parallelForOnGlobalPool(static_cast<int>(strips.size()), maxThreads, [&](int index) {
        {
            AppTLS::ThreadBudgetScope budget(1);
            OpenMPThreadsScope openMPThreads(1);
            std::string stripError;
            if (!renderStrip(tail, strips[index], rod, frame, &stripError)) {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (firstError.empty()) {
                    std::ostringstream ss;
                    ss << "strip " << index << " (rows " << strips[index].y1 << "-" << strips[index].y2 << "): " << stripError;
                    firstError = ss.str();
                }
            }
        }
        // A pool thread keeps the TLS of every node it rendered until it is cleaned; outside a scheduler task no
        // FrameContextScope does that, and the test thread's own TLS must survive.
        if (QThread::currentThread() != caller) {
            appPTR->getAppTLS()->cleanupTLSForThread();
        }
    });
    *milliseconds = millisecondsSince(start);

    if (!firstError.empty()) {
        *error = firstError;

        return false;
    }

    return true;
}

// Times only the renderRoI call of node over window, on this thread with a budget of 1.
bool
timeSingleCall(const NodePtr& node,
               const RectI& window,
               double* milliseconds,
               std::string* error)
{
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    std::unique_ptr<ParallelRenderArgsSetter> frameArgs(newFrameArgs(node, abortInfo, RenderStatsPtr()));
    EffectInstancePtr effect = node->getEffectInstance();
    const RectD canonicalWindow = window.toCanonical_noClipping(0, effect->getAspectRatio(-1));

    FrameRequestMap request;
    if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0, canonicalWindow, node, request) == eStatusFailed) {
        *error = "request pass failed";

        return false;
    }
    frameArgs->updateNodesRequest(request);

    EffectInstance::RenderRoIArgs args(kTime,
                                       RenderScale::fromMipmapLevel(0),
                                       0 /*mipmapLevel*/,
                                       ViewIdx(0),
                                       true /*byPassCache*/,
                                       window,
                                       RectD(),
                                       rgbaComponents(),
                                       eImageBitDepthFloat,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       kTime);
    std::map<ImageLayerDesc, ImagePtr> layers;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const EffectInstance::RenderRoIRetCode code = effect->renderRoI(args, &layers);
    *milliseconds = millisecondsSince(start);
    if (code != EffectInstance::eRenderRoIRetCodeOk) {
        *error = "renderRoI failed";

        return false;
    }
    if (!rgbaPlane(layers)) {
        *error = "renderRoI produced no plane";

        return false;
    }

    return true;
}

struct LinearCost {
    double fixedMs = 0.;
    double perPixelNs = 0.;
};

LinearCost
fitCost(double shortMs,
        double tallMs,
        int width,
        int shortRows,
        int tallRows)
{
    LinearCost cost;
    const double shortArea = static_cast<double>(width) * shortRows;
    const double tallArea = static_cast<double>(width) * tallRows;

    cost.perPixelNs = (tallMs - shortMs) / (tallArea - shortArea) * 1e6;
    cost.fixedMs = shortMs - cost.perPixelNs * 1e-6 * shortArea;

    return cost;
}

} // namespace

class TileSpike
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        resetProject();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    void resetProject()
    {
        getApp()->getProject()->reset(false, true);
        Format format(0, 0, kWidth, kHeight, "tileSpikeHD", 1.);
        getApp()->getProject()->setOrAddProjectFormat(format);
    }

    // The graph_bench chain: a CheckerBoard over the project extent whose color0 is keyed on every frame, so that no
    // node downstream is frame-invariant and cached, followed by n Grades.
    NodePtr buildChain(int n,
                       NodePtr* source = 0)
    {
        resetProject();
        NodePtr checker = createNode(QString::fromUtf8(kCheckerBoardPluginID));
        EXPECT_TRUE(bool(checker));
        if (!checker) {
            return NodePtr();
        }
        KnobChoice* extent = dynamic_cast<KnobChoice*>(checker->getKnobByName("extent").get());
        EXPECT_TRUE(extent != NULL);
        if (extent) {
            extent->setValueFromID("project", 0);
        }
        KnobColor* color0 = dynamic_cast<KnobColor*>(checker->getKnobByName("color0").get());
        EXPECT_TRUE(color0 != NULL);
        if (color0) {
            for (int f = kFirstKeyFrame; f <= kLastKeyFrame; ++f) {
                color0->setValueAtTime(f, 0.1 + 0.01 * f, ViewSpec::all(), 0);
            }
        }
        if (source) {
            *source = checker;
        }

        NodePtr upstream = checker;
        for (int i = 0; i < n; ++i) {
            NodePtr grade = createNode(QString::fromUtf8(PLUGINID_OFX_GRADE));
            EXPECT_TRUE(bool(grade));
            if (!grade) {
                return NodePtr();
            }
            KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
            EXPECT_TRUE(multiply != NULL);
            if (multiply) {
                multiply->setValues(1.0 + 0.0001 * (i % 7 + 1), 1.0, 1.0, 1.0, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
            }
            connectNodes(upstream, grade, 0, true);
            upstream = grade;
        }

        return upstream;
    }
};

TEST_F(TileSpike, StripPullAgainstWholeFrame)
{
    if (!spikeEnabled("StripPullAgainstWholeFrame")) {
        return;
    }

    std::ostringstream table;
    table << std::fixed << std::setprecision(1);
    table << "\nTile spike: CheckerBoard -> N Grades, " << kWidth << "x" << kHeight << " RGBA float, frame " << kTime
          << ", pool " << QThreadPool::globalInstance()->maxThreadCount() << " threads, hardware "
          << std::thread::hardware_concurrency() << ", median of " << kTimedRuns << " warm runs\n";
    table << std::left << std::setw(6) << "N" << std::setw(14) << "mode" << std::setw(8) << "strips"
          << std::right << std::setw(12) << "ms" << std::setw(10) << "speed-up" << std::setw(8) << "ms/node"
          << "  pixels\n";

    for (std::size_t c = 0; c < chainLengths().size(); ++c) {
        const int n = chainLengths()[c];
        NodePtr tail = buildChain(n);
        ASSERT_TRUE(bool(tail)) << "N=" << n;

        RectI rod;
        std::string error;
        ASSERT_TRUE(regionOfDefinition(tail, &rod, &error)) << "N=" << n << ": " << error;
        EXPECT_EQ(kWidth, rod.width()) << "N=" << n;
        EXPECT_EQ(kHeight, rod.height()) << "N=" << n;

        std::vector<float> wholePixels;
        std::vector<double> wholeTimes;
        for (int run = 0; run < kWarmRuns + kTimedRuns; ++run) {
            double ms = 0.;
            int tasksRun = 0;
            ASSERT_TRUE(renderWhole(tail, rod, &ms, (run == 0) ? &wholePixels : 0, &tasksRun, &error)) << "N=" << n << " whole: " << error;
            EXPECT_GT(tasksRun, 0) << "N=" << n << ": the whole-frame render ran no task through the scheduler";
            if (run >= kWarmRuns) {
                wholeTimes.push_back(ms);
            }
        }
        const double wholeMs = median(wholeTimes);
        table << std::left << std::setw(6) << n << std::setw(14) << "whole" << std::setw(8) << 1
              << std::right << std::setw(12) << wholeMs << std::setw(10) << "1.00x" << std::setw(8) << wholeMs / n
              << "  reference\n";

        double bestMs = 0.;
        int bestRows = 0;
        for (std::size_t h = 0; h < stripHeights().size(); ++h) {
            const int rows = stripHeights()[h];
            const std::vector<RectI> strips = splitIntoStrips(rod, rows);
            std::vector<float> frame(wholePixels.size(), 0.f);
            std::vector<double> times;
            for (int run = 0; run < kWarmRuns + kTimedRuns; ++run) {
                double ms = 0.;
                ASSERT_TRUE(renderStrips(tail, rod, strips, &ms, &frame, &error)) << "N=" << n << " rows=" << rows << ": " << error;
                if (run >= kWarmRuns) {
                    times.push_back(ms);
                }
            }
            const double stripMs = median(times);
            const std::string difference = firstDifference(wholePixels, frame, rod);
            EXPECT_TRUE(difference.empty()) << "N=" << n << " rows=" << rows << ": strip pull differs from the whole frame at " << difference;
            if ((bestRows == 0) || (stripMs < bestMs)) {
                bestMs = stripMs;
                bestRows = rows;
            }

            std::ostringstream mode;
            mode << "strip " << rows;
            std::ostringstream speedUp;
            speedUp << std::fixed << std::setprecision(2) << (stripMs > 0. ? wholeMs / stripMs : 0.) << "x";
            table << std::left << std::setw(6) << n << std::setw(14) << mode.str() << std::setw(8) << strips.size()
                  << std::right << std::setw(12) << stripMs << std::setw(10) << speedUp.str() << std::setw(8) << stripMs / n
                  << "  " << (difference.empty() ? "identical" : "DIFFER: " + difference) << "\n";
        }
        std::ostringstream best;
        best << std::fixed << std::setprecision(2) << (bestMs > 0. ? wholeMs / bestMs : 0.);
        table << "N=" << n << " best: " << bestRows << " rows, " << bestMs << " ms, speed-up " << best.str() << "x\n";
    }

    std::cout << table.str() << std::endl;
}

TEST_F(TileSpike, PerCallFixedCost)
{
    if (!spikeEnabled("PerCallFixedCost")) {
        return;
    }

    NodePtr checker;
    NodePtr grade = buildChain(1, &checker);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(checker));

    RectI rod;
    std::string error;
    ASSERT_TRUE(regionOfDefinition(grade, &rod, &error)) << error;
    ASSERT_GE(rod.height(), kCostTallRows);

    const int y1 = rod.y1 + (rod.height() - kCostTallRows) / 2;
    const RectI shortStrip(rod.x1, y1, rod.x2, y1 + 1);
    const RectI tallStrip(rod.x1, y1, rod.x2, y1 + kCostTallRows);

    AppTLS::ThreadBudgetScope budget(1);
    OpenMPThreadsScope openMPThreads(1);

    struct Measured {
        const char* label;
        NodePtr node;
        double shortMs;
        double tallMs;
    };
    std::vector<Measured> measured;
    measured.push_back(Measured { "CheckerBoard", checker, 0., 0. });
    measured.push_back(Measured { "Grade+input", grade, 0., 0. });

    for (std::size_t m = 0; m < measured.size(); ++m) {
        for (int pass = 0; pass < 2; ++pass) {
            const RectI& window = (pass == 0) ? shortStrip : tallStrip;
            std::vector<double> times;
            for (int run = 0; run < kWarmRuns + kCostRuns; ++run) {
                double ms = 0.;
                ASSERT_TRUE(timeSingleCall(measured[m].node, window, &ms, &error)) << measured[m].label << " rows=" << window.height() << ": " << error;
                if (run >= kWarmRuns) {
                    times.push_back(ms);
                }
            }
            ((pass == 0) ? measured[m].shortMs : measured[m].tallMs) = median(times);
        }
    }

    const LinearCost checkerCost = fitCost(measured[0].shortMs, measured[0].tallMs, rod.width(), 1, kCostTallRows);
    const LinearCost chainCost = fitCost(measured[1].shortMs, measured[1].tallMs, rod.width(), 1, kCostTallRows);
    const LinearCost gradeCost = fitCost(measured[1].shortMs - measured[0].shortMs, measured[1].tallMs - measured[0].tallMs, rod.width(), 1, kCostTallRows);

    std::ostringstream table;
    table << std::fixed << std::setprecision(4);
    table << "\nPer-call cost, one thread, renderRoI only, " << rod.width() << "-pixel rows, median of " << kCostRuns << " runs\n";
    table << std::left << std::setw(14) << "call" << std::right << std::setw(12) << "1 row ms" << std::setw(12) << "64 rows ms"
          << std::setw(12) << "a (ms)" << std::setw(14) << "b (ns/px)" << std::setw(13) << "HD est. ms" << "\n";
    const double hdArea = static_cast<double>(kWidth) * kHeight;
    const LinearCost* costs[] = { &checkerCost, &chainCost, &gradeCost };
    const char* labels[] = { "CheckerBoard", "Grade+input", "Grade alone" };
    const double shortMs[] = { measured[0].shortMs, measured[1].shortMs, measured[1].shortMs - measured[0].shortMs };
    const double tallMs[] = { measured[0].tallMs, measured[1].tallMs, measured[1].tallMs - measured[0].tallMs };
    for (int i = 0; i < 3; ++i) {
        table << std::left << std::setw(14) << labels[i] << std::right << std::setw(12) << shortMs[i] << std::setw(12) << tallMs[i]
              << std::setw(12) << costs[i]->fixedMs << std::setw(14) << costs[i]->perPixelNs
              << std::setw(13) << costs[i]->fixedMs + costs[i]->perPixelNs * 1e-6 * hdArea << "\n";
    }
    table << "cost(area) ~= a + b * area; \"Grade alone\" subtracts the CheckerBoard's call from the Grade's, which pulls it\n";

    std::cout << table.str() << std::endl;

    EXPECT_GT(measured[1].tallMs, 0.);
}
