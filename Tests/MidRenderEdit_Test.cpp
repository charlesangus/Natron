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
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include "BaseTest.h"
#include "FlatExrReader.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/BlockingBackgroundRender.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Merge/Switch.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/TLSHolder.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

#include <SequenceParsing.h>
#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {
const int kJobGuardSeconds = 60;
// A render waiting on an edit gives up after this and renders on, so an edit the main thread
// cannot make while the render waits costs time, never a hang.
const int kEditWaitSeconds = 10;
const int kStateCount = 4;

// Hands each edit a render asks for to the main thread, which makes it while the render waits.
// The render asks at its triggerAt-th, triggerAt+step-th... frame snapshot, one edit each time.
class EditBroker {
public:
    EditBroker(int editCount,
               int triggerAt,
               int step,
               const std::function<void(int)>& applyEdit)
        : _editCount(editCount)
        , _triggerAt(triggerAt)
        , _step(step)
        , _applyEdit(applyEdit)
    {
    }

    // On a rendering thread, once a frame's snapshot is taken.
    void onSnapshot()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        const int n = ++_snapshots;
        if ((n < _triggerAt) || ((n - _triggerAt) % _step != 0) || (_requested >= _editCount)) {
            return;
        }
        const int edit = ++_requested;
        _cond.notify_all();
        _cond.wait_for(lock, std::chrono::seconds(kEditWaitSeconds), [this, edit]() { return (_applied >= edit) || _done; });
    }

    void markDone()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _done = true;
        _cond.notify_all();
    }

    // On the main thread: makes the edits asked for and delivers queued events until the job is
    // done. Aborts when it is not done within the guard, since its threads cannot be joined.
    void serveUntilDone()
    {
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kJobGuardSeconds);
        std::unique_lock<std::mutex> lock(_mutex);
        while (!_done) {
            if (_requested > _applied) {
                const int edit = _applied;
                lock.unlock();
                _applyEdit(edit);
                lock.lock();
                ++_applied;
                _cond.notify_all();
                continue;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                std::cerr << "mid-render edit job did not finish within " << kJobGuardSeconds << " s" << std::endl;
                std::abort();
            }
            lock.unlock();
            QCoreApplication::processEvents();
            lock.lock();
            _cond.wait_for(lock, std::chrono::milliseconds(10), [this]() { return _done || (_requested > _applied); });
        }
    }

    int applied() const
    {
        std::lock_guard<std::mutex> lock(_mutex);

        return _applied;
    }

private:
    const int _editCount;
    const int _triggerAt;
    const int _step;
    const std::function<void(int)> _applyEdit;
    mutable std::mutex _mutex;
    std::condition_variable _cond;
    int _snapshots = 0;
    int _requested = 0;
    int _applied = 0;
    bool _done = false;
};

// What one frame's args held for the watched nodes when its snapshot settled.
struct FrameSnapshot {
    double time = 0.;
    std::thread::id thread;
    std::vector<U64> hashes;
    std::vector<NodeMetadataConstPtr> metadata;
    bool complete = false;
};

// The graph state each edit moves to: Switch input, Reformat size, then the second Read's file.
struct GraphStates {
    std::string fileA;
    std::string fileB;
    std::string editedFileB;
    int width[2];
    int height[2];
    std::string resize;
};
} // namespace

