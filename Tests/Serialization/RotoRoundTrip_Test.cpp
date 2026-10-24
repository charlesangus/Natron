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

#include <list>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "YamlTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/ViewIdx.h"

#include "Serialization/RotoSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/RotoYaml.h"

NATRON_NAMESPACE_USING

namespace {

const std::string kRotoNodeName = "RotoPaint1";

// Gives the decoded knobs of every item the types the live item knobs would, taken from the
// items that were encoded.
void
completeItemKnobsLike(Serialization::RotoLayerSerialization* decoded,
                      const Serialization::RotoLayerSerialization& original)
{
    std::list<Serialization::RotoLayerChildSerialization>::const_iterator source = original.children.begin();
    for (std::list<Serialization::RotoLayerChildSerialization>::iterator it = decoded->children.begin(); (it != decoded->children.end()) && (source != original.children.end()); ++it, ++source) {
        if (it->type == Serialization::eRotoLayerChildTypeLayer) {
            completeItemKnobsLike(&it->layer, source->layer);
        } else if (it->type == Serialization::eRotoLayerChildTypeBezier) {
            completeLike(&it->bezier.knobs, source->bezier.knobs);
        } else {
            completeLike(&it->stroke.knobs, source->stroke.knobs);
        }
    }
}

// A fresh shape has a single, unkeyed position: keying it at its creation time and then moving
// a control point and a feather point at a later time is what animates their curves.
void
animateSquare(const BezierPtr& square)
{
    square->setKeyframe(1.);
    square->setPointAtIndex(false, 0, 10., 5., 45., 5., 45., 5., 45.);
    square->setPointAtIndex(true, 2, 10., 30., 25., 28., 25., 32., 25.);
}

std::set<double>
keyframeTimes(const CurvePtr& curve)
{
    std::set<double> times;
    const KeyFrameSet keyframes = curve->getKeyFrames_mt_safe();

    for (KeyFrameSet::const_iterator it = keyframes.begin(); it != keyframes.end(); ++it) {
        times.insert(it->getTime());
    }

    return times;
}

// Compares the internal, not the GUI, curves of two points, and their tangents as stored: the
// realigned tangents depend on the broken flag, which saving does not keep.
void
expectSamePoint(const BezierCPPtr& expected,
                const BezierCPPtr& actual,
                const std::set<double>& times)
{
    EXPECT_EQ(keyframeTimes(expected->getXCurve()), keyframeTimes(actual->getXCurve()));
    EXPECT_EQ(keyframeTimes(expected->getLeftXCurve()), keyframeTimes(actual->getLeftXCurve()));
    for (std::set<double>::const_iterator it = times.begin(); it != times.end(); ++it) {
        double expectedX = 0., expectedY = 0., actualX = 0., actualY = 0.;
        expected->getPositionAtTime(false, *it, ViewIdx(0), &expectedX, &expectedY);
        actual->getPositionAtTime(false, *it, ViewIdx(0), &actualX, &actualY);
        EXPECT_DOUBLE_EQ(expectedX, actualX);
        EXPECT_DOUBLE_EQ(expectedY, actualY);

        expected->getLeftBezierPointAtTime(false, *it, ViewIdx(0), &expectedX, &expectedY, false);
        actual->getLeftBezierPointAtTime(false, *it, ViewIdx(0), &actualX, &actualY, false);
        EXPECT_DOUBLE_EQ(expectedX, actualX);
        EXPECT_DOUBLE_EQ(expectedY, actualY);

        expected->getRightBezierPointAtTime(false, *it, ViewIdx(0), &expectedX, &expectedY, false);
        actual->getRightBezierPointAtTime(false, *it, ViewIdx(0), &actualX, &actualY, false);
        EXPECT_DOUBLE_EQ(expectedX, actualX);
        EXPECT_DOUBLE_EQ(expectedY, actualY);
    }
}

void
expectSameShape(const BezierPtr& expected,
                const BezierPtr& actual)
{
    ASSERT_TRUE(bool(actual));
    EXPECT_EQ(expected->isCurveFinished(), actual->isCurveFinished());
    EXPECT_EQ(expected->isOpenBezier(), actual->isOpenBezier());

    std::set<double> expectedTimes, actualTimes;
    expected->getKeyframeTimes(&expectedTimes);
    actual->getKeyframeTimes(&actualTimes);
    EXPECT_EQ(expectedTimes, actualTimes);
    // Points without keyframes are only compared if at least one time is evaluated.
    expectedTimes.insert(0.);

    const std::list<BezierCPPtr> expectedPoints = expected->getControlPoints_mt_safe();
    const std::list<BezierCPPtr> actualPoints = actual->getControlPoints_mt_safe();
    ASSERT_EQ(expectedPoints.size(), actualPoints.size());
    std::list<BezierCPPtr>::const_iterator actualIt = actualPoints.begin();
    for (std::list<BezierCPPtr>::const_iterator it = expectedPoints.begin(); it != expectedPoints.end(); ++it, ++actualIt) {
        expectSamePoint(*it, *actualIt, expectedTimes);
        // Saving never keeps the broken flag of a bezier's points.
        EXPECT_FALSE((*actualIt)->getBroken());
    }

    const std::list<BezierCPPtr> expectedFeatherPoints = expected->getFeatherPoints_mt_safe();
    const std::list<BezierCPPtr> actualFeatherPoints = actual->getFeatherPoints_mt_safe();
    ASSERT_EQ(expectedFeatherPoints.size(), actualFeatherPoints.size());
    actualIt = actualFeatherPoints.begin();
    for (std::list<BezierCPPtr>::const_iterator it = expectedFeatherPoints.begin(); it != expectedFeatherPoints.end(); ++it, ++actualIt) {
        EXPECT_TRUE((*actualIt)->isFeatherPoint());
        expectSamePoint(*it, *actualIt, expectedTimes);
    }
}

void
expectSameStroke(const RotoStrokeItemPtr& expected,
                 const RotoStrokeItemPtr& actual)
{
    ASSERT_TRUE(bool(actual));
    EXPECT_EQ(expected->getBrushType(), actual->getBrushType());

    const std::list<CurvePtr> expectedCurves = expected->getXControlPoints();
    const std::list<CurvePtr> actualCurves = actual->getXControlPoints();
    ASSERT_EQ(expectedCurves.size(), actualCurves.size());
    std::list<CurvePtr>::const_iterator actualIt = actualCurves.begin();
    for (std::list<CurvePtr>::const_iterator it = expectedCurves.begin(); it != expectedCurves.end(); ++it, ++actualIt) {
        const std::set<double> times = keyframeTimes(*it);
        EXPECT_EQ(times, keyframeTimes(*actualIt));
        for (std::set<double>::const_iterator timeIt = times.begin(); timeIt != times.end(); ++timeIt) {
            EXPECT_DOUBLE_EQ((*it)->getValueAt(*timeIt), (*actualIt)->getValueAt(*timeIt));
        }
    }
}

} // anonymous namespace

