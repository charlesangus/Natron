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

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"
#include "HashSweepEdits.h"
#include "RenderBothWays.h"
#include "Serialization/SyntheticProjectGraphs.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/DeepImage.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Channel/AddLayers.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Deep/DeepCrop.h"
#include "Engine/Nodes/Deep/DeepFromImage.h"
#include "Engine/Nodes/Deep/DeepToImage.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/PrecompNode.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
using namespace NATRON_NAMESPACE::HashSweep;

// A render served from the caches must be indistinguishable from the same render with every cache
// empty: pixels of every plane, region of definition, metadata, and deep samples. Each scenario
// walks an edit sequence that could leave an entry keyed under the wrong state behind, renders
// through the caches after every edit, and then compares the last of those renders with a cold
// one. The cold render is the ground truth, so a mismatch is a hash gap. It is only ground truth
// when a second cold render, made on a mismatch, agrees with it: two that differ fail as a
// non-deterministic render rather than as a hash gap, since no cached render could match both.

namespace {

const int kSize = 64;

// Dealt round-robin like HashPluginSweep's shards, for the same reason: the plugin list is only
// known after the parameterised suites are expanded.
const int kSweepShardCount = 16;

// Every plugin is rendered, but only this many of its knob dimensions are edited, picked at an even
// stride over the editable ones, which keeps each shard well inside the per-case timeout.
const std::size_t kMaxEditsPerPlugin = 12;

std::string
modeName(RenderSchedulerModeEnum mode)
{
    return mode == eRenderSchedulerModeTaskGraph ? "Task graph" : "Legacy";
}

std::string
describeLayer(const ImageLayerDesc& layer)
{
    std::string out = layer.getLayerID() + "(";
    const std::vector<std::string>& channels = layer.getChannels();

    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (i) {
            out += ",";
        }
        out += channels[i];
    }

    return out + ")";
}

std::string
describeRect(const RectD& r)
{
    std::ostringstream ss;
    ss.precision(17);
    ss << "(" << r.x1 << "," << r.y1 << ")-(" << r.x2 << "," << r.y2 << ")";

    return ss.str();
}

std::string
describeRect(const RectI& r)
{
    std::ostringstream ss;
    ss << "(" << r.x1 << "," << r.y1 << ")-(" << r.x2 << "," << r.y2 << ")";

    return ss.str();
}

bool
sameBits(double a,
         double b)
{
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

bool
sameRect(const RectD& a,
         const RectD& b)
{
    return sameBits(a.x1, b.x1) && sameBits(a.y1, b.y1) && sameBits(a.x2, b.x2) && sameBits(a.y2, b.y2);
}

// What one render of one node showed, and the node's metadata read right after it.
struct OutputState {
    bool rendered = false;
    std::string error;
    std::vector<RenderedPlane> planes;
    DeepImagePtr deep;

    int rodStatus = 0;
    RectD rod;
    bool rodIsProjectFormat = false;
    RectI format;
    double par = 0.;
    double fps = 0.;
    int fielding = 0;
    bool continuous = false;
    bool frameVarying = false;
    std::string components;
    int nComps = 0;
    int bitDepth = 0;
    double firstFrame = 0.;
    double lastFrame = 0.;
    std::vector<std::string> presentLayers;
};

void
captureMetadata(const NodePtr& node,
                double time,
                ViewIdx view,
                OutputState* state)
{
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    // As in renderDirect(): only an output node can root a non-interactive render.
    const bool isRenderUserInteraction = !dynamic_cast<OutputEffectInstance*>(node->getEffectInstance().get());
    ParallelRenderArgsSetter frameRenderArgs(time,
                                             view,
                                             isRenderUserInteraction,
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
    const U64 hash = node->getHashValue();

    state->rodStatus = (int)effect->getRegionOfDefinition_public(hash, time, RenderScale::identity, view, &state->rod, &state->rodIsProjectFormat);
    state->format = effect->getTimeInvariantMetadata()->getOutputFormat();
    state->par = effect->getTimeInvariantMetadata()->getPixelAspectRatio(-1);
    state->fps = effect->getTimeInvariantMetadata()->getOutputFrameRate();
    state->fielding = (int)effect->getTimeInvariantMetadata()->getOutputFielding();
    state->continuous = effect->getTimeInvariantMetadata()->getIsContinuous();
    state->frameVarying = effect->getTimeInvariantMetadata()->getIsFrameVarying();
    ImageLayerDesc layer, paired;
    EffectInstance::getLayersFromMetadata(*effect->getTimeInvariantMetadata(), -1, &layer, &paired);
    state->components = describeLayer(layer) + " paired " + describeLayer(paired);
    state->nComps = EffectInstance::getNCompsFromMetadata(*effect->getTimeInvariantMetadata(), -1);
    state->bitDepth = (int)effect->getTimeInvariantMetadata()->getBitDepth(-1);
    effect->getFrameRange_public(hash, &state->firstFrame, &state->lastFrame);
    std::list<ImageLayerDesc> present;
    effect->getPresentLayers(time, view, -1, &present);
    state->presentLayers.clear();
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        state->presentLayers.push_back(describeLayer(*it));
    }
}

// The two renders a comparison is between, by the names its messages give them.
struct Sides {
    std::string first;
    std::string second;
};

const Sides kCachedAndCold = { "cached", "cold" };
const Sides kTwoColdRenders = { "first cold", "second cold" };

std::string
comparePlanes(const std::vector<RenderedPlane>& warm,
              const std::vector<RenderedPlane>& cold,
              const Sides& sides)
{
    if (warm.size() != cold.size()) {
        std::ostringstream ss;
        ss << "the " << sides.first << " render has " << warm.size() << " planes, the " << sides.second << " one " << cold.size();

        return ss.str();
    }
    for (std::size_t p = 0; p < warm.size(); ++p) {
        const RenderedPlane& a = warm[p];
        const RenderedPlane& b = cold[p];
        if (!(a.layer == b.layer) || (a.channels != b.channels)) {
            return "plane " + std::to_string(p) + " is " + describeLayer(a.layer) + " " + sides.first + " and " + describeLayer(b.layer) + " " + sides.second;
        }
        if (a.pixels.size() != b.pixels.size()) {
            return "plane " + a.layer.getLayerID() + " differs in size";
        }
        if (a.pixels.empty() || (std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size() * sizeof(float)) == 0)) {
            continue;
        }
        const std::size_t nComps = std::max<std::size_t>(1, a.channels.size());
        for (std::size_t i = 0; i < a.pixels.size(); ++i) {
            if (std::memcmp(&a.pixels[i], &b.pixels[i], sizeof(float)) != 0) {
                const std::size_t pixel = i / nComps;
                std::ostringstream ss;
                ss.precision(9);
                ss << "plane " << a.layer.getLayerID() << " pixel (" << a.window.x1 + (int)(pixel % a.window.width()) << ","
                   << a.window.y1 + (int)(pixel / a.window.width()) << ") channel "
                   << (a.channels.empty() ? std::string("?") : a.channels[i % nComps])
                   << ": " << sides.first << " " << a.pixels[i] << ", " << sides.second << " " << b.pixels[i];

                return ss.str();
            }
        }
    }

    return std::string();
}

