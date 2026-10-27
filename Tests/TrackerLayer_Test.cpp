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

#include <list>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QObject>
#include <QThread>
#include <QTimer>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/CornerPinKnobs.h"
#include "Engine/Project.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

// Tests/fixtures/tracker-patch*.####.exr: a textured 15x15 patch centred at (40, 48) on frame 1
// moves by (+6, +4) per frame. These are the centres libmv settles on for frames 2 and 3.
const double kGoldenX[2] = { 45.999671936, 51.9996948242 };
const double kGoldenY[2] = { 51.9998855591, 55.9998931885 };
const double kGoldenTolerance = 1e-4;

const int kStartFrame = 1;
const int kLastFrame = 3;

bool
hasKeyframeAt(const KnobDoublePtr& knob,
              double time)
{
    KeyFrame k;

    return knob->getCurve(ViewSpec(0), 0)->getKeyFrameWithTime(time, &k);
}

} // namespace

class TrackerLayerTest
    : public BaseTest {
protected:
    void createTrackerOverFixture(const std::string& pattern)
    {
        CreateNodeArgs readerArgs(_readPluginID.toStdString(), getApp()->getProject());
        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + pattern);
        NodePtr reader = getApp()->createNode(readerArgs);
        ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readPluginID.toStdString();

        _tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
        ASSERT_TRUE(bool(_tracker));
        connectNodes(reader, _tracker, 0, true);

        _context = _tracker->getTrackerContext();
        ASSERT_TRUE(bool(_context));

        ASSERT_FALSE(bool(_tracker->getLayerKnob()));
        ASSERT_FALSE(bool(_tracker->getKnobByName("trackRed")));

        _marker = _context->createMarker();
        ASSERT_TRUE(bool(_marker));
        KnobDoublePtr center = _marker->getCenterKnob();
        center->setValue(40., ViewSpec::all(), 0);
        center->setValue(48., ViewSpec::all(), 1);
    }

    // trackMarkers() hands the work to the tracker's scheduler thread, which reports back
    // through queued signals; pumping the event loop while polling delivers them here.
    void trackAndWait()
    {
        bool finished = false;
        QObject receiver;
        QObject::connect(_context.get(), &TrackerContext::trackingJobFinished, &receiver, [&finished]() {
            finished = true;
        });

        std::list<TrackMarkerPtr> markers;
        markers.push_back(_marker);
        _context->trackMarkers(markers, kStartFrame, kLastFrame + 1, 1, NULL);

        for (int i = 0; i < 3000 && !finished; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        ASSERT_TRUE(finished) << "tracking did not finish in time";
        // The scheduler signals the end of tracking before it goes idle.
        for (int i = 0; i < 500 && _context->isCurrentlyTracking(); ++i) {
            QThread::msleep(10);
        }
        ASSERT_FALSE(_context->isCurrentlyTracking());
    }

    void expectGoldenTrack()
    {
        KnobDoublePtr center = _marker->getCenterKnob();
        for (int frame = kStartFrame + 1; frame <= kLastFrame; ++frame) {
            EXPECT_TRUE(hasKeyframeAt(center, frame)) << "no tracked keyframe at frame " << frame;
            EXPECT_NEAR(kGoldenX[frame - kStartFrame - 1], center->getValueAtTime(frame, 0), kGoldenTolerance) << "frame " << frame;
            EXPECT_NEAR(kGoldenY[frame - kStartFrame - 1], center->getValueAtTime(frame, 1), kGoldenTolerance) << "frame " << frame;
        }
    }

    NodePtr _tracker;
    TrackerContextPtr _context;
    TrackMarkerPtr _marker;
};

TEST_F(TrackerLayerTest, ColorPlaneTracksToGolden)
{
    createTrackerOverFixture("tracker-patch.####.exr");
    if (HasFatalFailure()) {
        return;
    }

    trackAndWait();
    if (HasFatalFailure()) {
        return;
    }
    expectGoldenTrack();
}

