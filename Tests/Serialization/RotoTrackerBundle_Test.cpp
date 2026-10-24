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

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "FlatExrReader.h"
#include "ProjectEquivalence.h"
#include "YamlTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

#include "Serialization/Bundle/ZipStore.h"
#include "Serialization/CurveSerialization.h"
#include "Serialization/RotoSerialization.h"
#include "Serialization/Yaml/CurveYaml.h"

NATRON_NAMESPACE_USING

namespace {

const int kTrackFrames = 500;
const int kRenderedFrames[] = { 1, 9 };

// The square's shape is keyed on every frame of this range, so that its shape table, a row per
// key and control point, is a blob member rather than inline.
const int kShapeFirstFrame = 1;
const int kShapeLastFrame = 16;

// Enough control points for the polygon's point table to be a blob member on its own.
const int kPolygonPoints = 64;

std::string
exact(double value)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.17g/0x%016llx", value, (unsigned long long)YamlTest::bitsOf(value));

    return std::string(text);
}

// The curve as the format keeps it, every double as its bit pattern: derivatives count only for
// free and broken keyframes, the others are recomputed on load.
std::string
storedCurveText(const Serialization::CurveSerialization& curve)
{
    const Serialization::CurveSerialization stored = Serialization::canonicalCurveForYaml(curve);
    std::string text = std::to_string(stored.keyframes.size()) + " keys";
    for (const Serialization::KeyFrameSerialization& key : stored.keyframes) {
        text += " [t=" + exact(key.time) + " v=" + exact(key.value) + " l=" + exact(key.leftDerivative) + " r=" + exact(key.rightDerivative) + " lw=" + exact(key.leftWeight) + " rw=" + exact(key.rightWeight) + " i=" + std::to_string(key.interpolation) + "]";
    }

    return text;
}

std::string
storedCurveText(const CurvePtr& curve)
{
    Serialization::CurveSerialization serialization;
    if (curve) {
        curve->toSerialization(&serialization);
    }

    return storedCurveText(serialization);
}

std::string
itemTree(const RotoLayerPtr& layer)
{
    std::string text = "layer " + layer->getScriptName() + " {";
    const std::list<RotoItemPtr> items = layer->getItems_mt_safe();
    for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
        if (RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it)) {
            text += " " + itemTree(isLayer);
        } else if (std::dynamic_pointer_cast<RotoStrokeItem>(*it)) {
            text += " stroke " + (*it)->getScriptName();
        } else if (std::dynamic_pointer_cast<Bezier>(*it)) {
            text += " bezier " + (*it)->getScriptName();
        } else {
            text += " item " + (*it)->getScriptName();
        }
    }

    return text + " }";
}

void
addPoints(ProjectSnapshot* state,
          const std::string& prefix,
          const std::list<BezierCPPtr>& points)
{
    state->entries[prefix + " count"] = std::to_string(points.size());
    int index = 0;
    for (std::list<BezierCPPtr>::const_iterator it = points.begin(); it != points.end(); ++it, ++index) {
        const std::string key = prefix + " " + std::to_string(index);
        state->entries[key + " x"] = storedCurveText((*it)->getXCurve());
        state->entries[key + " y"] = storedCurveText((*it)->getYCurve());
        state->entries[key + " lx"] = storedCurveText((*it)->getLeftXCurve());
        state->entries[key + " ly"] = storedCurveText((*it)->getLeftYCurve());
        state->entries[key + " rx"] = storedCurveText((*it)->getRightXCurve());
        state->entries[key + " ry"] = storedCurveText((*it)->getRightYCurve());
        for (int frame : kRenderedFrames) {
            double x = 0., y = 0.;
            (*it)->getPositionAtTime(false, frame, ViewIdx(0), &x, &y);
            state->entries[key + " at " + std::to_string(frame)] = exact(x) + " " + exact(y);
        }
    }
}