class MidRenderEditTest
    : public BaseTest {
protected:
    void buildGraph(const GraphStates& states)
    {
        _states = states;
        _readA = createWorkingSpaceRead(states.fileA);
        _readB = createWorkingSpaceRead(states.fileB);
        _switch = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));
        _reformat = createNode(QString::fromUtf8(PLUGINID_NATRON_REFORMAT), PLUGIN_MAJOR_NATRON_REFORMAT);
        ASSERT_TRUE(_readA && _readB && _switch && _reformat);
        connectNodes(_readA, _switch, 0, true);
        connectNodes(_readB, _switch, 1, true);
        connectNodes(_switch, _reformat, 0, true);

        KnobChoice* type = dynamic_cast<KnobChoice*>(_reformat->getKnobByName(kReformatParamType).get());
        KnobChoice* resize = dynamic_cast<KnobChoice*>(_reformat->getKnobByName(kReformatParamResize).get());
        KnobBoolPtr center = std::dynamic_pointer_cast<KnobBool>(_reformat->getKnobByName(kReformatParamCenter));
        ASSERT_TRUE(type && resize && center);
        type->setValueFromID(kReformatParamTypeOptionToBox, 0);
        resize->setValueFromID(states.resize, 0);
        center->setValue(false, ViewSpec::all(), 0);
        _watched = { _readA, _readB, _switch, _reformat };
        applyState(0);
    }

    // Sets every edited knob to its value in state, whatever state the graph is in. Hashes are
    // content hashes, so returning to a state returns to its hashes.
    void applyState(int state)
    {
        KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(_switch->getKnobByName(kSwitchParamWhich));
        KnobIntPtr box = std::dynamic_pointer_cast<KnobInt>(_reformat->getKnobByName(kReformatParamBoxSize));
        KnobFilePtr fileB = std::dynamic_pointer_cast<KnobFile>(_readB->getKnobByName(kOfxImageEffectFileParamName));
        ASSERT_TRUE(which && box && fileB);
        which->setValue((state >= 1) ? 1 : 0, ViewSpec::all(), 0);
        box->setValue(_states.width[(state >= 2) ? 1 : 0], ViewSpec::all(), 0);
        box->setValue(_states.height[(state >= 2) ? 1 : 0], ViewSpec::all(), 1);
        fileB->setValue((state >= 3) ? _states.editedFileB : _states.fileB);
    }

    // Records every frame whose args tree is rooted at root, and hands broker the frames that
    // render off the main thread.
    void startRecording(const NodePtr& root,
                        EditBroker* broker)
    {
        const std::thread::id mainThread = std::this_thread::get_id();
        ParallelRenderArgsSetter::setFrameSnapshotHookForTests([this, root, broker, mainThread](const NodePtr& treeRoot, double time, ViewIdx view,
                                                                                                const std::map<NodePtr, ParallelRenderArgsPtr>& args) {
            if ((treeRoot != root) || (view != ViewIdx(0))) {
                return;
            }
            FrameSnapshot snapshot;
            snapshot.time = time;
            snapshot.thread = std::this_thread::get_id();
            snapshot.complete = true;
            for (std::size_t i = 0; i < _watched.size(); ++i) {
                std::map<NodePtr, ParallelRenderArgsPtr>::const_iterator found = args.find(_watched[i]);
                const bool present = (found != args.end()) && found->second->metadata;
                snapshot.complete = snapshot.complete && present;
                snapshot.hashes.push_back(present ? found->second->nodeHash : 0);
                snapshot.metadata.push_back(present ? found->second->metadata : NodeMetadataConstPtr());
            }
            {
                std::lock_guard<std::mutex> lock(_recordsMutex);
                _records.push_back(snapshot);
            }
            if (std::this_thread::get_id() != mainThread) {
                broker->onSnapshot();
            }
        });
    }

    // Brings the project to where the job's own renders would take it, so the job runs in the
    // states computeStateHashes reads: a Read's first render off the main thread adds its format to
    // the project, and connecting nodes queues a refresh of the project's layers. Either changes
    // every node's hash.
    void settleProject()
    {
        ProjectPtr project = getApp()->getProject();
        for (int state = 0; state < kStateCount; ++state) {
            applyState(state);
            const NodePtr reads[] = { _readA, _readB };
            for (const NodePtr& read : reads) {
                NodeMetadataConstPtr metadata = read->getEffectInstance()->getMetadata(1, ViewIdx(0));
                ASSERT_TRUE(bool(metadata)) << read->getScriptName();
                Format format;
                format.set(metadata->getOutputFormat());
                format.setPixelAspectRatio(metadata->getPixelAspectRatio(-1));
                project->setOrAddProjectFormat(format);
            }
        }
        applyState(0);
        QCoreApplication::processEvents();
    }

    void stopRecording()
    {
        ParallelRenderArgsSetter::setFrameSnapshotHookForTests(ParallelRenderArgsSetter::FrameSnapshotHook());
    }

    std::size_t recordCount()
    {
        std::lock_guard<std::mutex> lock(_recordsMutex);

        return _records.size();
    }

    // Each state's hashes for the watched nodes, read live after moving the graph to it.
    void computeStateHashes()
    {
        _stateHashes.assign(kStateCount, std::vector<U64>());
        for (int state = 0; state < kStateCount; ++state) {
            applyState(state);
            for (std::size_t i = 0; i < _watched.size(); ++i) {
                _stateHashes[state].push_back(_watched[i]->getHashValue());
            }
        }
    }

    // The state whose hashes a frame's args hold for every watched node, or -1 when the frame
    // mixes states.
    int stateOf(const FrameSnapshot& snapshot) const
    {
        for (int state = 0; state < kStateCount; ++state) {
            if (_stateHashes[state] == snapshot.hashes) {
                return state;
            }
        }

        return -1;
    }

    // Forgets every answer computed under any hash, so what follows is derived again.
    void freshen()
    {
        appPTR->clearAllCaches();
        for (std::size_t i = 0; i < _watched.size(); ++i) {
            EffectInstancePtr effect = _watched[i]->getEffectInstance();
            effect->clearActionsCache();
            effect->dropCachedMetadata();
        }
    }

    // Each frame holds one state's hashes, and the metadata it snapshotted is what that state
    // derives afresh. Returns each record's state.
    std::vector<int> expectSnapshotsAreStates()
    {
        std::vector<int> stateOfRecord;
        for (std::size_t r = 0; r < _records.size(); ++r) {
            EXPECT_TRUE(_records[r].complete) << "frame " << _records[r].time << " lacks a watched node's snapshot";
            const int state = _records[r].complete ? stateOf(_records[r]) : -1;
            EXPECT_GE(state, 0) << "frame " << _records[r].time << " holds hashes of no single state";
            stateOfRecord.push_back(state);
        }
        for (int state = 0; state < kStateCount; ++state) {
            applyState(state);
            freshen();
            for (std::size_t r = 0; r < _records.size(); ++r) {
                if (stateOfRecord[r] != state) {
                    continue;
                }
                for (std::size_t i = 0; i < _watched.size(); ++i) {
                    NodeMetadataConstPtr fresh = _watched[i]->getEffectInstance()->getMetadata(_records[r].time, ViewIdx(0));
                    EXPECT_TRUE(bool(fresh)) << _watched[i]->getScriptName();
                    if (!fresh) {
                        continue;
                    }
                    EXPECT_TRUE(*fresh == *_records[r].metadata[i]) << "frame " << _records[r].time << ", state " << state << ", " << _watched[i]->getScriptName();
                }
            }
        }

        return stateOfRecord;
    }

    // The Reformat's cached images at a frame under the hash its snapshot held.
    std::list<ImagePtr> cachedReformatImages(const FrameSnapshot& snapshot) const
    {
        std::list<ImagePtr> found;
        const U64 hash = snapshot.hashes[_watched.size() - 1];
        for (int frameVarying = 0; frameVarying < 2; ++frameVarying) {
            for (int fullScale = 0; fullScale < 2; ++fullScale) {
                const ImageKey key(_reformat.get(), hash, frameVarying != 0, snapshot.time, ViewIdx(0), 1., false, fullScale != 0);
                std::list<ImagePtr> images;
                if (appPTR->getImage(key, &images)) {
                    found.insert(found.end(), images.begin(), images.end());
                }
            }
        }

        return found;
    }

    // Every image cached under a frame's snapshot hash covers the region its state derives afresh.
    // Reads the cache before anything is freshened.
    void expectCachedImagesMatchStates(const std::vector<int>& stateOfRecord)
    {
        std::vector<std::list<RectD>> cachedRoDs(_records.size());
        for (std::size_t r = 0; r < _records.size(); ++r) {
            if (_records[r].complete) {
                const std::list<ImagePtr> images = cachedReformatImages(_records[r]);
                for (std::list<ImagePtr>::const_iterator it = images.begin(); it != images.end(); ++it) {
                    cachedRoDs[r].push_back((*it)->getRoD());
                }
            }
        }
        for (int state = 0; state < kStateCount; ++state) {
            applyState(state);
            freshen();
            for (std::size_t r = 0; r < _records.size(); ++r) {
                if ((stateOfRecord[r] != state) || cachedRoDs[r].empty()) {
                    continue;
                }
                RectD rod;
                bool isProjectFormat = false;
                ASSERT_EQ(eStatusOK, _reformat->getEffectInstance()->getRegionOfDefinition_public(_reformat->getHashValue(), _records[r].time, RenderScale::fromMipmapLevel(0), ViewIdx(0), &rod, &isProjectFormat));
                for (std::list<RectD>::const_iterator it = cachedRoDs[r].begin(); it != cachedRoDs[r].end(); ++it) {
                    EXPECT_TRUE(*it == rod) << "frame " << _records[r].time << ", state " << state;
                }
            }
        }
    }

    GraphStates _states;
    NodePtr _readA, _readB, _switch, _reformat;
    std::vector<NodePtr> _watched;
    std::mutex _recordsMutex;
    std::vector<FrameSnapshot> _records;
    std::vector<std::vector<U64>> _stateHashes;
};

