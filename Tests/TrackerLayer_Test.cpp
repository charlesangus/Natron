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
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

// Tests/fixtures/tracker-patch*.####.exr: a textured 15x15 patch centred at (40, 48) on frame 1
// moves by (+6, +4) per frame. These are the centres libmv settles on for frames 2 and 3 from
// the Color plane; a layer that carries the same pixels must land on the same values.
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
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + pattern);
        NodePtr reader = getApp()->createNode(readerArgs);
        ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

        _tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
        ASSERT_TRUE(bool(_tracker));
        connectNodes(reader, _tracker, 0, true);

        _context = _tracker->getTrackerContext();
        ASSERT_TRUE(bool(_context));

        _layer = std::dynamic_pointer_cast<KnobLayerSelect>(_tracker->getLayerKnob());
        ASSERT_TRUE(bool(_layer)) << "the Tracker has no layer select knob";
        ASSERT_EQ(std::string(kNodeParamLayerSelect), _layer->getName());
        ASSERT_FALSE(_layer->getWithChannelButtons());
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
    KnobLayerSelectPtr _layer;
};

TEST_F(TrackerLayerTest, ColorPlaneTracksToGolden)
{
    createTrackerOverFixture("tracker-patch.####.exr");
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(std::string(kNatronColorLayerID), _layer->getLayer());

    trackAndWait();
    if (HasFatalFailure()) {
        return;
    }
    expectGoldenTrack();
}

TEST_F(TrackerLayerTest, DiffuseLayerWithTheSamePixelsTracksToGolden)
{
    createTrackerOverFixture("tracker-patch.####.exr");
    if (HasFatalFailure()) {
        return;
    }
    _layer->setLayer("diffuse");

    trackAndWait();
    if (HasFatalFailure()) {
        return;
    }
    expectGoldenTrack();
}

// The Color plane of this sequence is flat: only the diffuse layer carries the patch, so
// landing on the golden centres proves the tracker read the selected layer, not Color.
TEST_F(TrackerLayerTest, DiffuseLayerIsReadWhenColorIsFlat)
{
    createTrackerOverFixture("tracker-patch-diffuse.####.exr");
    if (HasFatalFailure()) {
        return;
    }
    _layer->setLayer("diffuse");

    trackAndWait();
    if (HasFatalFailure()) {
        return;
    }
    expectGoldenTrack();
}

TEST_F(TrackerLayerTest, AbsentLayerTracksNothing)
{
    createTrackerOverFixture("tracker-patch.####.exr");
    if (HasFatalFailure()) {
        return;
    }
    _layer->setLayer("specular");

    trackAndWait();
    if (HasFatalFailure()) {
        return;
    }
    KnobDoublePtr center = _marker->getCenterKnob();
    EXPECT_TRUE(hasKeyframeAt(center, kStartFrame));
    for (int frame = kStartFrame + 1; frame <= kLastFrame; ++frame) {
        EXPECT_FALSE(hasKeyframeAt(center, frame)) << "frame " << frame << " was tracked from a layer the source does not carry";
    }
}