// Listeners of trackingFinished rely on the tracker having written every key of the run by then,
// including after a cancel.
TEST_F(TrackerLayerTest, CanceledRunWritesNoKeyAfterTrackingFinished)
{
    createTrackerOverFixture("tracker-patch.####.exr");
    if (HasFatalFailure()) {
        return;
    }

    // Past the fixture's last frame the reader holds it, so every frame tracks and the run is
    // long enough to cancel partway.
    const int farEnd = 2000;
    KnobDoublePtr center = _marker->getCenterKnob();
    bool started = false;
    bool finished = false;
    int keysAtFinish = -1;
    double lastKeyAtFinish = -1.;
    QObject receiver;
    QObject::connect(_context.get(), &TrackerContext::trackingStarted, &receiver, [this, &started](int) {
        started = true;
        _context->abortTracking();
    });
    QObject::connect(_context.get(), &TrackerContext::trackingFinished, &receiver, [&]() {
        finished = true;
        keysAtFinish = center->getKeyFramesCount(ViewSpec(0), 0);
        if (!center->getLastKeyFrameTime(ViewSpec(0), 0, &lastKeyAtFinish)) {
            lastKeyAtFinish = -1.;
        }
    });

    std::list<TrackMarkerPtr> markers;
    markers.push_back(_marker);
    _context->trackMarkers(markers, kStartFrame, farEnd, 1, NULL);

    for (int i = 0; i < 3000 && !finished; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    ASSERT_TRUE(started) << "tracking never reported that it started";
    ASSERT_TRUE(finished) << "tracking did not finish in time";
    for (int i = 0; i < 500 && _context->isCurrentlyTracking(); ++i) {
        QThread::msleep(10);
    }
    ASSERT_FALSE(_context->isCurrentlyTracking());
    for (int i = 0; i < 30; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }

    EXPECT_GT(keysAtFinish, 0);
    EXPECT_LT(lastKeyAtFinish, (double)farEnd - 1) << "the cancel did not stop the run";
    double lastKey = -1.;
    if (!center->getLastKeyFrameTime(ViewSpec(0), 0, &lastKey)) {
        lastKey = -1.;
    }
    EXPECT_EQ(keysAtFinish, center->getKeyFramesCount(ViewSpec(0), 0)) << "keys were written after trackingFinished";
    EXPECT_EQ(lastKeyAtFinish, lastKey) << "keys were written after trackingFinished";
}

namespace {

// Every key time and value of each dimension of a knob, to tell whether anything wrote to it.
std::vector<std::pair<double, double>>
keysOf(const KnobDoublePtr& knob)
{
    std::vector<std::pair<double, double>> ret;
    for (int d = 0; d < knob->getDimension(); ++d) {
        CurvePtr curve = knob->getCurve(ViewSpec(0), d);
        if (curve) {
            for (const KeyFrame& k : curve->getKeyFrames_mt_safe()) {
                ret.push_back(std::make_pair(k.getTime(), k.getValue()));
            }
        }
        ret.push_back(std::make_pair(-1., knob->getValue(d)));
    }

    return ret;
}

} // namespace

// The foreground dialog over a tracking run closes on trackingJobFinished, so the transform solve
// that follows the run, cancelled or not, must have written its last value by then.
TEST_F(TrackerLayerTest, TransformSolveEndsBeforeTrackingJobFinished)
{
    createTrackerOverFixture("tracker-patch.####.exr");
    if (HasFatalFailure()) {
        return;
    }
    // Automatic solving off, so that setting the motion type starts no solve of its own.
    KnobBoolPtr autoSolve = std::dynamic_pointer_cast<KnobBool>(_tracker->getKnobByName("autoComputeransform"));
    KnobChoicePtr motionType = std::dynamic_pointer_cast<KnobChoice>(_tracker->getKnobByName("motionType"));
    KnobChoicePtr transformType = std::dynamic_pointer_cast<KnobChoice>(_tracker->getKnobByName("transformType"));
    ASSERT_TRUE(autoSolve && motionType && transformType);
    autoSolve->setValue(false);
    // Transform, Match-Move.
    transformType->setValue(0);
    motionType->setValue(2);

    const char* const solvedNames[] = { "translate", "rotate", "scale", "center" };
    std::vector<KnobDoublePtr> solved;
    for (const char* name : solvedNames) {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(name));
        ASSERT_TRUE(bool(knob)) << name;
        solved.push_back(knob);
    }
    KnobDoublePtr translate = solved[0];

    const int farEnd = 2000;
    bool finished = false;
    bool jobFinished = false;
    bool jobFinishedBeforeFinished = false;
    int translateKeysAtJobEnd = -1;
    std::vector<std::vector<std::pair<double, double>>> atJobEnd;
    QObject receiver;
    // Cancelled after a few frames, so the solve runs after a cancel over the keys written so far.
    QObject::connect(_context.get(), &TrackerContext::trackingStarted, &receiver, [this](int) {
        QTimer::singleShot(100, _context.get(), [this]() {
            _context->abortTracking();
        });
    });
    QObject::connect(_context.get(), &TrackerContext::trackingFinished, &receiver, [&]() {
        finished = true;
    });
    QObject::connect(_context.get(), &TrackerContext::trackingJobFinished, &receiver, [&]() {
        jobFinishedBeforeFinished = !finished;
        jobFinished = true;
        translateKeysAtJobEnd = translate->getKeyFramesCount(ViewSpec(0), 0);
        for (const KnobDoublePtr& knob : solved) {
            atJobEnd.push_back(keysOf(knob));
        }
    });

    std::list<TrackMarkerPtr> markers;
    markers.push_back(_marker);
    _context->trackMarkers(markers, kStartFrame, farEnd, 1, NULL);

    for (int i = 0; i < 3000 && !jobFinished; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    ASSERT_TRUE(jobFinished) << "the tracking job did not finish in time";
    EXPECT_TRUE(finished);
    EXPECT_FALSE(jobFinishedBeforeFinished) << "trackingJobFinished came before trackingFinished";
    EXPECT_GT(translateKeysAtJobEnd, 0) << "the solve had not written the translation when the job finished";

    for (int i = 0; i < 100; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    EXPECT_FALSE(_context->isCurrentlyTracking());
    ASSERT_EQ(solved.size(), atJobEnd.size());
    for (std::size_t i = 0; i < solved.size(); ++i) {
        EXPECT_EQ(atJobEnd[i], keysOf(solved[i])) << solvedNames[i] << " was written after trackingJobFinished";
    }
}

namespace {

// The tracker's knobs that drive its internal CornerPin: the tracker-side name, the name of the
// CornerPin knob it drives, and whether the tracker knob is the internal Transform's alias, which
// the CornerPin knob is slaved to, rather than the CornerPin knob's own alias.
struct TrackerCornerPinLink {
    const char* trackerName;
    const char* cornerPinName;
    bool sharedWithTransform;
};

const TrackerCornerPinLink kCornerPinLinks[] = {
    { kCornerPinNodeParamFrom1, kCornerPinNodeParamFrom1, false },
    { kCornerPinNodeParamFrom2, kCornerPinNodeParamFrom2, false },
    { kCornerPinNodeParamFrom3, kCornerPinNodeParamFrom3, false },
    { kCornerPinNodeParamFrom4, kCornerPinNodeParamFrom4, false },
    { kCornerPinNodeParamTo1, kCornerPinNodeParamTo1, false },
    { kCornerPinNodeParamTo2, kCornerPinNodeParamTo2, false },
    { kCornerPinNodeParamTo3, kCornerPinNodeParamTo3, false },
    { kCornerPinNodeParamTo4, kCornerPinNodeParamTo4, false },
    { kCornerPinNodeParamEnable1, kCornerPinNodeParamEnable1, false },
    { kCornerPinNodeParamEnable2, kCornerPinNodeParamEnable2, false },
    { kCornerPinNodeParamEnable3, kCornerPinNodeParamEnable3, false },
    { kCornerPinNodeParamEnable4, kCornerPinNodeParamEnable4, false },
    { kCornerPinNodeParamOverlayPoints, kCornerPinNodeParamOverlayPoints, false },
    { kCornerPinNodeParamExtraMatrix, kCornerPinNodeParamExtraMatrix, false },
    // Shared with the internal Transform, so these carry its script names.
    { "invert", kCornerPinNodeParamInvert, true },
    { "filter", kCornerPinNodeParamFilter, true },
    { "clamp", kCornerPinNodeParamClamp, true },
    { "black_outside", kCornerPinNodeParamBlackOutside, true },
    { "motionBlur", kCornerPinNodeParamMotionBlur, true },
    { "shutter", kCornerPinNodeParamShutter, true },
    { "shutterOffset", kCornerPinNodeParamShutterOffset, true },
    { "shutterCustomOffset", kCornerPinNodeParamShutterCustomOffset, true },
};

const char* const kToNames[4] = { kCornerPinNodeParamTo1, kCornerPinNodeParamTo2, kCornerPinNodeParamTo3, kCornerPinNodeParamTo4 };
const char* const kFromNames[4] = { kCornerPinNodeParamFrom1, kCornerPinNodeParamFrom2, kCornerPinNodeParamFrom3, kCornerPinNodeParamFrom4 };
const char* const kEnableNames[4] = { kCornerPinNodeParamEnable1, kCornerPinNodeParamEnable2, kCornerPinNodeParamEnable3, kCornerPinNodeParamEnable4 };

// The tracker's knobs duplicated from its internal Transform, under the Transform's own names.
const char* const kTransformLinks[] = {
    "translate",
    "rotate",
    "scale",
    "uniform",
    "skewX",
    "skewY",
    "skewOrder",
    "center",
    "invert",
    "filter",
    "clamp",
    "black_outside",
    "motionBlur",
    "shutter",
    "shutterOffset",
    "shutterCustomOffset",
};

// Row-major, not the identity, so a dropped or transposed copy shows.
const double kExtraMatrix[9] = { 1.5, 0.25, 3., -0.5, 2., 7., 0.001, 0.002, 1. };

// Indices into the tracker's Transform Type and Motion Type choices.
const int kTransformTypeTransform = 0;
const int kTransformTypeCornerPin = 1;
const int kMotionTypeMatchMove = 2;

std::set<NodePtr>
cornerPinsIn(const AppInstancePtr& app)
{
    std::set<NodePtr> ret;
    for (const NodePtr& node : app->getProject()->getNodes()) {
        if (node->getPluginID() == PLUGINID_NATRON_CORNERPIN) {
            ret.insert(node);
        }
    }

    return ret;
}

} // namespace

class TrackerCornerPinTest
    : public BaseTest {
protected:
    void createTracker()
    {
        _tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
        ASSERT_TRUE(bool(_tracker));
        _context = _tracker->getTrackerContext();
        ASSERT_TRUE(bool(_context));

        KnobChoicePtr transformType = std::dynamic_pointer_cast<KnobChoice>(_tracker->getKnobByName("transformType"));
        ASSERT_TRUE(bool(transformType));
        transformType->setValue(kTransformTypeCornerPin);
        _cornerPin = _context->getCurrentlySelectedTransformNode();
        ASSERT_TRUE(bool(_cornerPin));
    }

    // Keys the tracker's "to" points the way the CornerPin solver does: one key per frame.
    void animateTrackerToPoints()
    {
        for (int i = 0; i < 4; ++i) {
            KnobDoublePtr to = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kToNames[i]));
            ASSERT_TRUE(bool(to)) << kToNames[i];
            for (int frame = 1; frame <= 3; ++frame) {
                to->setValueAtTime(frame, 10. * i + 3. * frame, ViewSpec::all(), 0);
                to->setValueAtTime(frame, 20. * i - 2. * frame, ViewSpec::all(), 1);
            }
        }
        KnobBoolPtr enable4 = std::dynamic_pointer_cast<KnobBool>(_tracker->getKnobByName(kCornerPinNodeParamEnable4));
        ASSERT_TRUE(bool(enable4));
        enable4->setValue(false);
    }

    NodePtr exportCornerPin(bool linked)
    {
        KnobChoicePtr motionType = std::dynamic_pointer_cast<KnobChoice>(_tracker->getKnobByName("motionType"));
        KnobBoolPtr exportLink = std::dynamic_pointer_cast<KnobBool>(_tracker->getKnobByName("exportLink"));
        EXPECT_TRUE(motionType && exportLink);
        if (!motionType || !exportLink) {
            return NodePtr();
        }
        motionType->setValue(kMotionTypeMatchMove);
        exportLink->setValue(linked);

        const std::set<NodePtr> before = cornerPinsIn(getApp());
        _context->exportTrackDataFromExportOptions();
        const std::set<NodePtr> after = cornerPinsIn(getApp());

        NodePtr created;
        for (const NodePtr& node : after) {
            if (!before.count(node)) {
                EXPECT_FALSE(bool(created)) << "the export created more than one CornerPin";
                created = node;
            }
        }

        return created;
    }

    void expectToPointsFollowTracker(const NodePtr& exported)
    {
        for (int i = 0; i < 4; ++i) {
            KnobDoublePtr trackerTo = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kToNames[i]));
            KnobDoublePtr exportedTo = std::dynamic_pointer_cast<KnobDouble>(exported->getKnobByName(kToNames[i]));
            ASSERT_TRUE(trackerTo && exportedTo) << kToNames[i];
            for (int frame = 1; frame <= 3; ++frame) {
                for (int d = 0; d < 2; ++d) {
                    EXPECT_DOUBLE_EQ(trackerTo->getValueAtTime(frame, d), exportedTo->getValueAtTime(frame, d)) << kToNames[i] << " frame " << frame << " dim " << d;
                }
            }
        }
        KnobBoolPtr exportedEnable4 = std::dynamic_pointer_cast<KnobBool>(exported->getKnobByName(kCornerPinNodeParamEnable4));
        ASSERT_TRUE(bool(exportedEnable4));
        EXPECT_FALSE(exportedEnable4->getValue());
    }

    void setTrackerExtraMatrix()
    {
        KnobDoublePtr matrix = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kCornerPinNodeParamExtraMatrix));
        ASSERT_TRUE(bool(matrix));
        ASSERT_EQ(9, matrix->getDimension());
        for (int d = 0; d < 9; ++d) {
            matrix->setValue(kExtraMatrix[d], ViewSpec::all(), d);
        }
    }

    void expectExtraMatrixCopied(const NodePtr& exported)
    {
        KnobDoublePtr matrix = std::dynamic_pointer_cast<KnobDouble>(exported->getKnobByName(kCornerPinNodeParamExtraMatrix));
        ASSERT_TRUE(bool(matrix));
        ASSERT_EQ(9, matrix->getDimension());
        for (int d = 0; d < 9; ++d) {
            EXPECT_DOUBLE_EQ(kExtraMatrix[d], matrix->getValue(d)) << "dim " << d;
        }
    }

    // The CornerPin solve runs on the global thread pool and lands through a queued signal.
    void solveAndWait()
    {
        KnobDoublePtr to1 = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kCornerPinNodeParamTo1));
        ASSERT_TRUE(bool(to1));
        _context->solveTransformParams();
        for (int i = 0; i < 3000 && !hasKeyframeAt(to1, 3); ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        ASSERT_TRUE(hasKeyframeAt(to1, 3)) << "the CornerPin solve did not finish in time";
    }

    NodePtr _tracker;
    TrackerContextPtr _context;
    NodePtr _cornerPin;
};