std::string
compareDeep(const DeepImagePtr& warm,
            const DeepImagePtr& cold,
            const Sides& sides)
{
    if (!warm || !cold) {
        return (warm || cold) ? "only one render produced a deep image" : std::string();
    }
    if (!(warm->getBounds() == cold->getBounds())) {
        return "deep bounds " + describeRect(warm->getBounds()) + " " + sides.first + ", " + describeRect(cold->getBounds()) + " " + sides.second;
    }
    if (warm->isTidy() != cold->isTidy()) {
        return "deep tidiness differs";
    }
    const SampleTable& ta = warm->getSampleTable();
    const SampleTable& tb = cold->getSampleTable();
    if ((ta.getCounts() != tb.getCounts()) || (ta.getOffsets() != tb.getOffsets())) {
        return "deep sample counts differ";
    }
    const std::map<std::string, DeepChannelBuffer>& ca = warm->getChannels();
    const std::map<std::string, DeepChannelBuffer>& cb = cold->getChannels();
    if (ca.size() != cb.size()) {
        return "deep channel count differs";
    }
    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = ca.begin(); it != ca.end(); ++it) {
        std::map<std::string, DeepChannelBuffer>::const_iterator found = cb.find(it->first);
        if (found == cb.end()) {
            return "deep channel " + it->first + " is missing from the " + sides.second + " render";
        }
        if (it->second.size() != found->second.size()) {
            return "deep channel " + it->first + " differs in size";
        }
        if (!it->second.empty() && (std::memcmp(it->second.data(), found->second.data(), it->second.size() * sizeof(float)) != 0)) {
            for (std::size_t s = 0; s < it->second.size(); ++s) {
                if (std::memcmp(&it->second.data()[s], &found->second.data()[s], sizeof(float)) != 0) {
                    std::ostringstream ss;
                    ss.precision(9);
                    ss << "deep channel " << it->first << " sample " << s << ": " << sides.first << " " << it->second.data()[s] << ", " << sides.second << " " << found->second.data()[s];

                    return ss.str();
                }
            }
        }
    }

    return std::string();
}

template <typename T>
std::string
compareField(const char* name,
             const T& warm,
             const T& cold,
             const Sides& sides)
{
    if (warm == cold) {
        return std::string();
    }
    std::ostringstream ss;
    ss.precision(17);
    ss << name << ": " << sides.first << " " << warm << ", " << sides.second << " " << cold;

    return ss.str();
}

std::string
joinedNames(const std::vector<std::string>& names)
{
    std::string out;

    for (std::size_t i = 0; i < names.size(); ++i) {
        out += (i ? ", " : "") + names[i];
    }

    return out;
}

// Empty when the two states are bit-identical, else the first difference.
std::string
compareStates(const OutputState& warm,
              const OutputState& cold,
              const Sides& sides = kCachedAndCold)
{
    if (warm.rendered != cold.rendered) {
        return warm.rendered ? "the " + sides.second + " render failed (" + cold.error + ") where the " + sides.first + " one succeeded"
                             : "the " + sides.first + " render failed (" + warm.error + ") where the " + sides.second + " one succeeded";
    }
    std::string diff;
    if (warm.rendered) {
        diff = comparePlanes(warm.planes, cold.planes, sides);
        if (diff.empty()) {
            diff = compareDeep(warm.deep, cold.deep, sides);
        }
        if (!diff.empty()) {
            return diff;
        }
    }
    if ((warm.rodStatus != cold.rodStatus) || !sameRect(warm.rod, cold.rod) || (warm.rodIsProjectFormat != cold.rodIsProjectFormat)) {
        return "region of definition " + describeRect(warm.rod) + " " + sides.first + ", " + describeRect(cold.rod) + " " + sides.second
            + " (status " + std::to_string(warm.rodStatus) + "/" + std::to_string(cold.rodStatus) + ")";
    }
    if (!(warm.format == cold.format)) {
        return "output format " + describeRect(warm.format) + " " + sides.first + ", " + describeRect(cold.format) + " " + sides.second;
    }
    const std::string fields[] = {
        sameBits(warm.par, cold.par) ? std::string() : compareField("pixel aspect ratio", warm.par, cold.par, sides),
        sameBits(warm.fps, cold.fps) ? std::string() : compareField("frame rate", warm.fps, cold.fps, sides),
        compareField("fielding", warm.fielding, cold.fielding, sides),
        compareField("continuous", warm.continuous, cold.continuous, sides),
        compareField("frame varying", warm.frameVarying, cold.frameVarying, sides),
        compareField("components", warm.components, cold.components, sides),
        compareField("component count", warm.nComps, cold.nComps, sides),
        compareField("bit depth", warm.bitDepth, cold.bitDepth, sides),
        sameBits(warm.firstFrame, cold.firstFrame) ? std::string() : compareField("first frame", warm.firstFrame, cold.firstFrame, sides),
        sameBits(warm.lastFrame, cold.lastFrame) ? std::string() : compareField("last frame", warm.lastFrame, cold.lastFrame, sides),
        compareField("present layers", joinedNames(warm.presentLayers), joinedNames(cold.presentLayers), sides),
    };
    for (std::size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (!fields[i].empty()) {
            return fields[i];
        }
    }

    return std::string();
}

class SchedulerModeRestorer {
public:
    SchedulerModeRestorer()
        : _saved(appPTR->getRenderSchedulerMode())
    {
    }

    ~SchedulerModeRestorer()
    {
        appPTR->setRenderSchedulerMode(_saved);
    }

    SchedulerModeRestorer(const SchedulerModeRestorer&) = delete;
    SchedulerModeRestorer& operator=(const SchedulerModeRestorer&) = delete;

private:
    RenderSchedulerModeEnum _saved;
};

void
setMTime(const std::string& path,
         time_t seconds)
{
    struct timespec times[2];

    times[0].tv_sec = seconds;
    times[0].tv_nsec = 0;
    times[1].tv_sec = seconds;
    times[1].tv_nsec = 0;
    ASSERT_EQ(0, utimensat(AT_FDCWD, path.c_str(), times, 0));
}

bool
writeFlatExr(const std::string& path,
             int width,
             int height,
             float value)
{
    OIIO::ImageSpec spec(width, height, 4, OIIO::TypeDesc::FLOAT);

    spec.channelnames = { "R", "G", "B", "A" };
    spec.alpha_channel = 3;
    std::vector<float> pixels((std::size_t)width * height * 4, value);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        if ((i % 4) == 3) {
            pixels[i] = 1.f;
        } else {
            pixels[i] = value + 0.01f * (float)((i / 4) % (std::size_t)width);
        }
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

    return out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();
}

template <typename K>
std::shared_ptr<K>
knobOf(const NodePtr& node,
       const std::string& name)
{
    return node ? std::dynamic_pointer_cast<K>(node->getKnobByName(name)) : std::shared_ptr<K>();
}

} // namespace