class RotoRoundTripTest
    : public BaseTest {
protected:
    RotoContextPtr createRotoPaintContext()
    {
        NodePtr reader = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
        NodePtr rotoPaint = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));

        if (!reader || !rotoPaint) {
            return RotoContextPtr();
        }
        connectNodes(reader, rotoPaint, 0, true);

        return rotoPaint->getRotoContext();
    }

    // RotoContext::makeStroke() inserts the item into its layer before calling createNodes(),
    // which null-derefs the not-yet-created merge node; this creates the stroke the way
    // RotoPaintInteract::makeStroke() does instead, nodes first.
    RotoStrokeItemPtr addStroke(const RotoContextPtr& context,
                                const RotoLayerPtr& layer,
                                const std::string& name)
    {
        RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, context, name, RotoLayerPtr()));

        stroke->createNodes();
        context->addItem(layer, 0, stroke, RotoItem::eSelectionReasonOther);
        stroke->appendPoint(true, RotoPoint(10., 10., 1., 1.));
        stroke->appendPoint(false, RotoPoint(40., 60., 0.5, 2.));
        stroke->appendPoint(true, RotoPoint(70., 15., 0.75, 3.));
        stroke->appendPoint(false, RotoPoint(90., 35., 1., 4.));
        stroke->setStrokeFinished();

        return stroke;
    }

    // Saves the context to YAML, loads that YAML onto a fresh RotoPaint and checks that saving
    // the fresh one writes the same YAML. Returns the fresh context, or null on failure.
    RotoContextPtr expectRoundTripMatches(const RotoContextPtr& source,
                                          Serialization::RotoContextSerialization* loaded)
    {
        Serialization::RotoContextSerialization saved;
        source->toSerialization(&saved);
        Serialization::MemoryBlobStore blobs;
        const std::string savedYaml = Serialization::encodeRotoYaml(saved, kRotoNodeName, blobs);

        *loaded = Serialization::decodeRotoYaml(savedYaml, blobs);
        Serialization::MemoryBlobStore reencodedBlobs;
        EXPECT_EQ(savedYaml, Serialization::encodeRotoYaml(*loaded, kRotoNodeName, reencodedBlobs));
        completeItemKnobsLike(&loaded->baseLayer, saved.baseLayer);

        RotoContextPtr restored = createRotoPaintContext();
        if (!restored) {
            ADD_FAILURE() << "Could not create the RotoPaint node to load into";

            return RotoContextPtr();
        }
        restored->fromSerialization(*loaded);

        Serialization::RotoContextSerialization resaved;
        restored->toSerialization(&resaved);
        Serialization::MemoryBlobStore resavedBlobs;
        EXPECT_EQ(savedYaml, Serialization::encodeRotoYaml(resaved, kRotoNodeName, resavedBlobs));

        return restored;
    }
};