// The internal CornerPin is the native node, built in, so a tracker needs no OpenFX bundle.
TEST_F(TrackerCornerPinTest, InternalCornerPinIsNative)
{
    createTracker();
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(std::string(PLUGINID_NATRON_CORNERPIN), _cornerPin->getPluginID());
    EXPECT_FALSE(_cornerPin->getEffectInstance()->isOpenFX());
}

TEST_F(TrackerCornerPinTest, DuplicatedKnobsDriveInternalCornerPin)
{
    createTracker();
    if (HasFatalFailure()) {
        return;
    }

    for (const TrackerCornerPinLink& link : kCornerPinLinks) {
        KnobIPtr trackerKnob = _tracker->getKnobByName(link.trackerName);
        KnobIPtr cornerPinKnob = _cornerPin->getKnobByName(link.cornerPinName);
        ASSERT_TRUE(bool(trackerKnob)) << "tracker has no " << link.trackerName;
        ASSERT_TRUE(bool(cornerPinKnob)) << "CornerPin has no " << link.cornerPinName;
        if (link.sharedWithTransform) {
            for (int d = 0; d < cornerPinKnob->getDimension(); ++d) {
                EXPECT_EQ(trackerKnob, cornerPinKnob->getMaster(d).second) << link.cornerPinName << " dim " << d << " is not slaved to the tracker's " << link.trackerName;
            }
            continue;
        }
        EXPECT_EQ(trackerKnob, cornerPinKnob->getAliasMaster()) << link.cornerPinName << " is not an alias of the tracker's " << link.trackerName;
        for (int d = 0; d < cornerPinKnob->getDimension(); ++d) {
            EXPECT_TRUE(cornerPinKnob->isLinkPermanent(d)) << link.cornerPinName << " dim " << d;
            EXPECT_EQ(trackerKnob->getLinkGroup(d), cornerPinKnob->getLinkGroup(d)) << link.cornerPinName << " dim " << d << " is not driven by the tracker's " << link.trackerName;
        }
    }

    EXPECT_TRUE(bool(_tracker->getKnobByName(kCornerPinNodeParamToGroup)));
    EXPECT_TRUE(bool(_tracker->getKnobByName(kCornerPinNodeParamFromGroup)));

    KnobDoublePtr trackerTo1 = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kCornerPinNodeParamTo1));
    KnobDoublePtr cornerPinTo1 = std::dynamic_pointer_cast<KnobDouble>(_cornerPin->getKnobByName(kCornerPinNodeParamTo1));
    ASSERT_TRUE(trackerTo1 && cornerPinTo1);
    trackerTo1->setValue(123.5, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(123.5, cornerPinTo1->getValue(0));

    KnobDoublePtr trackerShutter = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName("shutter"));
    KnobDoublePtr cornerPinShutter = std::dynamic_pointer_cast<KnobDouble>(_cornerPin->getKnobByName(kCornerPinNodeParamShutter));
    ASSERT_TRUE(trackerShutter && cornerPinShutter);
    trackerShutter->setValue(0.25, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.25, cornerPinShutter->getValue(0));
}