void
addRotoItems(ProjectSnapshot* state,
             const RotoLayerPtr& layer)
{
    const std::list<RotoItemPtr> items = layer->getItems_mt_safe();
    for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
        const std::string name = (*it)->getScriptName();
        if (RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it)) {
            addRotoItems(state, isLayer);
        } else if (RotoStrokeItemPtr stroke = std::dynamic_pointer_cast<RotoStrokeItem>(*it)) {
            Serialization::RotoStrokeItemSerialization data;
            stroke->toSerialization(&data);
            state->entries["stroke " + name + " brush"] = std::to_string(data.brushType);
            state->entries["stroke " + name + " segments"] = std::to_string(data.strokes.size());
            int segment = 0;
            for (const Serialization::RotoStrokeCurvesSerialization& curves : data.strokes) {
                const std::string key = "stroke " + name + " segment " + std::to_string(segment++);
                state->entries[key + " x"] = storedCurveText(curves.xCurve);
                state->entries[key + " y"] = storedCurveText(curves.yCurve);
                state->entries[key + " pressure"] = storedCurveText(curves.pressureCurve);
            }
        } else if (BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it)) {
            std::set<double> times;
            bezier->getKeyframeTimes(&times);
            std::string timesText;
            for (double time : times) {
                timesText += " " + exact(time);
            }
            state->entries["bezier " + name + " keyframes"] = timesText;
            state->entries["bezier " + name + " flags"] = std::string(bezier->isCurveFinished() ? "finished" : "unfinished") + (bezier->isOpenBezier() ? " open" : " closed");
            state->entries["bezier " + name + " feather"] = storedCurveText(bezier->getFeatherKnob()->getCurve(ViewSpec(0), 0));
            addPoints(state, "bezier " + name + " point", bezier->getControlPoints_mt_safe());
            addPoints(state, "bezier " + name + " feather point", bezier->getFeatherPoints_mt_safe());
        }
    }
}

// The items and tracks as they would be saved, so that two states compare whether or not the
// nodes holding them still exist.
ProjectSnapshot
captureRotoTrackerState(const NodePtr& rotoPaint,
                        const NodePtr& tracker)
{
    ProjectSnapshot state;
    RotoContextPtr roto = rotoPaint->getRotoContext();
    if (roto) {
        RotoLayerPtr baseLayer = roto->getOrCreateBaseLayer();
        state.entries["roto tree"] = itemTree(baseLayer);
        addRotoItems(&state, baseLayer);
    }

    TrackerContextPtr trackerContext = tracker->getTrackerContext();
    if (trackerContext) {
        std::vector<TrackMarkerPtr> tracks;
        trackerContext->getAllMarkers(&tracks);
        state.entries["track count"] = std::to_string(tracks.size());
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const std::string key = "track " + std::to_string(i);
            state.entries[key + " name"] = tracks[i]->getScriptName_mt_safe();
            state.entries[key + " enabled"] = tracks[i]->getEnabledKnob()->getValue() ? "true" : "false";
            std::set<int> userKeys;
            tracks[i]->getUserKeyframes(&userKeys);
            std::string keysText;
            for (int key : userKeys) {
                keysText += " " + std::to_string(key);
            }
            state.entries[key + " user keys"] = keysText;
            for (int d = 0; d < 2; ++d) {
                state.entries[key + " center " + std::to_string(d)] = storedCurveText(tracks[i]->getCenterKnob()->getCurve(ViewSpec(0), d));
            }
        }
    }

    return state;
}

void
expectSameImage(const FlatExrImage& expected,
                const FlatExrImage& actual)
{
    EXPECT_EQ(expected.width, actual.width);
    EXPECT_EQ(expected.height, actual.height);
    EXPECT_EQ(expected.channels, actual.channels);
    ASSERT_EQ(expected.pixels.size(), actual.pixels.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < expected.pixels.size(); ++i) {
        if (YamlTest::bitsOfFloat(expected.pixels[i]) != YamlTest::bitsOfFloat(actual.pixels[i])) {
            ++differing;
        }
    }
    EXPECT_EQ(0u, differing) << "of " << expected.pixels.size() << " values";
}

} // anonymous namespace