TEST_F(RotoRoundTripTest, Stroke)
{
    RotoContextPtr context = createRotoPaintContext();

    ASSERT_TRUE(bool(context));

    RotoStrokeItemPtr stroke = addStroke(context, context->getOrCreateBaseLayer(), "Stroke1");
    ASSERT_TRUE(bool(stroke));
    // A loaded selection only ever selects beziers again, so a selected stroke would not
    // survive the round trip.
    context->clearSelection(RotoItem::eSelectionReasonOther);

    Serialization::RotoContextSerialization loaded;
    RotoContextPtr restored = expectRoundTripMatches(context, &loaded);
    ASSERT_TRUE(bool(restored));

    ASSERT_EQ(1u, loaded.baseLayer.children.size());
    const Serialization::RotoLayerChildSerialization& child = loaded.baseLayer.children.front();
    EXPECT_EQ(Serialization::eRotoLayerChildTypeStroke, child.type);
    EXPECT_EQ("Stroke1", child.stroke.name);
    EXPECT_EQ((int)eRotoStrokeTypeSolid, child.stroke.brushType);
    ASSERT_EQ(2u, child.stroke.strokes.size());
    EXPECT_EQ(2u, child.stroke.strokes.front().xCurve.keyframes.size());
    EXPECT_FALSE(child.stroke.knobs.empty());

    RotoLayerPtr restoredBaseLayer = restored->getOrCreateBaseLayer();
    const std::list<RotoItemPtr> restoredItems = restoredBaseLayer->getItems_mt_safe();
    ASSERT_EQ(1u, restoredItems.size());
    RotoStrokeItemPtr restoredStroke = std::dynamic_pointer_cast<RotoStrokeItem>(restoredItems.front());
    ASSERT_TRUE(bool(restoredStroke));
    EXPECT_EQ("Stroke1", restoredStroke->getScriptName());
    EXPECT_EQ(restoredBaseLayer, restoredStroke->getParentLayer());
    EXPECT_EQ(eRotoStrokeTypeSolid, restoredStroke->getBrushType());
    ASSERT_EQ(2u, restoredStroke->getXControlPoints().size());
    expectSameStroke(stroke, restoredStroke);
    EXPECT_TRUE(restored->getSelectedItems().empty());
}