TEST_F(TrackerCornerPinTest, DuplicatedKnobsDriveInternalTransform)
{
    createTracker();
    if (HasFatalFailure()) {
        return;
    }
    KnobChoicePtr transformType = std::dynamic_pointer_cast<KnobChoice>(_tracker->getKnobByName("transformType"));
    ASSERT_TRUE(bool(transformType));
    transformType->setValue(kTransformTypeTransform);
    NodePtr transform = _context->getCurrentlySelectedTransformNode();
    ASSERT_TRUE(bool(transform));
    ASSERT_NE(_cornerPin, transform);

    for (const char* name : kTransformLinks) {
        KnobIPtr trackerKnob = _tracker->getKnobByName(name);
        KnobIPtr transformKnob = transform->getKnobByName(name);
        ASSERT_TRUE(bool(trackerKnob)) << "tracker has no " << name;
        ASSERT_TRUE(bool(transformKnob)) << "Transform has no " << name;
        EXPECT_EQ(trackerKnob, transformKnob->getAliasMaster()) << name << " is not an alias of the tracker's knob";
        for (int d = 0; d < transformKnob->getDimension(); ++d) {
            EXPECT_TRUE(transformKnob->isLinkPermanent(d)) << name << " dim " << d;
            EXPECT_EQ(trackerKnob->getLinkGroup(d), transformKnob->getLinkGroup(d)) << name << " dim " << d << " is not driven by the tracker";
        }
    }
}