class CacheEquivalenceTest
    : public SyntheticProjectGraphTest {
protected:
    struct Step {
        std::string label;
        std::function<void()> edit;
    };

    // What a scenario builder sets up in a fresh project: the node whose output is compared and the
    // edits walked before the comparison.
    struct Scenario {
        NodePtr output;
        bool deep = false;
        // In pixels at mipmapLevel, which a flat render, warm or cold, renders at.
        RectI roi = RectI(0, 0, kSize, kSize);
        unsigned mipmapLevel = 0;
        double time = 1.;
        // The planes a Legacy render asks for; empty means every layer the output presents.
        std::list<ImageLayerDesc> layers;
        std::vector<Step> steps;
    };

    typedef std::function<bool(Scenario*)> Builder;

    virtual void SetUp() OVERRIDE
    {
        SyntheticProjectGraphTest::SetUp();
        _baselineInstances = appPTR->getNumInstances();
        _mode = appPTR->getRenderSchedulerMode();
        resetProject();
        _cpuOnly.reset(new GpuPlacementOffGuard);
    }

    virtual void TearDown() OVERRIDE
    {
        _cpuOnly.reset();
        project()->reset(false, true);
        waitForSubAppsToQuit();
        appPTR->clearAllCaches();
        SyntheticProjectGraphTest::TearDown();
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    // Dropping a Precomp quits its sub-app through a queued signal; left pending, it would tear
    // down whichever app is top-level when it fires.
    void waitForSubAppsToQuit() const
    {
        QElapsedTimer timer;

        timer.start();
        while (appPTR->getNumInstances() > _baselineInstances && timer.elapsed() < 10000) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        EXPECT_EQ(_baselineInstances, appPTR->getNumInstances());
    }

    void resetProject()
    {
        project()->reset(false, true);
        waitForSubAppsToQuit();
        project()->setOrAddProjectFormat(Format(0, 0, kSize, kSize, "cacheEquivalence", 1.));
    }

    std::list<ImageLayerDesc> legacyLayersFor(const Scenario& s) const
    {
        if (!s.layers.empty()) {
            return s.layers;
        }
        std::list<ImageLayerDesc> present;
        s.output->getEffectInstance()->getPresentLayers(s.time, ViewIdx(0), -1, &present);
        if (present.empty()) {
            present.push_back(ImageLayerDesc::getRGBAComponents());
        }

        return present;
    }

    void renderDeep(const NodePtr& node,
                    double time,
                    const RectI& roi,
                    OutputState* state)
    {
        SchedulerModeRestorer restore;
        appPTR->setRenderSchedulerMode(_mode);
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstance::RenderDeepRoIArgs args(time,
                                               RenderScale::identity,
                                               0 /*mipmapLevel*/,
                                               ViewIdx(0),
                                               false /*byPassCache*/,
                                               roi,
                                               RectD(),
                                               0 /*caller*/,
                                               time);
        EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
        try {
            code = node->getEffectInstance()->renderDeepRoI(args, &state->deep);
        } catch (const std::exception& e) {
            state->error = std::string("renderDeepRoI threw: ") + e.what();
        }
        state->rendered = (code == EffectInstance::eRenderRoIRetCodeOk) && bool(state->deep);
        if (!state->rendered && state->error.empty()) {
            state->error = "renderDeepRoI failed";
        }
    }

    // `cold` empties every cache first and, for a flat Legacy render, bypasses them for the
    // whole tree.
    OutputState render(const Scenario& s,
                       const std::list<ImageLayerDesc>& legacyLayers,
                       bool cold)
    {
        OutputState state;

        if (cold) {
            appPTR->clearAllCaches();
        }
        if (s.deep) {
            renderDeep(s.output, s.time, s.roi, &state);
        } else {
            state.rendered = renderNodePlanesInMode(s.output, s.time, ViewIdx(0), s.mipmapLevel, s.roi, legacyLayers, _mode, cold, &state.planes, &state.error);
        }
        captureMetadata(s.output, s.time, ViewIdx(0), &state);

        return state;
    }

    struct Verdict {
        // Set when the cached render differs from a cold render that a second cold render agrees with.
        std::string hashGap;
        // Set when that second cold render differs, which leaves the cached render with no ground
        // truth to be judged against.
        std::string nondeterminism;
        bool coldRendered = false;
        std::string coldError;
    };

    // Cold renders already made in a scenario, by the content hash of the node they rendered.
    typedef std::map<U64, OutputState> ColdRenders;

    // The cached render must equal the cold one, and a state whose hash was rendered cold before
    // must equal that render too: equal hashes must render equal pixels. Only a difference costs a
    // second cold render, which tells a node that renders non-deterministically from a hash that
    // misses some of the state; the UHD bench comps cannot afford one for every state.
    Verdict judgeCachedRender(const Scenario& s,
                              const std::list<ImageLayerDesc>& layers,
                              ColdRenders* earlier = 0)
    {
        Verdict verdict;
        const OutputState cached = render(s, layers, false);
        const OutputState cold = render(s, layers, true);
        const U64 hash = s.output->getHashValue();
        const ColdRenders::const_iterator found = earlier ? earlier->find(hash) : ColdRenders::const_iterator();
        const bool seenBefore = earlier && (found != earlier->end());

        verdict.coldRendered = cold.rendered;
        verdict.coldError = cold.error;
        std::string diff;
        if (seenBefore) {
            const std::string acrossStates = compareStates(found->second, cold, kTwoColdRenders);
            if (!acrossStates.empty()) {
                diff = "an earlier state with the same hash renders differently cold: " + acrossStates;
            }
        }
        if (diff.empty()) {
            diff = compareStates(cached, cold);
        }
        if (diff.empty()) {
            if (earlier && !seenBefore) {
                (*earlier)[hash] = cold;
            }

            return verdict;
        }
        verdict.nondeterminism = compareStates(cold, render(s, layers, true), kTwoColdRenders);
        if (verdict.nondeterminism.empty()) {
            verdict.hashGap = diff;
        }

        return verdict;
    }

    static std::string nondeterminismMessage(const std::string& where,
                                             const std::string& diff)
    {
        return where + ": two cold renders of the same state differ, so the node renders non-deterministically"
                       " (unwritten output pixels, unseeded randomness or a thread-dependent reduction) and its cached render"
                       " cannot be checked: "
            + diff;
    }

    // Renders `node` through the caches in the mode under test, for steps that warm a node other
    // than the one compared.
    void warm(const NodePtr& node,
              double time = 1.)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        ignore_result(renderNodePlanesInMode(node, time, ViewIdx(0), 0, RectI(0, 0, kSize, kSize), std::list<ImageLayerDesc>(), _mode, false, &planes, &error));
    }

    // Builds the scenario afresh for every prefix of its steps, so that every intermediate state,
    // not only the last, is compared after the edits that led to it.
    void runScenario(const std::string& name,
                     const Builder& build)
    {
        runScenario(name, build, { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph });
    }

    void runScenario(const std::string& name,
                     const Builder& build,
                     const std::vector<RenderSchedulerModeEnum>& modes)
    {
        const RenderSchedulerModeEnum ambient = _mode;

        for (std::size_t m = 0; m < modes.size(); ++m) {
            _mode = modes[m];
            ColdRenders coldRenders;
            for (std::size_t prefix = 0;; ++prefix) {
                resetProject();
                Scenario s;
                if (!build(&s) || !s.output) {
                    ADD_FAILURE() << name << ": the scenario could not be built";
                    _mode = ambient;

                    return;
                }
                if (prefix > s.steps.size()) {
                    break;
                }
                appPTR->clearAllCaches();
                const std::list<ImageLayerDesc> baseLayers = legacyLayersFor(s);
                ignore_result(render(s, baseLayers, false));
                std::string path = "start";
                for (std::size_t i = 0; i < prefix; ++i) {
                    s.steps[i].edit();
                    path += " -> " + s.steps[i].label;
                    ignore_result(render(s, legacyLayersFor(s), false));
                }
                const std::list<ImageLayerDesc> layers = legacyLayersFor(s);
                const std::string where = name + " [" + modeName(_mode) + "] after " + path;
                const Verdict verdict = judgeCachedRender(s, layers, &coldRenders);
                EXPECT_TRUE(verdict.nondeterminism.empty()) << nondeterminismMessage(where, verdict.nondeterminism);
                EXPECT_TRUE(verdict.hashGap.empty()) << where << ": the cached render differs from a cold one, a hash gap: " << verdict.hashGap;
                if (!verdict.coldRendered) {
                    std::cout << "[cache equivalence] " << where << ": the cold renders failed: " << verdict.coldError << std::endl;
                }
            }
        }
        _mode = ambient;
        resetProject();
    }

    NodePtr createIn(const NodeCollectionPtr& parent,
                     const std::string& pluginID,
                     int majorVersion = -1)
    {
        CreateNodeArgs args(pluginID, parent);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, majorVersion, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);

        return getApp()->createNode(args);
    }

    NodePtr create(const std::string& pluginID,
                   int majorVersion = -1)
    {
        return createIn(project(), pluginID, majorVersion);
    }

    NodePtr createEmptyGroup(const NodeCollectionPtr& parent)
    {
        CreateNodeArgs args(PLUGINID_NATRON_GROUP, parent);
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    static NodeCollectionPtr collectionOf(const NodePtr& group)
    {
        return std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
    }

    NodePtr createChecker()
    {
        NodePtr checker = create(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        KnobDoublePtr boxSize = knobOf<KnobDouble>(checker, kCheckerBoardParamBoxSize);
        if (boxSize) {
            boxSize->setValue(8., ViewSpec::all(), 0);
            boxSize->setValue(8., ViewSpec::all(), 1);
        }

        return checker;
    }

    NodePtr createBlurOn(const NodePtr& input,
                         double size)
    {
        NodePtr blur = create(PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
        KnobDoublePtr knob = knobOf<KnobDouble>(blur, kBlurParamSize);
        if (!blur || !knob) {
            return NodePtr();
        }
        knob->setValue(size, ViewSpec::all(), 0);
        knob->setValue(size, ViewSpec::all(), 1);
        if (input && !blur->connectInput(input, 0)) {
            return NodePtr();
        }

        return blur;
    }

    static Step setDouble(const std::string& label,
                          const KnobDoublePtr& knob,
                          int dimension,
                          double value)
    {
        return Step { label, [knob, dimension, value]() { knob->setValue(value, ViewSpec::all(), dimension); } };
    }

    static Step setBlurSize(const std::string& label,
                            const NodePtr& blur,
                            double value)
    {
        KnobDoublePtr knob = knobOf<KnobDouble>(blur, kBlurParamSize);

        return Step { label, [knob, value]() {
                         knob->setValue(value, ViewSpec::all(), 0);
                         knob->setValue(value, ViewSpec::all(), 1);
                     } };
    }

    static Step setBool(const std::string& label,
                        const KnobBoolPtr& knob,
                        bool value)
    {
        return Step { label, [knob, value]() { knob->setValue(value); } };
    }

    // Builds Constant -> native Write in the test project, renders frames 1..3 with a different grey
    // on each, and saves it. Returns the saved .ntpz path, or an empty string on failure.
    std::string buildAndSavePrecompSubProject(const QTemporaryDir& dir)
    {
        resetProject();
        NodePtr constant = create(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
        NodePtr writer = createNativeWriter(QDir(dir.path()).filePath(QString::fromUtf8("frames.####.exr")).toStdString());
        KnobColorPtr color = knobOf<KnobColor>(constant, kConstantParamColor);
        KnobChoicePtr bitDepth = knobOf<KnobChoice>(writer, kNatronWriteParamBitDepth);
        KnobChoicePtr range = knobOf<KnobChoice>(writer, kNatronWriteParamFrameRange);
        KnobIntPtr first = knobOf<KnobInt>(writer, kNatronWriteParamFirstFrame);
        KnobIntPtr last = knobOf<KnobInt>(writer, kNatronWriteParamLastFrame);
        if (!constant || !writer || !color || !bitDepth || !range || !first || !last) {
            return std::string();
        }
        connectNodes(constant, writer, 0, true);
        const double greys[] = { 0.2, 0.5, 0.8 };
        for (int frame = 1; frame <= 3; ++frame) {
            for (int dim = 0; dim < 3; ++dim) {
                color->setValueAtTime(frame, greys[frame - 1], ViewSpec::all(), dim);
            }
            color->setValueAtTime(frame, 1., ViewSpec::all(), 3);
        }
        bitDepth->setValueFromID("32f", 0);
        range->setValueFromID(kNatronWriteFrameRangeManual, 0);
        first->setValue(1);
        last->setValue(3);

        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
        if (!effect) {
            return std::string();
        }
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, 1, 3, 1, false));
        getApp()->startWritersRendering(false, works);

        QString savedPath;
        if (!project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("sub.ntpz"), &savedPath)) {
            return std::string();
        }

        return savedPath.toStdString();
    }

    static NodePtr findByPluginID(const NodesList& nodes,
                                  const std::string& pluginID)
    {
        for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if (*it && ((*it)->getPluginID() == pluginID)) {
                return *it;
            }
        }

        return NodePtr();
    }

    // A reversible edit of the first knob dimension of `node` the sweep's perturbation can change,
    // as an apply step and a revert step.
    static bool appendFirstKnobToggle(const NodePtr& node,
                                      std::vector<Step>* steps)
    {
        const KnobsVec knobs = node->getKnobs();

        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            const KnobIPtr& knob = *it;
            if (!knob || !knob->isRenderHashed() || !staticValueSkipReason(knob, 0).empty()) {
                continue;
            }
            std::shared_ptr<KnobEdit> edit = std::make_shared<KnobEdit>();
            bool unsupported = false;
            if (!makeEdit(knob, 0, edit.get(), &unsupported).empty() || unsupported) {
                continue;
            }
            const std::string where = node->getScriptName() + "." + knob->getName();
            steps->push_back(Step { where + " " + edit->what, [edit]() { ignore_result(edit->apply()); } });
            steps->push_back(Step { where + " reverted", [edit]() { edit->revert(); } });

            return true;
        }

        return false;
    }

    RenderSchedulerModeEnum _mode = eRenderSchedulerModeLegacy;
    int _baselineInstances = 0;
    DisableUnreachableRAMPurging _noPurging;
    std::unique_ptr<GpuPlacementOffGuard> _cpuOnly;
};