// A 20-frame blocking file render, edited three times while it runs: every frame is written, from
// one state's snapshot, and its file is that state's file.
TEST_F(MidRenderEditTest, BlockingFileRenderWritesEachFrameFromItsSnapshot)
{
    const std::string fixtures = std::string(NATRON_TESTS_FIXTURES_DIR "/");
    GraphStates states;
    states.fileA = fixtures + "flat-rgb-only.exr";
    states.fileB = fixtures + "flat-rgba-only.exr";
    states.editedFileB = fixtures + "flat-rgb-only.exr";
    states.width[0] = 8;
    states.height[0] = 8;
    states.width[1] = 12;
    states.height[1] = 10;
    states.resize = kReformatParamResizeOptionDistort;
    buildGraph(states);
    if (HasFatalFailure()) {
        return;
    }

    NodePtr writer = createNativeWriter();
    ASSERT_TRUE(bool(writer));
    connectNodes(_reformat, writer, 0, true);
    KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(writer->getKnobByName("partSplitting").get());
    KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
    KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
    ASSERT_TRUE(partSplitting && bitDepth && compression);
    partSplitting->setValueFromID("single", 0);
    bitDepth->setValueFromID("32f", 0);
    compression->setValueFromID("none", 0);
    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string pattern = (tmp.path() + QLatin1String("/mid.####.exr")).toStdString();
    writer->setOutputFilesForWriter(pattern);
    const std::vector<std::string>& viewNames = getApp()->getProject()->getProjectViewNames();

    const int firstFrame = 1;
    const int lastFrame = 20;
    EditBroker broker(kStateCount - 1, 4, 4, [this](int edit) { applyState(edit + 1); });
    ASSERT_NO_FATAL_FAILURE(settleProject());
    startRecording(writer, &broker);
    BlockingBackgroundRender render(writerEffect);
    std::thread renderThread([&render, &broker, firstFrame, lastFrame]() {
        render.blockingRender(false, firstFrame, lastFrame, 1);
        appPTR->getAppTLS()->cleanupTLSForThread();
        broker.markDone();
    });
    broker.serveUntilDone();
    renderThread.join();
    stopRecording();
    EXPECT_EQ(kStateCount - 1, broker.applied());

    computeStateHashes();
    const std::vector<int> stateOfRecord = expectSnapshotsAreStates();

    // The Reads hold one still at every frame, so frame 1 stands for any frame of a state.
    std::map<int, FlatExrImage> fresh;
    std::set<int> statesSeen(stateOfRecord.begin(), stateOfRecord.end());
    for (std::set<int>::const_iterator it = statesSeen.begin(); it != statesSeen.end(); ++it) {
        if (*it < 0) {
            continue;
        }
        applyState(*it);
        freshen();
        const std::string freshPattern = (tmp.path() + QString::fromUtf8("/fresh%1.####.exr").arg(*it)).toStdString();
        writer->setOutputFilesForWriter(freshPattern);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
        getApp()->startWritersRendering(false, works);
        std::string error;
        ASSERT_TRUE(readFlatExr(SequenceParsing::generateFileNameFromPattern(freshPattern, viewNames, 1, 0), &fresh[*it], &error)) << "state " << *it << ": " << error;
    }

    // Pixels come from live knobs, not the snapshot, so a frame in flight when an edit lands may
    // carry the edit's pixels in its snapshot's layout. Only frames set up in the last state, which
    // no edit follows, are compared pixel for pixel.
    int pixelFrames = 0;
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        std::set<int> frameStates;
        for (std::size_t r = 0; r < _records.size(); ++r) {
            if (_records[r].time == frame) {
                frameStates.insert(stateOfRecord[r]);
            }
        }
        ASSERT_EQ(1u, frameStates.size()) << "frame " << frame << " was set up under " << frameStates.size() << " states";
        const int state = *frameStates.begin();
        if (state < 0) {
            continue;
        }
        FlatExrImage written;
        std::string error;
        ASSERT_TRUE(readFlatExr(SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0), &written, &error)) << "frame " << frame << ": " << error;
        const FlatExrImage& expected = fresh[state];
        EXPECT_EQ(expected.channels, written.channels) << "frame " << frame << ", state " << state;
        EXPECT_EQ(expected.x1, written.x1) << "frame " << frame;
        EXPECT_EQ(expected.y1, written.y1) << "frame " << frame;
        EXPECT_EQ(expected.width, written.width) << "frame " << frame;
        EXPECT_EQ(expected.height, written.height) << "frame " << frame;
        if (state == kStateCount - 1) {
            ++pixelFrames;
            EXPECT_TRUE(expected.pixels == written.pixels) << "frame " << frame << ", state " << state;
        }
    }
    EXPECT_GT(pixelFrames, 0);
}

