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

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QObject>
#include <QThread>

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
        QObject::connect(_context.get(), &TrackerContext::trackingFinished, &receiver, [&finished]() {
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

namespace {

// The tracker's knobs that drive its internal CornerPin: the tracker-side name, and the name of
// the CornerPin knob it is linked to.
struct TrackerCornerPinLink {
    const char* trackerName;
    const char* cornerPinName;
};

const TrackerCornerPinLink kCornerPinLinks[] = {
    { kCornerPinNodeParamFrom1, kCornerPinNodeParamFrom1 },
    { kCornerPinNodeParamFrom2, kCornerPinNodeParamFrom2 },
    { kCornerPinNodeParamFrom3, kCornerPinNodeParamFrom3 },
    { kCornerPinNodeParamFrom4, kCornerPinNodeParamFrom4 },
    { kCornerPinNodeParamTo1, kCornerPinNodeParamTo1 },
    { kCornerPinNodeParamTo2, kCornerPinNodeParamTo2 },
    { kCornerPinNodeParamTo3, kCornerPinNodeParamTo3 },
    { kCornerPinNodeParamTo4, kCornerPinNodeParamTo4 },
    { kCornerPinNodeParamEnable1, kCornerPinNodeParamEnable1 },
    { kCornerPinNodeParamEnable2, kCornerPinNodeParamEnable2 },
    { kCornerPinNodeParamEnable3, kCornerPinNodeParamEnable3 },
    { kCornerPinNodeParamEnable4, kCornerPinNodeParamEnable4 },
    { kCornerPinNodeParamOverlayPoints, kCornerPinNodeParamOverlayPoints },
    { kCornerPinNodeParamExtraMatrix, kCornerPinNodeParamExtraMatrix },
    // Shared with the internal Transform, so these carry its script names.
    { "invert", kCornerPinNodeParamInvert },
    { "filter", kCornerPinNodeParamFilter },
    { "clamp", kCornerPinNodeParamClamp },
    { "black_outside", kCornerPinNodeParamBlackOutside },
    { "motionBlur", kCornerPinNodeParamMotionBlur },
    { "shutter", kCornerPinNodeParamShutter },
    { "shutterOffset", kCornerPinNodeParamShutterOffset },
    { "shutterCustomOffset", kCornerPinNodeParamShutterCustomOffset },
};

const char* const kToNames[4] = { kCornerPinNodeParamTo1, kCornerPinNodeParamTo2, kCornerPinNodeParamTo3, kCornerPinNodeParamTo4 };
const char* const kFromNames[4] = { kCornerPinNodeParamFrom1, kCornerPinNodeParamFrom2, kCornerPinNodeParamFrom3, kCornerPinNodeParamFrom4 };
const char* const kEnableNames[4] = { kCornerPinNodeParamEnable1, kCornerPinNodeParamEnable2, kCornerPinNodeParamEnable3, kCornerPinNodeParamEnable4 };

// Indices into the tracker's Transform Type and Motion Type choices.
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
        for (int d = 0; d < cornerPinKnob->getDimension(); ++d) {
            EXPECT_EQ(trackerKnob, cornerPinKnob->getMaster(d).second) << link.cornerPinName << " dim " << d << " is not driven by the tracker's " << link.trackerName;
        }
    }

    KnobDoublePtr trackerTo1 = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kCornerPinNodeParamTo1));
    KnobDoublePtr cornerPinTo1 = std::dynamic_pointer_cast<KnobDouble>(_cornerPin->getKnobByName(kCornerPinNodeParamTo1));
    ASSERT_TRUE(trackerTo1 && cornerPinTo1);
    trackerTo1->setValue(123.5, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(123.5, cornerPinTo1->getValue(0));
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

    NodePtr exported = exportCornerPin(true);
    ASSERT_TRUE(bool(exported));
    EXPECT_FALSE(exported->getEffectInstance()->isOpenFX());
    expectToPointsFollowTracker(exported);
    if (HasFatalFailure()) {
        return;
    }

    KnobDoublePtr trackerTo2 = std::dynamic_pointer_cast<KnobDouble>(_tracker->getKnobByName(kCornerPinNodeParamTo2));
    KnobDoublePtr exportedTo2 = std::dynamic_pointer_cast<KnobDouble>(exported->getKnobByName(kCornerPinNodeParamTo2));
    for (int d = 0; d < 2; ++d) {
        EXPECT_EQ(KnobIPtr(trackerTo2), exportedTo2->getMaster(d).second);
    }
    trackerTo2->setValueAtTime(2, -7., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(-7., exportedTo2->getValueAtTime(2, 0));

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

    NodePtr exported = exportCornerPin(false);
    ASSERT_TRUE(bool(exported));
    expectToPointsFollowTracker(exported);

    KnobDoublePtr exportedTo1 = std::dynamic_pointer_cast<KnobDouble>(exported->getKnobByName(kCornerPinNodeParamTo1));
    ASSERT_TRUE(bool(exportedTo1));
    EXPECT_FALSE(bool(exportedTo1->getMaster(0).second));
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(nullptr, exported->getKnobByName(kEnableNames[i])->getMaster(0).second.get()) << kEnableNames[i];
    }
}