TEST_F(RotoRoundTripTest, AnimatedFeatheredBezierWithBrokenTangent)
{
    RotoContextPtr context = createRotoPaintContext();

    ASSERT_TRUE(bool(context));

    BezierPtr square = context->makeSquare(1., 50., 20., 1.);
    ASSERT_TRUE(bool(square));
    animateSquare(square);

    const std::list<BezierCPPtr> controlPoints = square->getControlPoints_mt_safe();
    ASSERT_EQ(4u, controlPoints.size());
    controlPoints.front()->setBroken(true);
    square->setLeftBezierPoint(0, 10., 2., 52.);

    KnobDoublePtr featherKnob = square->getFeatherKnob();
    ASSERT_TRUE(bool(featherKnob));
    featherKnob->setValueAtTime(1, 0., ViewSpec::all(), 0);
    featherKnob->setValueAtTime(10, 6., ViewSpec::all(), 0);

    context->clearSelection(RotoItem::eSelectionReasonOther);
    context->select(square, RotoItem::eSelectionReasonOther);

    Serialization::RotoContextSerialization loaded;
    RotoContextPtr restored = expectRoundTripMatches(context, &loaded);
    ASSERT_TRUE(bool(restored));

    ASSERT_EQ(1u, loaded.baseLayer.children.size());
    const Serialization::RotoLayerChildSerialization& child = loaded.baseLayer.children.front();
    EXPECT_EQ(Serialization::eRotoLayerChildTypeBezier, child.type);
    ASSERT_EQ(4u, child.bezier.controlPoints.size());
    EXPECT_EQ(4u, child.bezier.featherPoints.size());
    EXPECT_TRUE(child.bezier.closed);
    EXPECT_FALSE(child.bezier.isOpenBezier);
    EXPECT_EQ(2u, child.bezier.controlPoints.front().xCurve.keyframes.size());
    EXPECT_EQ(2u, child.bezier.controlPoints.front().leftXCurve.keyframes.size());
    // Saving never keeps the broken flag of a bezier's points.
    EXPECT_FALSE(child.bezier.controlPoints.front().broken);
    ASSERT_EQ(1u, loaded.selectedItems.size());
    EXPECT_EQ(square->getScriptName(), loaded.selectedItems.front());

    BezierPtr restoredSquare = std::dynamic_pointer_cast<Bezier>(restored->getItemByName(square->getScriptName()));
    ASSERT_TRUE(bool(restoredSquare));
    EXPECT_EQ(restored->getOrCreateBaseLayer(), restoredSquare->getParentLayer());
    EXPECT_EQ(4, restoredSquare->getControlPointsCount());
    std::set<double> restoredTimes;
    restoredSquare->getKeyframeTimes(&restoredTimes);
    std::set<double> expectedTimes;
    expectedTimes.insert(1.);
    expectedTimes.insert(10.);
    EXPECT_EQ(expectedTimes, restoredTimes);
    expectSameShape(square, restoredSquare);

    KnobDoublePtr restoredFeatherKnob = restoredSquare->getFeatherKnob();
    ASSERT_TRUE(bool(restoredFeatherKnob));
    EXPECT_DOUBLE_EQ(0., restoredFeatherKnob->getValueAtTime(1.));
    EXPECT_DOUBLE_EQ(6., restoredFeatherKnob->getValueAtTime(10.));

    const std::list<RotoItemPtr> restoredSelection = restored->getSelectedItems();
    ASSERT_EQ(1u, restoredSelection.size());
    EXPECT_EQ(restoredSquare, restoredSelection.front());
}