// A 10-frame track, edited three times while it runs: every image the track pulls is set up under
// one state, and what it cached under that state's hash is that state's image.
TEST_F(MidRenderEditTest, TrackPullsEachFrameFromItsSnapshot)
{
    const std::string fixtures = std::string(NATRON_TESTS_FIXTURES_DIR "/");
    GraphStates states;
    // Both Switch inputs carry the tracked patch, so the track holds through the first edits.
    states.fileA = fixtures + "tracker-patch.####.exr";
    states.fileB = fixtures + "tracker-patch.####.exr";
    states.editedFileB = fixtures + "tracker-patch-diffuse.####.exr";
    states.width[0] = 128;
    states.height[0] = 128;
    states.width[1] = 144;
    states.height[1] = 136;
    states.resize = kReformatParamResizeOptionNone;
    buildGraph(states);
    if (HasFatalFailure()) {
        return;
    }

    NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
    ASSERT_TRUE(bool(tracker));
    connectNodes(_reformat, tracker, 0, true);
    TrackerContextPtr context = tracker->getTrackerContext();
    ASSERT_TRUE(bool(context));
    TrackMarkerPtr marker = context->createMarker();
    ASSERT_TRUE(bool(marker));
    KnobDoublePtr center = marker->getCenterKnob();
    center->setValue(40., ViewSpec::all(), 0);
    center->setValue(48., ViewSpec::all(), 1);

    EditBroker broker(kStateCount - 1, 2, 2, [this](int edit) { applyState(edit + 1); });
    QObject receiver;
    QObject::connect(context.get(), &TrackerContext::trackingFinished, &receiver, [&broker]() {
        broker.markDone();
    });
    ASSERT_NO_FATAL_FAILURE(settleProject());
    startRecording(tracker, &broker);
    std::list<TrackMarkerPtr> markers;
    markers.push_back(marker);
    context->trackMarkers(markers, 1, 11, 1, NULL);
    broker.serveUntilDone();
    // The tracker signals the end of tracking before it goes idle.
    const std::chrono::steady_clock::time_point idleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(kJobGuardSeconds);
    while (context->isCurrentlyTracking() && (std::chrono::steady_clock::now() < idleDeadline)) {
        QThread::msleep(1);
    }
    ASSERT_FALSE(context->isCurrentlyTracking());
    stopRecording();
    EXPECT_EQ(kStateCount - 1, broker.applied());
    ASSERT_FALSE(_records.empty());

    computeStateHashes();
    const std::vector<int> stateOfRecord = expectSnapshotsAreStates();
    expectCachedImagesMatchStates(stateOfRecord);
}