TEST_F(CacheEquivalenceTest, ValueTogglesAndUndoRedo)
{
    runScenario("value toggles", [this](Scenario* s) {
        NodePtr checker = createChecker();
        NodePtr blur = createBlurOn(checker, 1.);
        NodePtr grade = create(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
        KnobColorPtr multiply = knobOf<KnobColor>(grade, kGradeParamMultiply);
        if (!checker || !blur || !grade || !multiply || !grade->connectInput(blur, 0)) {
            return false;
        }
        s->output = grade;
        s->steps.push_back(setBlurSize("size 2", blur, 2.));
        s->steps.push_back(setBlurSize("size 1 (undo)", blur, 1.));
        s->steps.push_back(setBlurSize("size 2 (redo)", blur, 2.));
        s->steps.push_back(Step { "multiply 0.5 on every channel", [multiply]() {
                                     multiply->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonUserEdited);
                                 } });
        s->steps.push_back(Step { "multiply 1 (undo)", [multiply]() {
                                     multiply->setValues(1., 1., 1., 1., ViewSpec::all(), eValueChangedReasonUserEdited);
                                 } });
        s->steps.push_back(setBlurSize("size 1 (undo)", blur, 1.));
        s->steps.push_back(Step { "size keyed", [blur]() {
                                     knobOf<KnobDouble>(blur, kBlurParamSize)->setValueAtTime(1., 3., ViewSpec::all(), 0);
                                 } });
        s->steps.push_back(Step { "key removed (undo)", [blur]() {
                                     KnobDoublePtr size = knobOf<KnobDouble>(blur, kBlurParamSize);
                                     size->removeAnimation(ViewSpec::all(), 0);
                                     size->setValue(1., ViewSpec::all(), 0);
                                 } });

        return true;
    });
}

TEST_F(CacheEquivalenceTest, NodeDisableToggles)
{
    runScenario("node disable", [this](Scenario* s) {
        NodePtr checker = createChecker();
        NodePtr upper = createBlurOn(checker, 2.);
        NodePtr lower = createBlurOn(upper, 1.);
        if (!checker || !upper || !lower) {
            return false;
        }
        s->output = lower;
        s->steps.push_back(setBool("upper disabled", upper->getDisabledKnob(), true));
        s->steps.push_back(setBool("upper enabled", upper->getDisabledKnob(), false));
        s->steps.push_back(setBool("lower disabled", lower->getDisabledKnob(), true));

        return true;
    });
}