class RotoTrackerBundleTest
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

    // RotoContext::makeStroke() inserts the item before creating its nodes and null-derefs the
    // merge node it lacks; this is how RotoPaintInteract::makeStroke() builds one instead.
    RotoStrokeItemPtr addStroke(const RotoContextPtr& context,
                                const RotoLayerPtr& layer,
                                const std::string& name,
                                int samples,
                                double phase)
    {
        RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, context, name, RotoLayerPtr()));
        stroke->createNodes();
        context->addItem(layer, 0, stroke, RotoItem::eSelectionReasonOther);
        stroke->getBrushSizeKnob()->setValue(1.5, ViewSpec::all(), 0);
        for (int i = 0; i < samples; ++i) {
            const double t = i / (double)samples;
            const RotoPoint point(4. + 3. * std::cos(phase + 6. * t), 4. + 3. * std::sin(phase + 4. * t), 0.2 + 0.8 * std::fabs(std::sin(9. * t)), 1. + 0.02 * i);
            stroke->appendPoint((i == 0) || (i == samples / 2), point);
        }
        stroke->setStrokeFinished();

        return stroke;
    }

    // A RotoPaint on an 8x8 image: the base layer holds a 64-point polygon and a stroke and an
    // outer layer; the outer layer holds a stroke and an inner layer, which holds a feathered
    // square animated over kShapeFirstFrame..kShapeLastFrame with one broken tangent.
    NodePtr buildRotoPaint()
    {
        NodePtr reader = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
        NodePtr rotoPaint = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
        if (!reader || !rotoPaint) {
            return NodePtr();
        }
        connectNodes(reader, rotoPaint, 0, true);
        RotoContextPtr context = rotoPaint->getRotoContext();
        if (!context) {
            return NodePtr();
        }

        // A fresh context selects nothing, so the first layer goes under the base layer and the
        // next layer and shape under the layer created just before them.
        RotoLayerPtr baseLayer = context->getOrCreateBaseLayer();
        RotoLayerPtr outerLayer = context->addLayer();
        RotoLayerPtr innerLayer = context->addLayer();
        BezierPtr square = context->makeSquare(1., 7., 4., kShapeFirstFrame);
        if (!baseLayer || !outerLayer || !innerLayer || !square) {
            return NodePtr();
        }
        for (int frame = kShapeFirstFrame; frame <= kShapeLastFrame; ++frame) {
            square->setKeyframe(frame);
        }
        square->setPointAtIndex(false, 0, 8., 1.5, 6.5, 1., 7., 2., 6.);
        square->setPointAtIndex(false, 2, kShapeLastFrame, 6., 2., 6.5, 2.5, 5.5, 1.5);
        square->setPointAtIndex(true, 1, 10., 6., 7.5, 5.5, 7.5, 6.5, 7.5);
        const std::list<BezierCPPtr> controlPoints = square->getControlPoints_mt_safe();
        if (controlPoints.empty()) {
            return NodePtr();
        }
        controlPoints.front()->setBroken(true);
        square->setLeftBezierPoint(0, 8., 0.5, 5.);
        KnobDoublePtr feather = square->getFeatherKnob();
        if (!feather) {
            return NodePtr();
        }
        feather->setValueAtTime(kShapeFirstFrame, 0., ViewSpec::all(), 0);
        feather->setValueAtTime(kShapeLastFrame, 1.5, ViewSpec::all(), 0);

        if (!addStroke(context, outerLayer, "OuterStroke", 80, 0.)) {
            return NodePtr();
        }

        context->clearSelection(RotoItem::eSelectionReasonOther);
        BezierPtr polygon = context->makeBezier(4. + 2.5, 4., "Polygon", kShapeFirstFrame, false);
        if (!polygon) {
            return NodePtr();
        }
        for (int i = 1; i < kPolygonPoints; ++i) {
            const double angle = 2. * M_PI * i / kPolygonPoints;
            polygon->addControlPoint(4. + 2.5 * std::cos(angle), 4. + 2.5 * std::sin(angle), kShapeFirstFrame);
        }
        polygon->setCurveFinished(true);
        polygon->getColorKnob()->setValue(0.25, ViewSpec::all(), 2);

        if (!addStroke(context, baseLayer, "BaseStroke", 70, 1.)) {
            return NodePtr();
        }
        context->clearSelection(RotoItem::eSelectionReasonOther);

        return rotoPaint;
    }

    // A Tracker with three tracks keyed on every frame of 1..kTrackFrames, the second one
    // disabled and the others with user keyframes.
    NodePtr buildTracker()
    {
        NodePtr reader = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
        NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
        if (!reader || !tracker) {
            return NodePtr();
        }
        connectNodes(reader, tracker, 0, true);
        TrackerContextPtr context = tracker->getTrackerContext();
        if (!context) {
            return NodePtr();
        }
        for (int t = 0; t < 3; ++t) {
            TrackMarkerPtr track = context->createMarker();
            if (!track) {
                return NodePtr();
            }
            // createMarker() already gives the track a unique name, and setScriptName() returns
            // false when asked for the name the track already has, so the name is checked instead.
            const std::string name = "track" + std::to_string(t + 1);
            track->setScriptName(name);
            if (track->getScriptName_mt_safe() != name) {
                ADD_FAILURE() << "the new track is named " << track->getScriptName_mt_safe() << ", not " << name;

                return NodePtr();
            }
            KnobDoublePtr center = track->getCenterKnob();
            for (int frame = 1; frame <= kTrackFrames; ++frame) {
                center->setValueAtTime(frame, 100. * t + frame * 0.37 + std::sin(frame * 0.1), ViewSpec::all(), 0);
                center->setValueAtTime(frame, 50. + 20. * t + std::cos(frame * 0.07) * 12.5, ViewSpec::all(), 1);
            }
            if (t == 1) {
                track->getEnabledKnob()->setValue(false, ViewSpec::all(), 0);
            } else {
                track->setUserKeyframe(1);
                track->setUserKeyframe(kTrackFrames / 2 + t);
                track->setUserKeyframe(kTrackFrames);
            }
        }

        return tracker;
    }

    // The native Write in the one layout FlatExrReader parses: one 32-bit float uncompressed
    // part holding every layer.
    NodePtr addWriter(const NodePtr& input)
    {
        NodePtr writer = createNativeWriter();
        if (!writer) {
            return NodePtr();
        }
        connectNodes(input, writer, 0, true);
        KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(writer->getKnobByName("partSplitting").get());
        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
        KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
        KnobChannelSet* channels = dynamic_cast<KnobChannelSet*>(writer->getKnobByName(kNodeParamChannelSet).get());
        if (!partSplitting || !bitDepth || !compression || !channels) {
            return NodePtr();
        }
        partSplitting->setValueFromID("single", 0);
        bitDepth->setValueFromID("32f", 0);
        compression->setValueFromID("none", 0);
        channels->setAll();

        return writer;
    }

    // Renders kRenderedFrames through writer into tmp, one file per frame named after tag.
    bool renderFrames(const NodePtr& writer,
                      const QTemporaryDir& tmp,
                      const std::string& tag,
                      std::vector<FlatExrImage>* images,
                      std::string* error)
    {
        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
        if (!writerEffect) {
            *error = "the writer has no OutputEffectInstance";

            return false;
        }
        images->clear();
        for (int frame : kRenderedFrames) {
            const std::string path = (tmp.path() + QLatin1Char('/')).toStdString() + tag + "-" + std::to_string(frame) + ".exr";
            writer->setOutputFilesForWriter(path);
            QFile::remove(QString::fromStdString(path));
            std::list<AppInstance::RenderWork> works;
            works.push_back(AppInstance::RenderWork(writerEffect, frame, frame, 1, false));
            getApp()->startWritersRendering(false, works);
            if (!QFile::exists(QString::fromStdString(path))) {
                *error = "frame " + std::to_string(frame) + " was not rendered: " + path;

                return false;
            }
            images->push_back(FlatExrImage());
            if (!readFlatExr(path, &images->back(), error)) {
                return false;
            }
        }

        return true;
    }

    bool saveAs(const QTemporaryDir& tmp,
                const QString& fileName,
                QString* savedPath)
    {
        return getApp()->getProject()->saveProject(tmp.path() + QLatin1Char('/'), fileName, savedPath);
    }

    bool resetAndLoad(const QString& path)
    {
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        const QFileInfo info(path);
        if (!project->loadProject(info.absolutePath() + QLatin1Char('/'), info.fileName())) {
            return false;
        }
        const QStringList warnings = project->getLoadWarnings();
        EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();

        return true;
    }

    NodePtr node(const std::string& fullyQualifiedName) const
    {
        return getApp()->getNodeByFullySpecifiedName(fullyQualifiedName);
    }
};