// Ten colour samples, edited three times while they run: each sample reads one state's snapshot,
// and samples what that state samples afresh.
TEST_F(MidRenderEditTest, ColourSamplesReadEachFramesSnapshot)
{
    const std::string fixtures = std::string(NATRON_TESTS_FIXTURES_DIR "/");
    GraphStates states;
    states.fileA = fixtures + "flat-rgb-only.exr";
    states.fileB = fixtures + "flat-rgba-only.exr";
    states.editedFileB = fixtures + "flat-rgba-pattern.exr";
    states.width[0] = 8;
    states.height[0] = 8;
    states.width[1] = 12;
    states.height[1] = 10;
    states.resize = kReformatParamResizeOptionDistort;
    buildGraph(states);
    if (HasFatalFailure()) {
        return;
    }

    struct Sample {
        int frame;
        bool ok;
        float rgba[4];
        std::size_t firstRecord;
        std::size_t endRecord;
        int editsBefore;
        int editsAfter;
    };
    const int sampleCount = 10;
    const RectI window(0, 0, 16, 16);
    std::vector<Sample> samples(sampleCount);
    std::thread::id samplerThreadId;

    EditBroker broker(kStateCount - 1, 2, 2, [this](int edit) { applyState(edit + 1); });
    ASSERT_NO_FATAL_FAILURE(settleProject());
    startRecording(_reformat, &broker);
    std::thread samplerThread([this, &samples, &samplerThreadId, &broker, &window]() {
        samplerThreadId = std::this_thread::get_id();
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i].frame = static_cast<int>(i) + 1;
            samples[i].firstRecord = recordCount();
            samples[i].editsBefore = broker.applied();
            samples[i].ok = sampleNodeColor(_reformat, samples[i].frame, ViewIdx(0), 0, window, ImageLayerDesc::getRGBAComponents(), samples[i].rgba);
            samples[i].endRecord = recordCount();
            samples[i].editsAfter = broker.applied();
        }
        appPTR->getAppTLS()->cleanupTLSForThread();
        broker.markDone();
    });
    broker.serveUntilDone();
    samplerThread.join();
    stopRecording();
    EXPECT_EQ(kStateCount - 1, broker.applied());

    computeStateHashes();
    const std::vector<int> stateOfRecord = expectSnapshotsAreStates();
    expectCachedImagesMatchStates(stateOfRecord);

    // Pixels come from live knobs, not the snapshot, so a sample an edit lands in may read the
    // edit's pixels. Edits land only while this thread waits in a snapshot, so every other sample
    // is compared with a fresh one.
    int comparedSamples = 0;
    for (int state = 0; state < kStateCount; ++state) {
        applyState(state);
        freshen();
        for (std::size_t i = 0; i < samples.size(); ++i) {
            std::set<int> sampleStates;
            for (std::size_t r = samples[i].firstRecord; r < samples[i].endRecord; ++r) {
                if (_records[r].thread == samplerThreadId) {
                    sampleStates.insert(stateOfRecord[r]);
                }
            }
            ASSERT_EQ(1u, sampleStates.size()) << "sample at frame " << samples[i].frame;
            if (*sampleStates.begin() != state) {
                continue;
            }
            ASSERT_TRUE(samples[i].ok) << "sample at frame " << samples[i].frame;
            if (samples[i].editsBefore != samples[i].editsAfter) {
                continue;
            }
            ++comparedSamples;
            float rgba[4];
            ASSERT_TRUE(sampleNodeColor(_reformat, samples[i].frame, ViewIdx(0), 0, window, ImageLayerDesc::getRGBAComponents(), rgba));
            for (int c = 0; c < 4; ++c) {
                EXPECT_EQ(rgba[c], samples[i].rgba[c]) << "frame " << samples[i].frame << ", state " << state << ", channel " << c;
            }
        }
    }
    EXPECT_GT(comparedSamples, 0);
}