// Tracks a single marker that only translates and solves the CornerPin from it, so every "to"
// point is its "from" point moved by the marker's offset from the reference frame.
TEST_F(TrackerCornerPinTest, SolvedTrackDrivesExportedCornerPin)
{
    createTracker();
    if (HasFatalFailure()) {
        return;
    }
    // Off, so the knob edits below do not start solves of their own that race the explicit one.
    KnobBoolPtr autoSolve = std::dynamic_pointer_cast<KnobBool>(_tracker->getKnobByName("autoComputeransform"));
    ASSERT_TRUE(bool(autoSolve));
    autoSolve->setValue(false);
    KnobChoicePtr motionType = std::dynamic_pointer_cast<KnobChoice>(_tracker->getKnobByName("motionType"));
    ASSERT_TRUE(bool(motionType));
    motionType->setValue(kMotionTypeMatchMove);
    ASSERT_EQ(1, _context->getTransformReferenceFrame());

    const double from[4][2] = { { 0., 0. }, { 100., 0. }, { 100., 80. }, { 0., 80. } };
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr fromKnob = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kFromNames[i]));
        ASSERT_TRUE(bool(fromKnob)) << kFromNames[i];
        fromKnob->setValue(from[i][0], ViewSpec::all(), 0);
        fromKnob->setValue(from[i][1], ViewSpec::all(), 1);
    }

    const double track[3][2] = { { 40., 48. }, { 46.5, 51. }, { 53., 47.25 } };
    TrackMarkerPtr marker = _context->createMarker();
    ASSERT_TRUE(bool(marker));
    KnobDoublePtr center = marker->getCenterKnob();
    for (int frame = 1; frame <= 3; ++frame) {
        center->setValueAtTime(frame, track[frame - 1][0], ViewSpec::all(), 0);
        center->setValueAtTime(frame, track[frame - 1][1], ViewSpec::all(), 1);
    }

    solveAndWait();
    if (HasFatalFailure()) {
        return;
    }

    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr trackerTo = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kToNames[i]));
        ASSERT_TRUE(bool(trackerTo)) << kToNames[i];
        for (int frame = 1; frame <= 3; ++frame) {
            for (int d = 0; d < 2; ++d) {
                const double expected = from[i][d] + track[frame - 1][d] - track[0][d];
                EXPECT_NEAR(expected, trackerTo->getValueAtTime(frame, d), 1e-9) << kToNames[i] << " frame " << frame << " dim " << d;
            }
        }
    }

    KnobBoolPtr enable4 = std::dynamic_pointer_cast<KnobBool>(_tracker->getKnobByName(kCornerPinNodeParamEnable4));
    ASSERT_TRUE(bool(enable4));
    enable4->setValue(false);

    NodePtr exported = exportCornerPin(true);
    ASSERT_TRUE(bool(exported));
    expectToPointsFollowTracker(exported);
}