TEST_F(CacheEquivalenceTest, OuterAndNestedGroupDisable)
{
    runScenario("group disable", [this](Scenario* s) {
        NodePtr checker = createChecker();
        NodePtr outer = createEmptyGroup(project());
        if (!checker || !outer) {
            return false;
        }
        NodeCollectionPtr outerCollection = collectionOf(outer);
        NodePtr outerInput = createIn(outerCollection, PLUGINID_NATRON_INPUT);
        NodePtr outerBlur = createIn(outerCollection, PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
        NodePtr outerOutput = createIn(outerCollection, PLUGINID_NATRON_OUTPUT);
        NodePtr nested = createEmptyGroup(outerCollection);
        if (!outerInput || !outerBlur || !outerOutput || !nested) {
            return false;
        }
        NodeCollectionPtr nestedCollection = collectionOf(nested);
        NodePtr nestedInput = createIn(nestedCollection, PLUGINID_NATRON_INPUT);
        NodePtr nestedGrade = createIn(nestedCollection, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
        NodePtr nestedOutput = createIn(nestedCollection, PLUGINID_NATRON_OUTPUT);
        KnobColorPtr multiply = knobOf<KnobColor>(nestedGrade, kGradeParamMultiply);
        KnobDoublePtr size = knobOf<KnobDouble>(outerBlur, kBlurParamSize);
        if (!nestedInput || !nestedGrade || !nestedOutput || !multiply || !size) {
            return false;
        }
        multiply->setValues(0.25, 0.5, 0.75, 1., ViewSpec::all(), eValueChangedReasonUserEdited);
        size->setValue(2., ViewSpec::all(), 0);
        size->setValue(2., ViewSpec::all(), 1);
        if (!nestedGrade->connectInput(nestedInput, 0) || !nestedOutput->connectInput(nestedGrade, 0)
            || !outerBlur->connectInput(outerInput, 0) || !nested->connectInput(outerBlur, 0) || !outerOutput->connectInput(nested, 0)
            || !outer->connectInput(checker, 0)) {
            return false;
        }
        NodePtr below = createBlurOn(outer, 1.);
        if (!below) {
            return false;
        }
        s->output = below;
        s->steps.push_back(setBool("outer disabled", outer->getDisabledKnob(), true));
        s->steps.push_back(setBool("outer enabled", outer->getDisabledKnob(), false));
        s->steps.push_back(setBool("nested disabled", nested->getDisabledKnob(), true));
        s->steps.push_back(setBool("outer disabled", outer->getDisabledKnob(), true));
        s->steps.push_back(setBool("outer enabled", outer->getDisabledKnob(), false));
        s->steps.push_back(setBool("nested enabled", nested->getDisabledKnob(), false));

        return true;
    });
}

TEST_F(CacheEquivalenceTest, GroupInputRewireAndInputSwaps)
{
    runScenario("group input rewire", [this](Scenario* s) {
        NodePtr checker = createChecker();
        NodePtr constant = create(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
        NodePtr group = createEmptyGroup(project());
        if (!checker || !constant || !group) {
            return false;
        }
        KnobColorPtr color = knobOf<KnobColor>(constant, kConstantParamColor);
        if (color) {
            color->setValues(0.1, 0.6, 0.3, 1., ViewSpec::all(), eValueChangedReasonUserEdited);
        }
        NodeCollectionPtr collection = collectionOf(group);
        NodePtr input = createIn(collection, PLUGINID_NATRON_INPUT);
        NodePtr blur = createIn(collection, PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
        NodePtr output = createIn(collection, PLUGINID_NATRON_OUTPUT);
        if (!input || !blur || !output || !blur->connectInput(input, 0) || !output->connectInput(blur, 0)) {
            return false;
        }
        NodePtr merge = create(PLUGINID_NATRON_MERGE, PLUGIN_MAJOR_NATRON_MERGE);
        NodePtr blurredChecker = createBlurOn(checker, 3.);
        if (!merge || !blurredChecker || !merge->connectInput(group, kMergeInputB) || !merge->connectInput(blurredChecker, kMergeInputA)) {
            return false;
        }
        s->output = merge;
        s->steps.push_back(Step { "group fed the checker", [group, checker]() { ignore_result(group->replaceInput(checker, 0)); } });
        s->steps.push_back(Step { "group fed the constant", [group, constant]() { ignore_result(group->replaceInput(constant, 0)); } });
        s->steps.push_back(Step { "group fed the checker again", [group, checker]() { ignore_result(group->replaceInput(checker, 0)); } });
        s->steps.push_back(Step { "merge inputs swapped", [merge]() { merge->switchInput0And1(); } });
        s->steps.push_back(Step { "merge inputs swapped back", [merge]() { merge->switchInput0And1(); } });
        s->steps.push_back(Step { "group disconnected", [group]() { ignore_result(group->disconnectInput(0)); } });

        return true;
    });
}

TEST_F(CacheEquivalenceTest, ExpressionEdits)
{
    runScenario("expressions", [this](Scenario* s) {
        NodePtr checker = createChecker();
        NodePtr driven = createBlurOn(checker, 1.);
        NodePtr driver = createBlurOn(NodePtr(), 2.);
        KnobDoublePtr drivenSize = knobOf<KnobDouble>(driven, kBlurParamSize);
        KnobDoublePtr driverSize = knobOf<KnobDouble>(driver, kBlurParamSize);
        if (!checker || !driven || !driver || !drivenSize || !driverSize) {
            return false;
        }
        const std::string reference = driver->getScriptName() + ".size.getValue(0)";
        s->output = driven;
        s->steps.push_back(Step { "expression reads the driver", [drivenSize, reference]() { drivenSize->setExpression(0, reference, false, true); } });
        s->steps.push_back(setDouble("driver size 4", driverSize, 0, 4.));
        s->steps.push_back(setDouble("driver size 2 (undo)", driverSize, 0, 2.));
        s->steps.push_back(Step { "expression 3 * 1", [drivenSize]() { drivenSize->setExpression(0, "3 * 1", false, true); } });
        s->steps.push_back(Step { "expression reads the driver again", [drivenSize, reference]() { drivenSize->setExpression(0, reference, false, true); } });
        s->steps.push_back(Step { "expression cleared", [drivenSize]() { drivenSize->clearExpression(0, true); } });

        return true;
    });
}

TEST_F(CacheEquivalenceTest, LinkedKnobEdits)
{
    runScenario("linked knobs", [this](Scenario* s) {
        NodePtr checker = createChecker();
        NodePtr slave = createBlurOn(checker, 1.);
        NodePtr master = createBlurOn(NodePtr(), 3.);
        KnobDoublePtr slaveSize = knobOf<KnobDouble>(slave, kBlurParamSize);
        KnobDoublePtr masterSize = knobOf<KnobDouble>(master, kBlurParamSize);
        if (!checker || !slave || !master || !slaveSize || !masterSize) {
            return false;
        }
        s->output = slave;
        s->steps.push_back(Step { "linked to the master", [slaveSize, masterSize]() { ignore_result(slaveSize->slaveTo(0, masterSize, 0)); } });
        s->steps.push_back(setDouble("master size 5", masterSize, 0, 5.));
        s->steps.push_back(setDouble("master size 3 (undo)", masterSize, 0, 3.));
        s->steps.push_back(Step { "unlinked", [slaveSize]() { slaveSize->unSlave(0, false); } });
        s->steps.push_back(setDouble("master size 5", masterSize, 0, 5.));
        s->steps.push_back(Step { "linked again", [slaveSize, masterSize]() { ignore_result(slaveSize->slaveTo(0, masterSize, 0)); } });

        return true;
    });
}

TEST_F(CacheEquivalenceTest, RotoBezierEdits)
{
    runScenario("roto beziers", [this](Scenario* s) {
        NodePtr checker = createChecker();
        NodePtr roto = create(PLUGINID_NATRON_ROTO);
        if (!checker || !roto || !roto->connectInput(checker, 0)) {
            return false;
        }
        RotoContextPtr context = roto->getRotoContext();
        if (!context) {
            return false;
        }
        const double time = 1.;
        BezierPtr square = context->makeSquare(8., 40., 24., time);
        if (!square) {
            return false;
        }
        context->refreshRotoPaintTree();
        KnobDoublePtr opacity = square->getOpacityKnob();
        if (!opacity) {
            return false;
        }
        s->output = roto;
        s->time = time;
        s->steps.push_back(Step { "point moved", [square, time]() { square->movePointByIndex(0, time, 6., -4.); } });
        s->steps.push_back(Step { "point moved back (undo)", [square, time]() { square->movePointByIndex(0, time, -6., 4.); } });
        s->steps.push_back(Step { "point moved (redo)", [square, time]() { square->movePointByIndex(0, time, 6., -4.); } });
        s->steps.push_back(setDouble("opacity 0.5", opacity, 0, 0.5));
        s->steps.push_back(setDouble("opacity 1 (undo)", opacity, 0, 1.));
        s->steps.push_back(Step { "ellipse added", [context, time]() {
                                     ignore_result(context->makeEllipse(40., 20., 16., true, time));
                                     context->refreshRotoPaintTree();
                                 } });
        s->steps.push_back(Step { "square removed", [context, square]() {
                                     context->removeItem(square);
                                     context->refreshRotoPaintTree();
                                 } });

        return true;
    });
}

TEST_F(CacheEquivalenceTest, RotoPaintStrokesAddedAndEdited)
{
    runScenario("roto paint strokes", [this](Scenario* s) {
        NodePtr checker = createChecker();
        NodePtr paint = create(PLUGINID_NATRON_ROTOPAINT);
        if (!checker || !paint || !paint->connectInput(checker, 0)) {
            return false;
        }
        RotoContextPtr context = paint->getRotoContext();
        if (!context) {
            return false;
        }
        // RotoContext::makeStroke() inserts the item before creating its nodes, which dereferences
        // the merge node that does not exist yet; the interact creates the nodes first.
        RotoLayerPtr layer = context->getOrCreateBaseLayer();
        std::shared_ptr<RotoStrokeItemPtr> stroke = std::make_shared<RotoStrokeItemPtr>();
        s->output = paint;
        s->steps.push_back(Step { "stroke added", [context, layer, stroke]() {
                                     *stroke = RotoStrokeItemPtr(new RotoStrokeItem(eRotoStrokeTypeSolid, context, "Stroke1", RotoLayerPtr()));
                                     (*stroke)->createNodes();
                                     context->addItem(layer, 0, *stroke, RotoItem::eSelectionReasonOther);
                                     ignore_result((*stroke)->appendPoint(true, RotoPoint(10., 10., 1., 1.)));
                                     ignore_result((*stroke)->appendPoint(false, RotoPoint(40., 50., 1., 2.)));
                                 } });
        s->steps.push_back(Step { "stroke extended", [stroke]() { ignore_result((*stroke)->appendPoint(false, RotoPoint(55., 20., 1., 3.))); } });
        s->steps.push_back(Step { "brush size 12", [stroke]() { (*stroke)->getBrushSizeKnob()->setValue(12.); } });
        s->steps.push_back(Step { "brush size back (undo)", [stroke]() {
                                     KnobDoublePtr size = (*stroke)->getBrushSizeKnob();
                                     size->setValue(size->getDefaultValue(0));
                                 } });
        s->steps.push_back(Step { "square added", [context]() {
                                     ignore_result(context->makeSquare(20., 30., 16., 1.));
                                     context->refreshRotoPaintTree();
                                 } });

        return true;
    });
}

// Files are looked at again only on Reload, so the rewrite is only compared after one. A second
// Read of the same file is compared too: Reload must re-key every Read of the file, not only the
// one whose button was pressed.
TEST_F(CacheEquivalenceTest, ReadRewrittenInPlaceThenReloaded)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("rewritten.exr")).toStdString();

    for (int which = 0; which < 2; ++which) {
        runScenario(which ? "second Read of a rewritten file" : "rewritten Read", [this, &path, which](Scenario* s) {
            if (!writeFlatExr(path, 16, 12, 0.25f)) {
                return false;
            }
            setMTime(path, 1000000);
            NodePtr reader = createWorkingSpaceRead(path);
            NodePtr twin = createWorkingSpaceRead(path);
            KnobFilePtr fileKnob = knobOf<KnobFile>(reader, kOfxImageEffectFileParamName);
            if (!reader || !twin || !fileKnob) {
                return false;
            }
            NodePtr blur = createBlurOn(which ? twin : reader, 1.);
            if (!blur) {
                return false;
            }
            NodePtr other = which ? reader : twin;
            s->output = blur;
            s->steps.push_back(Step { "other Read rendered", [this, other]() { warm(other); } });
            s->steps.push_back(Step { "file rewritten and reloaded", [path, fileKnob]() {
                                         EXPECT_TRUE(writeFlatExr(path, 32, 24, 0.75f));
                                         setMTime(path, 2000000);
                                         fileKnob->reloadFile();
                                     } });
            s->steps.push_back(Step { "file restored and reloaded", [path, fileKnob]() {
                                         EXPECT_TRUE(writeFlatExr(path, 16, 12, 0.25f));
                                         setMTime(path, 1000000);
                                         fileKnob->reloadFile();
                                     } });

            return true;
        });
    }
}

TEST_F(CacheEquivalenceTest, LayerRegistryChanges)
{
    runScenario("layer registry", [this](Scenario* s) {
        NodePtr reader = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
        NodePtr add = create(PLUGINID_NATRON_ADDLAYERS);
        KnobChannelSetPtr layers = knobOf<KnobChannelSet>(add, kAddLayersParamLayers);
        if (!reader || !add || !layers || !add->connectInput(reader, 0)) {
            return false;
        }
        layers->setLayer(0, "mask", NULL);
        ProjectPtr p = project();
        s->output = add;
        s->steps.push_back(Step { "mask registered", [p]() {
                                     std::string error;
                                     ignore_result(p->addLayer(ImageLayerDesc("mask", "mask", "", std::vector<std::string>(1, "A")), LayerRegistryEntry::eOriginUser, &error));
                                 } });
        s->steps.push_back(Step { "unused layer registered", [p]() {
                                     std::string error;
                                     ignore_result(p->addLayer(ImageLayerDesc("extra", "extra", "", std::vector<std::string>(1, "X")), LayerRegistryEntry::eOriginUser, &error));
                                 } });
        s->steps.push_back(Step { "unused layer removed", [p]() {
                                     std::string error;
                                     ignore_result(p->removeLayer("extra", &error));
                                 } });
        s->steps.push_back(Step { "diffuse added too", [layers]() { layers->addLayer("diffuse", NULL); } });

        return true;
    });
}

TEST_F(CacheEquivalenceTest, Precomp)
{
    std::unique_ptr<QTemporaryDir> dir(new QTemporaryDir(appPTR->getApplicationBinaryPath() + QLatin1String("/precomp-XXXXXX")));
    ASSERT_TRUE(dir->isValid());
    const std::string sub = buildAndSavePrecompSubProject(*dir);
    ASSERT_FALSE(sub.empty());

    runScenario("precomp", [this, &sub](Scenario* s) {
        NodePtr precomp = create(PLUGINID_NATRON_PRECOMP);
        if (!precomp) {
            return false;
        }
        KnobFilePtr file = knobOf<KnobFile>(precomp, "projectFilename");
        KnobChoicePtr writeNode = knobOf<KnobChoice>(precomp, "writeNode");
        if (!file || !writeNode) {
            return false;
        }
        file->setValue(sub, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
        NodePtr blur = createBlurOn(NodePtr(), 1.);
        if (!blur || !project()->connectNodes(0, precomp, blur)) {
            return false;
        }
        s->output = blur;
        s->time = 2.;
        s->steps.push_back(setBlurSize("size 2", blur, 2.));
        s->steps.push_back(setBlurSize("size 1 (undo)", blur, 1.));
        s->steps.push_back(Step { "no write selected", [writeNode]() { writeNode->setValue(0, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL); } });
        s->steps.push_back(Step { "write selected again", [writeNode]() { writeNode->setValue(1, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL); } });

        return true;
    });
}

TEST_F(CacheEquivalenceTest, TwoIdenticalNodesShareAnEntry)
{
    runScenario("identical nodes", [this](Scenario* s) {
        NodePtr checkerA = createChecker();
        NodePtr checkerB = createChecker();
        NodePtr blurA = createBlurOn(checkerA, 2.);
        NodePtr blurB = createBlurOn(checkerB, 2.);
        if (!checkerA || !checkerB || !blurA || !blurB) {
            return false;
        }
        s->output = blurB;
        s->steps.push_back(Step { "twin rendered", [this, blurA]() { warm(blurA); } });
        KnobDoublePtr sizeA = knobOf<KnobDouble>(blurA, kBlurParamSize);
        s->steps.push_back(Step { "twin size 5 and rendered", [this, blurA, sizeA]() {
                                     sizeA->setValue(5., ViewSpec::all(), 0);
                                     warm(blurA);
                                 } });
        s->steps.push_back(Step { "twin size 2 and rendered", [this, blurA, sizeA]() {
                                     sizeA->setValue(2., ViewSpec::all(), 0);
                                     warm(blurA);
                                 } });
        s->steps.push_back(setBlurSize("size 5, like the twin was", blurB, 5.));
        s->steps.push_back(Step { "twin disabled and rendered", [this, blurA]() {
                                     blurA->getDisabledKnob()->setValue(true);
                                     warm(blurA);
                                 } });
        s->steps.push_back(setBlurSize("size 2", blurB, 2.));

        return true;
    });
}

TEST_F(CacheEquivalenceTest, DeepSamples)
{
    for (int flattened = 0; flattened < 2; ++flattened) {
        runScenario(flattened ? "deep flattened" : "deep samples", [this, flattened](Scenario* s) {
            NodePtr checker = createChecker();
            NodePtr fromImage = create(PLUGINID_NATRON_DEEPFROMIMAGE);
            NodePtr crop = create(PLUGINID_NATRON_DEEPCROP);
            KnobDoublePtr depth = knobOf<KnobDouble>(fromImage, "depth");
            KnobDoublePtr bbox = knobOf<KnobDouble>(crop, "bbox");
            KnobBoolPtr useBBox = knobOf<KnobBool>(crop, "useBBox");
            if (!checker || !fromImage || !crop || !depth || !bbox || !useBBox
                || !fromImage->connectInput(checker, 0) || !crop->connectInput(fromImage, 0)) {
                return false;
            }
            depth->setValue(5.);
            bbox->setValue(8., ViewSpec::all(), 0);
            bbox->setValue(8., ViewSpec::all(), 1);
            bbox->setValue(40., ViewSpec::all(), 2);
            bbox->setValue(40., ViewSpec::all(), 3);
            useBBox->setValue(false);
            NodePtr output = crop;
            if (flattened) {
                output = create(PLUGINID_NATRON_DEEPTOIMAGE);
                if (!output || !output->connectInput(crop, 0)) {
                    return false;
                }
            }
            s->output = output;
            s->deep = !flattened;
            s->steps.push_back(setDouble("depth 8", depth, 0, 8.));
            s->steps.push_back(setDouble("depth 5 (undo)", depth, 0, 5.));
            s->steps.push_back(setBool("bbox crop on", useBBox, true));
            s->steps.push_back(setDouble("bbox moved", bbox, 0, 16.));
            s->steps.push_back(setDouble("bbox back (undo)", bbox, 0, 8.));
            s->steps.push_back(setBool("from image disabled", fromImage->getDisabledKnob(), true));
            s->steps.push_back(setBool("from image enabled", fromImage->getDisabledKnob(), false));

            return true;
        });
    }
}

TEST_F(CacheEquivalenceTest, SyntheticGeneratorGradeGraph)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    runScenario("synthetic generator grade", [this, &tmp](Scenario* s) {
        if (!buildGeneratorGradeWriteGraph(tmp)) {
            return false;
        }
        NodePtr grade = findByPluginID(project()->getNodes(), PLUGINID_NATRON_GRADE);
        KnobColorPtr whitePoint = knobOf<KnobColor>(grade, kGradeParamWhitePoint);
        if (!grade || !whitePoint) {
            return false;
        }
        s->output = grade;
        s->time = 5.;
        s->steps.push_back(Step { "white point key moved", [whitePoint]() { whitePoint->setValueAtTime(10, 3.5, ViewSpec::all(), 0); } });
        s->steps.push_back(Step { "white point key back (undo)", [whitePoint]() { whitePoint->setValueAtTime(10, 2.5, ViewSpec::all(), 0); } });
        s->steps.push_back(Step { "green expression doubled", [whitePoint]() { whitePoint->setExpression(1, "thisNode.whitePoint.getValue(0) * 2", false, true); } });
        s->steps.push_back(Step { "green expression back (undo)", [whitePoint]() { whitePoint->setExpression(1, "thisNode.whitePoint.getValue(0)", false, true); } });

        return true;
    });
}

TEST_F(CacheEquivalenceTest, SyntheticRotoPaintGraph)
{
    runScenario("synthetic roto paint", [this](Scenario* s) {
        if (!buildRotoPaintAndTrackerGraph()) {
            return false;
        }
        NodePtr paint = findByPluginID(project()->getNodes(), PLUGINID_NATRON_ROTOPAINT);
        RotoContextPtr context = paint ? paint->getRotoContext() : RotoContextPtr();
        if (!context) {
            return false;
        }
        BezierPtr square;
        const std::list<RotoDrawableItemPtr> items = context->getCurvesByRenderOrder(false);
        for (std::list<RotoDrawableItemPtr>::const_iterator it = items.begin(); it != items.end() && !square; ++it) {
            square = std::dynamic_pointer_cast<Bezier>(*it);
        }
        if (!square || !square->getFeatherKnob()) {
            return false;
        }
        KnobDoublePtr feather = square->getFeatherKnob();
        s->output = paint;
        s->time = 5.;
        s->steps.push_back(Step { "point moved", [square]() { square->movePointByIndex(0, 1., 3., 2.); } });
        s->steps.push_back(Step { "point moved back (undo)", [square]() { square->movePointByIndex(0, 1., -3., -2.); } });
        s->steps.push_back(Step { "feather key moved", [feather]() { feather->setValueAtTime(10, 9., ViewSpec::all(), 0); } });
        s->steps.push_back(Step { "feather key back (undo)", [feather]() { feather->setValueAtTime(10, 6., ViewSpec::all(), 0); } });

        return true;
    });
}

// The bench comps are UHD, so a window at the centre of the format stands for the frame. Each comp
// and scheduler mode is its own case, which keeps every one inside the per-case timeout.
// defocus_retime pulls dozens of plate frames through blurs of up to 300 px for any window, so it
// renders at 1/8 scale, where that upstream area is over an order of magnitude smaller and a debug
// build fits the timeout.
struct BenchComp {
    const char* file;
    RenderSchedulerModeEnum mode;
    unsigned mipmapLevel;
};

static void
PrintTo(const BenchComp& comp,
        std::ostream* os)
{
    *os << comp.file << " [" << modeName(comp.mode) << "]";
}

class CacheEquivalenceBenchCompTest
    : public CacheEquivalenceTest,
      public ::testing::WithParamInterface<BenchComp> {
};

TEST_P(CacheEquivalenceBenchCompTest, EditsAndDisables)
{
    const QString dir = QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/../../tools/bench/comps/");
    const QString name = QString::fromUtf8(GetParam().file);

    runScenario(GetParam().file, [this, &dir, &name](Scenario* s) {
        if (!project()->loadProject(dir, name, false, false)) {
            return false;
        }
        NodesList nodes = project()->getNodes();
        NodePtr writer;
        std::vector<NodePtr> editable;
        for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if (!*it) {
                continue;
            }
            if ((*it)->getEffectInstance()->isWriter()) {
                writer = writer ? writer : *it;
            } else {
                editable.push_back(*it);
            }
        }
        if (!writer || !writer->getInput(0) || editable.empty()) {
            return false;
        }
        std::sort(editable.begin(), editable.end(), [](const NodePtr& a, const NodePtr& b) { return a->getScriptName() < b->getScriptName(); });
        s->output = writer->getInput(0);
        s->mipmapLevel = GetParam().mipmapLevel;
        const RectI format = s->output->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat().downscalePowerOfTwoSmallestEnclosing(s->mipmapLevel);
        const int cx = (format.x1 + format.x2) / 2;
        const int cy = (format.y1 + format.y2) / 2;
        s->roi = RectI(cx - kSize / 2, cy - kSize / 2, cx + kSize / 2, cy + kSize / 2);
        const std::size_t picks = std::min<std::size_t>(4, editable.size());
        for (std::size_t i = 0; i < picks; ++i) {
            ignore_result(appendFirstKnobToggle(editable[i * editable.size() / picks], &s->steps));
        }
        NodePtr middle = editable[editable.size() / 2];
        s->steps.push_back(setBool(middle->getScriptName() + " disabled", middle->getDisabledKnob(), true));
        s->steps.push_back(setBool(middle->getScriptName() + " enabled", middle->getDisabledKnob(), false));

        return true;
    },
                { GetParam().mode });
}

INSTANTIATE_TEST_CASE_P(Comps,
                        CacheEquivalenceBenchCompTest,
                        ::testing::Values(BenchComp { "keying_grade.ntpz", eRenderSchedulerModeLegacy, 0 },
                                          BenchComp { "keying_grade.ntpz", eRenderSchedulerModeTaskGraph, 0 },
                                          BenchComp { "cg_multipass.ntpz", eRenderSchedulerModeLegacy, 0 },
                                          BenchComp { "cg_multipass.ntpz", eRenderSchedulerModeTaskGraph, 0 },
                                          BenchComp { "defocus_retime.ntpz", eRenderSchedulerModeLegacy, 3 },
                                          BenchComp { "defocus_retime.ntpz", eRenderSchedulerModeTaskGraph, 3 }));

// Each bundled plugin is rendered on a checkerboard feeding every input, and each sampled knob
// dimension is perturbed, rendered, reverted and rendered again through the caches before that
// last render is compared with a cold one, and with a second cold one when they differ. It runs
// in the scheduler mode the process was started in, so the gate's run in each mode covers that
// mode.
class CacheEquivalenceSweepBase
    : public CacheEquivalenceTest {
protected:
    NodePtr createPluginNode(const PluginVersion& plugin)
    {
        CreateNodeArgs args(plugin.id, project());

        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, plugin.major, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, plugin.minor, 1);
        args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        args.setProperty<bool>(kCreateNodeArgsPropSilent, true);
        args.setProperty<bool>(kCreateNodeArgsPropTrustPluginID, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    // The Viewer draws into its own textures and a Backdrop renders nothing, so neither has an
    // image of its own in the caches under test.
    static bool rendersAnImage(const NodePtr& node)
    {
        return !node->isEffectViewer() && (node->getPluginID() != PLUGINID_NATRON_BACKDROP);
    }

    struct Findings {
        std::vector<std::string> hashGaps;
        std::vector<std::string> nondeterministic;
    };

    void compareNow(const Scenario& s,
                    const std::string& where,
                    Findings* findings)
    {
        const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
        const Verdict verdict = judgeCachedRender(s, layers);

        if (!verdict.nondeterminism.empty()) {
            findings->nondeterministic.push_back(nondeterminismMessage(where, verdict.nondeterminism));
        } else if (!verdict.hashGap.empty()) {
            findings->hashGaps.push_back(where + ": " + verdict.hashGap);
        }
    }

    // A Group or a Precomp renders nothing of its own: what consumes one pulls the node its output
    // redirects to, so a Dot below it stands in for that consumer. Anything else is rendered itself.
    NodePtr consumerOf(const NodePtr& node)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        if (!std::dynamic_pointer_cast<NodeGroup>(effect) && !std::dynamic_pointer_cast<PrecompNode>(effect)) {
            return NodePtr();
        }
        NodePtr dot = create(PLUGINID_NATRON_DOT);
        if (dot && !dot->connectInput(node, 0)) {
            dot->destroyNode(true, false);

            return NodePtr();
        }

        return dot;
    }

    void sweepPlugin(const PluginVersion& plugin,
                     const Allowlist& allowlist,
                     const NodePtr& source,
                     const QTemporaryDir& tmp,
                     Findings* findings,
                     int* editsMade)
    {
        const std::string where = plugin.describe() + " [" + modeName(_mode) + "]";
        NodePtr node = createPluginNode(plugin);

        if (!node) {
            findings->hashGaps.push_back(where + ": the node could not be created");

            return;
        }
        if (!rendersAnImage(node)) {
            std::cout << "[cache equivalence sweep] " << where << ": renders no cached image, not compared" << std::endl;
            node->destroyNode(true, false);

            return;
        }
        for (int i = 0; i < node->getNInputs(); ++i) {
            ignore_result(node->connectInput(source, i));
        }
        if (node->getEffectInstance()->isWriter()) {
            node->setOutputFilesForWriter(QDir(tmp.path()).filePath(QString::fromUtf8("sweep.####.exr")).toStdString());
        }

        NodePtr consumer = consumerOf(node);
        Scenario s;
        s.output = consumer ? consumer : node;
        // A node putting deep data on its output is only ever asked for a deep render.
        s.deep = node->getEffectInstance()->producesDeepData();
        appPTR->clearAllCaches();
        compareNow(s, where + " as created", findings);

        std::vector<std::pair<KnobIPtr, int>> candidates;
        if (!allowlist.exemptsPlugin(plugin.id)) {
            const KnobsVec knobs = node->getKnobs();
            for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
                if (!*it || !(*it)->isRenderHashed() || allowlist.exempts(plugin.id, (*it)->getName())) {
                    continue;
                }
                for (int d = 0; d < (*it)->getDimension(); ++d) {
                    if (staticValueSkipReason(*it, d).empty()) {
                        candidates.push_back(std::make_pair(*it, d));
                    }
                }
            }
        }
        const std::size_t picks = std::min(kMaxEditsPerPlugin, candidates.size());
        const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
        for (std::size_t p = 0; p < picks; ++p) {
            const std::pair<KnobIPtr, int>& candidate = candidates[p * candidates.size() / picks];
            KnobEdit edit;
            bool unsupported = false;
            if (!makeEdit(candidate.first, candidate.second, &edit, &unsupported).empty() || unsupported) {
                continue;
            }
            const std::string label = where + " " + knobLabel(candidate.first, candidate.second) + " " + edit.what;
            ignore_result(render(s, layers, false));
            if (!edit.apply()) {
                continue;
            }
            ++*editsMade;
            ignore_result(render(s, layers, false));
            edit.revert();
            compareNow(s, label + " and back", findings);
        }
        if (consumer) {
            consumer->destroyNode(true, false);
        }
        node->destroyNode(true, false);
    }
};