TEST_F(RotoRoundTripTest, NestedLayers)
{
    RotoContextPtr context = createRotoPaintContext();

    ASSERT_TRUE(bool(context));

    // A fresh context selects nothing, so the first layer goes under the base layer and each
    // later layer or shape goes under the layer created just before it.
    RotoLayerPtr baseLayer = context->getOrCreateBaseLayer();
    RotoLayerPtr outerLayer = context->addLayer();
    RotoLayerPtr innerLayer = context->addLayer();
    ASSERT_TRUE(bool(outerLayer) && bool(innerLayer));
    BezierPtr innerSquare = context->makeSquare(5., 40., 10., 1.);
    ASSERT_TRUE(bool(innerSquare));
    animateSquare(innerSquare);

    RotoStrokeItemPtr outerStroke = addStroke(context, outerLayer, "OuterStroke");
    ASSERT_TRUE(bool(outerStroke));

    context->clearSelection(RotoItem::eSelectionReasonOther);
    BezierPtr baseEllipse = context->makeEllipse(60., 60., 20., true, 1.);
    ASSERT_TRUE(bool(baseEllipse));

    context->clearSelection(RotoItem::eSelectionReasonOther);
    context->select(innerSquare, RotoItem::eSelectionReasonOther);

    Serialization::RotoContextSerialization loaded;
    RotoContextPtr restored = expectRoundTripMatches(context, &loaded);
    ASSERT_TRUE(bool(restored));

    const std::list<Serialization::RotoLayerChildSerialization>& baseChildren = loaded.baseLayer.children;
    ASSERT_EQ(2u, baseChildren.size());
    EXPECT_EQ(Serialization::eRotoLayerChildTypeBezier, baseChildren.front().type);
    EXPECT_EQ(baseEllipse->getScriptName(), baseChildren.front().bezier.name);
    ASSERT_EQ(Serialization::eRotoLayerChildTypeLayer, baseChildren.back().type);

    const Serialization::RotoLayerSerialization& outerData = baseChildren.back().layer;
    EXPECT_EQ(outerLayer->getScriptName(), outerData.name);
    EXPECT_EQ(baseLayer->getScriptName(), outerData.parentLayerName);
    ASSERT_EQ(2u, outerData.children.size());
    EXPECT_EQ(Serialization::eRotoLayerChildTypeStroke, outerData.children.front().type);
    ASSERT_EQ(Serialization::eRotoLayerChildTypeLayer, outerData.children.back().type);

    const Serialization::RotoLayerSerialization& innerData = outerData.children.back().layer;
    EXPECT_EQ(innerLayer->getScriptName(), innerData.name);
    EXPECT_EQ(outerLayer->getScriptName(), innerData.parentLayerName);
    ASSERT_EQ(1u, innerData.children.size());
    EXPECT_EQ(Serialization::eRotoLayerChildTypeBezier, innerData.children.front().type);
    EXPECT_EQ(innerSquare->getScriptName(), innerData.children.front().bezier.name);

    RotoLayerPtr restoredBaseLayer = restored->getOrCreateBaseLayer();
    const std::list<RotoItemPtr> restoredBaseItems = restoredBaseLayer->getItems_mt_safe();
    ASSERT_EQ(2u, restoredBaseItems.size());

    BezierPtr restoredEllipse = std::dynamic_pointer_cast<Bezier>(restoredBaseItems.front());
    ASSERT_TRUE(bool(restoredEllipse));
    EXPECT_EQ(baseEllipse->getScriptName(), restoredEllipse->getScriptName());
    EXPECT_EQ(restoredBaseLayer, restoredEllipse->getParentLayer());
    expectSameShape(baseEllipse, restoredEllipse);

    RotoLayerPtr restoredOuterLayer = std::dynamic_pointer_cast<RotoLayer>(restoredBaseItems.back());
    ASSERT_TRUE(bool(restoredOuterLayer));
    EXPECT_EQ(outerLayer->getScriptName(), restoredOuterLayer->getScriptName());
    EXPECT_EQ(restoredBaseLayer, restoredOuterLayer->getParentLayer());
    const std::list<RotoItemPtr> restoredOuterItems = restoredOuterLayer->getItems_mt_safe();
    ASSERT_EQ(2u, restoredOuterItems.size());

    RotoStrokeItemPtr restoredOuterStroke = std::dynamic_pointer_cast<RotoStrokeItem>(restoredOuterItems.front());
    ASSERT_TRUE(bool(restoredOuterStroke));
    EXPECT_EQ("OuterStroke", restoredOuterStroke->getScriptName());
    EXPECT_EQ(restoredOuterLayer, restoredOuterStroke->getParentLayer());
    expectSameStroke(outerStroke, restoredOuterStroke);

    RotoLayerPtr restoredInnerLayer = std::dynamic_pointer_cast<RotoLayer>(restoredOuterItems.back());
    ASSERT_TRUE(bool(restoredInnerLayer));
    EXPECT_EQ(innerLayer->getScriptName(), restoredInnerLayer->getScriptName());
    EXPECT_EQ(restoredOuterLayer, restoredInnerLayer->getParentLayer());
    const std::list<RotoItemPtr> restoredInnerItems = restoredInnerLayer->getItems_mt_safe();
    ASSERT_EQ(1u, restoredInnerItems.size());

    BezierPtr restoredInnerSquare = std::dynamic_pointer_cast<Bezier>(restoredInnerItems.front());
    ASSERT_TRUE(bool(restoredInnerSquare));
    EXPECT_EQ(innerSquare->getScriptName(), restoredInnerSquare->getScriptName());
    EXPECT_EQ(restoredInnerLayer, restoredInnerSquare->getParentLayer());
    expectSameShape(innerSquare, restoredInnerSquare);

    const std::list<RotoItemPtr> restoredSelection = restored->getSelectedItems();
    ASSERT_EQ(1u, restoredSelection.size());
    EXPECT_EQ(restoredInnerSquare, restoredSelection.front());
}