TEST_F(TrackerCornerPinTest, LinkedExportFollowsTrack)
{
    createTracker();
    if (HasFatalFailure()) {
        return;
    }
    animateTrackerToPoints();
    if (HasFatalFailure()) {
        return;
    }
    setTrackerExtraMatrix();
    if (HasFatalFailure()) {
        return;
    }

    NodePtr exported = exportCornerPin(true);
    ASSERT_TRUE(bool(exported));
    EXPECT_FALSE(exported->getEffectInstance()->isOpenFX());
    expectToPointsFollowTracker(exported);
    if (HasFatalFailure()) {
        return;
    }
    expectExtraMatrixCopied(exported);

    KnobDoublePtr trackerTo2 = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kCornerPinNodeParamTo2));
    KnobDoublePtr exportedTo2 = std::dynamic_pointer_cast<KnobDouble>(exported->getKnobByName(kCornerPinNodeParamTo2));
    for (int d = 0; d < 2; ++d) {
        EXPECT_EQ(KnobIPtr(trackerTo2), exportedTo2->getMaster(d).second);
    }
    trackerTo2->setValueAtTime(2, -7., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(-7., exportedTo2->getValueAtTime(2, 0));

    KnobDoublePtr trackerMatrix = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kCornerPinNodeParamExtraMatrix));
    KnobDoublePtr exportedMatrix = std::dynamic_pointer_cast<KnobDouble>(exported->getKnobByName(kCornerPinNodeParamExtraMatrix));
    ASSERT_TRUE(trackerMatrix && exportedMatrix);
    for (int d = 0; d < 9; ++d) {
        EXPECT_EQ(KnobIPtr(trackerMatrix), exportedMatrix->getMaster(d).second) << "dim " << d;
        trackerMatrix->setValue(kExtraMatrix[d] + 0.25 * (d + 1), ViewSpec::all(), d);
    }
    for (int d = 0; d < 9; ++d) {
        EXPECT_DOUBLE_EQ(kExtraMatrix[d] + 0.25 * (d + 1), exportedMatrix->getValue(d)) << "dim " << d;
    }

    const int refFrame = _context->getTransformReferenceFrame();
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr trackerFrom = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kFromNames[i]));
        KnobDoublePtr exportedFrom = std::dynamic_pointer_cast<KnobDouble>(exported->getKnobByName(kFromNames[i]));
        ASSERT_TRUE(trackerFrom && exportedFrom) << kFromNames[i];
        for (int d = 0; d < 2; ++d) {
            EXPECT_DOUBLE_EQ(trackerFrom->getValueAtTime(refFrame, d), exportedFrom->getValue(d)) << kFromNames[i] << " dim " << d;
        }
    }
}