class CacheEquivalenceSweepTest
    : public CacheEquivalenceSweepBase,
      public ::testing::WithParamInterface<int> {
};

TEST_P(CacheEquivalenceSweepTest, CachedRenderAfterAPerturbedAndRevertedKnobEqualsAColdRender)
{
    const Allowlist allowlist = loadAllowlist();
    ASSERT_TRUE(allowlist.errors.empty()) << allowlist.errors.front();
    const std::vector<PluginVersion> plugins = sweptPlugins();
    ASSERT_FALSE(plugins.empty());
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr source = createChecker();
    ASSERT_TRUE(bool(source));

    Findings findings;
    int pluginsSwept = 0;
    int editsMade = 0;
    for (std::size_t i = GetParam(); i < plugins.size(); i += kSweepShardCount) {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        const std::size_t gapsBefore = findings.hashGaps.size();
        const std::size_t nondeterministicBefore = findings.nondeterministic.size();
        const int editsBefore = editsMade;
        sweepPlugin(plugins[i], allowlist, source, tmp, &findings, &editsMade);
        ++pluginsSwept;
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::cout << "[cache equivalence sweep] " << plugins[i].describe() << ": " << (editsMade - editsBefore) << " edits, "
                  << (findings.hashGaps.size() - gapsBefore) << " mismatches, "
                  << (findings.nondeterministic.size() - nondeterministicBefore) << " non-deterministic, " << ms << " ms" << std::endl;
    }
    std::cout << "[cache equivalence sweep] shard " << GetParam() << "/" << kSweepShardCount << " [" << modeName(_mode) << "]: "
              << pluginsSwept << " plugins, " << editsMade << " edits" << std::endl;

    std::ostringstream gaps;
    for (std::size_t i = 0; i < findings.hashGaps.size(); ++i) {
        gaps << "  " << findings.hashGaps[i] << "\n";
    }
    EXPECT_TRUE(findings.hashGaps.empty()) << findings.hashGaps.size() << " cached render(s) differ from a cold render, each a hash gap:\n"
                                           << gaps.str();
    std::ostringstream nondeterministic;
    for (std::size_t i = 0; i < findings.nondeterministic.size(); ++i) {
        nondeterministic << "  " << findings.nondeterministic[i] << "\n";
    }
    EXPECT_TRUE(findings.nondeterministic.empty()) << findings.nondeterministic.size()
                                                   << " state(s) render non-deterministically, which no cache can be checked against:\n"
                                                   << nondeterministic.str();
}

INSTANTIATE_TEST_CASE_P(Shards, CacheEquivalenceSweepTest, ::testing::Range(0, kSweepShardCount));