TEST_F(RotoTrackerBundleTest, RotoAndTrackerSurviveSaveAndLoad)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ProjectPtr project = getApp()->getProject();

    NodePtr builtRotoPaint = buildRotoPaint();
    ASSERT_TRUE(bool(builtRotoPaint));
    NodePtr builtTracker = buildTracker();
    ASSERT_TRUE(bool(builtTracker));
    NodePtr builtWriter = addWriter(builtRotoPaint);
    ASSERT_TRUE(bool(builtWriter));
    const std::string rotoPaintName = builtRotoPaint->getFullyQualifiedName();
    const std::string trackerName = builtTracker->getFullyQualifiedName();
    const std::string writerName = builtWriter->getFullyQualifiedName();

    const ProjectSnapshot builtState = captureRotoTrackerState(builtRotoPaint, builtTracker);
    EXPECT_EQ("3", builtState.entries.at("track count"));
    EXPECT_EQ("false", builtState.entries.at("track 1 enabled"));

    std::vector<FlatExrImage> builtImages;
    std::string error;
    ASSERT_TRUE(renderFrames(builtWriter, tmp, "built", &builtImages, &error)) << error;
    // The shapes paint white over the red input, so a render that ignored them would not be told
    // apart by the comparisons below.
    const FlatExrImage& firstImage = builtImages.front();
    const int green = firstImage.channelIndex("G");
    ASSERT_LE(0, green);
    bool paintedSomething = false;
    for (std::size_t i = green; i < firstImage.pixels.size(); i += firstImage.channels.size()) {
        paintedSomething = paintedSomething || (firstImage.pixels[i] != 0.f);
    }
    EXPECT_TRUE(paintedSomething);

    QString firstPath;
    ASSERT_TRUE(saveAs(tmp, QString::fromUtf8("roto-tracker.ntpz"), &firstPath));
    {
        const Serialization::ZipReader archive(firstPath.toStdString());
        std::string root;
        for (const Serialization::ZipMember& member : archive.members()) {
            if (member.name.ends_with("/project.yaml")) {
                root = member.name.substr(0, member.name.size() - std::string("project.yaml").size());
            }
        }
        ASSERT_FALSE(root.empty()) << "the saved bundle has no project.yaml under a folder";
        const std::string rotoBlobs = root + "blobs/" + rotoPaintName + "/";
        const std::string trackBlobs = root + "blobs/" + trackerName + "/";
        int strokeBlobCount = 0, bezierBlobCount = 0, trackBlobCount = 0;
        for (const Serialization::ZipMember& member : archive.members()) {
            if (member.name.starts_with(rotoBlobs) && member.name.ends_with(".stroke.bin")) {
                ++strokeBlobCount;
            } else if (member.name.starts_with(rotoBlobs) && (member.name.ends_with(".points.bin") || member.name.ends_with(".shape.bin"))) {
                ++bezierBlobCount;
            } else if (member.name.starts_with(trackBlobs) && member.name.ends_with(".track.bin")) {
                ++trackBlobCount;
            }
        }
        EXPECT_EQ(2, strokeBlobCount);
        EXPECT_LE(1, bezierBlobCount);
        EXPECT_EQ(3, trackBlobCount);
    }

    ASSERT_TRUE(resetAndLoad(firstPath));
    NodePtr rotoPaint = node(rotoPaintName);
    NodePtr tracker = node(trackerName);
    NodePtr writer = node(writerName);
    ASSERT_TRUE(rotoPaint && tracker && writer);
    EXPECT_TRUE(projectsAreEquivalent(builtState, captureRotoTrackerState(rotoPaint, tracker)));

    std::vector<FlatExrImage> loadedImages;
    ASSERT_TRUE(renderFrames(writer, tmp, "loaded", &loadedImages, &error)) << error;
    ASSERT_EQ(builtImages.size(), loadedImages.size());
    for (std::size_t i = 0; i < builtImages.size(); ++i) {
        SCOPED_TRACE("frame " + std::to_string(kRenderedFrames[i]));
        expectSameImage(builtImages[i], loadedImages[i]);
    }

    // Loading a RotoPaint settles state the built graph does not have yet: it switches the paint
    // tool, and with it the compositing operator its knobs hold. The project is therefore
    // compared across a second save and load, from which on nothing may change.
    const ProjectSnapshot loaded = captureProjectSnapshot(project);
    QString secondPath;
    ASSERT_TRUE(saveAs(tmp, QString::fromUtf8("roto-tracker-again.ntpz"), &secondPath));
    ASSERT_TRUE(resetAndLoad(secondPath));
    EXPECT_TRUE(projectsAreEquivalent(loaded, captureProjectSnapshot(project)));

    NodePtr reloadedRotoPaint = node(rotoPaintName);
    NodePtr reloadedTracker = node(trackerName);
    NodePtr reloadedWriter = node(writerName);
    ASSERT_TRUE(reloadedRotoPaint && reloadedTracker && reloadedWriter);
    EXPECT_TRUE(projectsAreEquivalent(builtState, captureRotoTrackerState(reloadedRotoPaint, reloadedTracker)));

    std::vector<FlatExrImage> reloadedImages;
    ASSERT_TRUE(renderFrames(reloadedWriter, tmp, "reloaded", &reloadedImages, &error)) << error;
    ASSERT_EQ(builtImages.size(), reloadedImages.size());
    for (std::size_t i = 0; i < builtImages.size(); ++i) {
        SCOPED_TRACE("frame " + std::to_string(kRenderedFrames[i]));
        expectSameImage(builtImages[i], reloadedImages[i]);
    }
}