TEST_F(TrackerCornerPinTest, CopiedExportFollowsTrack)
{
    createTracker();
    if (HasFatalFailure()) {
        return;
    }
    animateTrackerToPoints();
    if (HasFatalFailure()) {
        return;
    }
    setTrackerExtraMatrix();
    if (HasFatalFailure()) {
        return;
    }

    NodePtr exported = exportCornerPin(false);
    ASSERT_TRUE(bool(exported));
    expectToPointsFollowTracker(exported);
    expectExtraMatrixCopied(exported);

    KnobDoublePtr exportedTo1 = std::dynamic_pointer_cast<KnobDouble>(exported->getKnobByName(kCornerPinNodeParamTo1));
    ASSERT_TRUE(bool(exportedTo1));
    EXPECT_FALSE(bool(exportedTo1->getMaster(0).second));
    KnobIPtr exportedMatrix = exported->getKnobByName(kCornerPinNodeParamExtraMatrix);
    ASSERT_TRUE(bool(exportedMatrix));
    for (int d = 0; d < 9; ++d) {
        EXPECT_EQ(nullptr, exportedMatrix->getMaster(d).second.get()) << "matrix dim " << d;
    }
    for (int i = 0; i < 4; ++i) {
        KnobIPtr exportedEnable = exported->getKnobByName(kEnableNames[i]);
        ASSERT_TRUE(bool(exportedEnable)) << kEnableNames[i];
        EXPECT_EQ(nullptr, exportedEnable->getMaster(0).second.get()) << kEnableNames[i];
    }
}
