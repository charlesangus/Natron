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
#include <cstdint>
#include <iterator>
#include <list>
#include <string>

#include <gtest/gtest.h>

#include "Serialization/RotoSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/RotoYaml.h"
#include "Serialization/Yaml/YamlScalars.h"
#include "YamlTestHelpers.h"

NATRON_NAMESPACE_USING
using Serialization::BezierCPSerialization;
using Serialization::BezierSerialization;
using Serialization::CurveSerialization;
using Serialization::KeyFrameSerialization;
using Serialization::RotoContextSerialization;
using Serialization::RotoLayerChildSerialization;
using Serialization::RotoLayerSerialization;
using Serialization::RotoStrokeCurvesSerialization;
using Serialization::RotoStrokeItemSerialization;
using Serialization::YamlWarnings;

namespace {

// Deterministic values spread over many exponents and mantissas, so a lossy text round trip
// would show.
class Sequence {
public:
    explicit Sequence(std::uint64_t seed)
        : _state(seed * 6364136223846793005ULL + 1442695040888963407ULL)
    {
    }

    double next(double scale)
    {
        _state = _state * 6364136223846793005ULL + 1442695040888963407ULL;
        const double unit = (double)(_state >> 11) / (double)(1ULL << 53);

        return (unit - 0.3) * scale;
    }

private:
    std::uint64_t _state;
};

KeyFrameSerialization
key(double time,
    double value,
    int interpolation)
{
    KeyFrameSerialization result;
    result.time = time;
    result.value = value;
    result.interpolation = interpolation;

    return result;
}

void
setItem(Serialization::RotoItemSerialization* item,
        const std::string& name,
        const std::string& parent)
{
    item->name = name;
    item->label = name;
    item->activated = true;
    item->parentLayerName = parent;
}

void
setDrawable(Serialization::RotoDrawableItemSerialization* item,
            double seed)
{
    item->overlayColor[0] = 0.25 * seed;
    item->overlayColor[1] = 1.;
    item->overlayColor[2] = 0.1;
    item->overlayColor[3] = 1.;
    Serialization::KnobSerialization opacity;
    opacity.name = "opacity";
    opacity.typeName = "Double";
    opacity.dimension = 1;
    opacity.dimensions.resize(1);
    opacity.dimensions[0].value = Serialization::KnobValueSerialization(std::in_place_type<double>, 0.5 + seed);
    item->knobs.push_back(opacity);
}

RotoStrokeItemSerialization
makeStroke(const std::string& name,
           const std::string& parent,
           const std::list<int>& segmentSizes,
           int interpolation)
{
    RotoStrokeItemSerialization stroke;
    setItem(&stroke, name, parent);
    setDrawable(&stroke, 1.);
    stroke.brushType = 3;
    Sequence values(segmentSizes.size() + 17);
    for (int size : segmentSizes) {
        RotoStrokeCurvesSerialization curves;
        double time = values.next(1.);
        for (int i = 0; i < size; ++i) {
            time += 0.001 + std::abs(values.next(0.01));
            curves.xCurve.keyframes.push_back(key(time, values.next(1920.), interpolation));
            curves.yCurve.keyframes.push_back(key(time, values.next(1080.), interpolation));
            curves.pressureCurve.keyframes.push_back(key(time, std::abs(values.next(1.)), interpolation));
        }
        stroke.strokes.push_back(curves);
    }

    return stroke;
}

BezierCPSerialization
makePoint(Sequence* values,
          const std::list<double>& times,
          int pointIndex)
{
    BezierCPSerialization point;
    point.x = values->next(1000.);
    point.y = values->next(1000.);
    point.leftX = values->next(10.);
    point.leftY = values->next(10.);
    point.rightX = values->next(10.);
    point.rightY = values->next(10.);
    int k = 0;
    for (double time : times) {
        const int interpolation = ((k + pointIndex) % 3 == 0) ? 0 : ((k % 2) ? 1 : 2);
        point.xCurve.keyframes.push_back(key(time, values->next(1000.), interpolation));
        point.yCurve.keyframes.push_back(key(time, values->next(1000.), interpolation));
        point.leftXCurve.keyframes.push_back(key(time, values->next(10.), interpolation));
        point.leftYCurve.keyframes.push_back(key(time, values->next(10.), interpolation));
        point.rightXCurve.keyframes.push_back(key(time, values->next(10.), interpolation));
        point.rightYCurve.keyframes.push_back(key(time, values->next(10.), interpolation));
        ++k;
    }

    return point;
}

// A feathered bezier whose control points and feather points all have keys at the same times.
BezierSerialization
makeBezier(const std::string& name,
           const std::string& parent,
           int pointCount,
           int keyCount)
{
    BezierSerialization bezier;
    setItem(&bezier, name, parent);
    setDrawable(&bezier, 2.);
    bezier.closed = true;
    std::list<double> times;
    for (int k = 0; k < keyCount; ++k) {
        times.push_back(1. + k * 1.5);
    }
    Sequence values((std::uint64_t)pointCount * 31 + keyCount);
    for (int p = 0; p < pointCount; ++p) {
        bezier.controlPoints.push_back(makePoint(&values, times, p));
        bezier.featherPoints.push_back(makePoint(&values, times, p));
    }

    return bezier;
}

void expectLayersEqual(const RotoLayerSerialization& e, const RotoLayerSerialization& a);

void
expectItemsEqual(const Serialization::RotoItemSerialization& e,
                 const Serialization::RotoItemSerialization& a)
{
    EXPECT_EQ(e.name, a.name);
    EXPECT_EQ(e.label, a.label);
    EXPECT_EQ(e.activated, a.activated);
    EXPECT_EQ(e.parentLayerName, a.parentLayerName);
    EXPECT_EQ(e.locked, a.locked);
}

void
expectDrawablesEqual(const Serialization::RotoDrawableItemSerialization& e,
                     const Serialization::RotoDrawableItemSerialization& a)
{
    expectItemsEqual(e, a);
    for (int i = 0; i < 4; ++i) {
        EXPECT_SAME_DOUBLE(e.overlayColor[i], a.overlayColor[i]);
    }
    expectExpectedKnobs(e.knobs, a.knobs);
}

void
expectPointsEqual(const std::list<BezierCPSerialization>& expected,
                  const std::list<BezierCPSerialization>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    auto a = actual.begin();
    int index = 0;
    for (const BezierCPSerialization& e : expected) {
        SCOPED_TRACE("point " + std::to_string(index++));
        EXPECT_SAME_DOUBLE(e.x, a->x);
        EXPECT_SAME_DOUBLE(e.y, a->y);
        EXPECT_SAME_DOUBLE(e.leftX, a->leftX);
        EXPECT_SAME_DOUBLE(e.leftY, a->leftY);
        EXPECT_SAME_DOUBLE(e.rightX, a->rightX);
        EXPECT_SAME_DOUBLE(e.rightY, a->rightY);
        EXPECT_EQ(e.broken, a->broken);
        expectCurvesEquivalent(e.xCurve, a->xCurve);
        expectCurvesEquivalent(e.yCurve, a->yCurve);
        expectCurvesEquivalent(e.leftXCurve, a->leftXCurve);
        expectCurvesEquivalent(e.leftYCurve, a->leftYCurve);
        expectCurvesEquivalent(e.rightXCurve, a->rightXCurve);
        expectCurvesEquivalent(e.rightYCurve, a->rightYCurve);
        ++a;
    }
}

void
expectChildrenEqual(const RotoLayerChildSerialization& e,
                    const RotoLayerChildSerialization& a)
{
    ASSERT_EQ(e.type, a.type);
    switch (e.type) {
    case Serialization::eRotoLayerChildTypeBezier: {
        SCOPED_TRACE("bezier " + e.bezier.name);
        expectDrawablesEqual(e.bezier, a.bezier);
        EXPECT_EQ(e.bezier.closed, a.bezier.closed);
        EXPECT_EQ(e.bezier.isOpenBezier, a.bezier.isOpenBezier);
        expectPointsEqual(e.bezier.controlPoints, a.bezier.controlPoints);
        expectPointsEqual(e.bezier.featherPoints, a.bezier.featherPoints);
        break;
    }
    case Serialization::eRotoLayerChildTypeStroke: {
        SCOPED_TRACE("stroke " + e.stroke.name);
        expectDrawablesEqual(e.stroke, a.stroke);
        EXPECT_EQ(e.stroke.brushType, a.stroke.brushType);
        ASSERT_EQ(e.stroke.strokes.size(), a.stroke.strokes.size());
        auto curves = a.stroke.strokes.begin();
        for (const RotoStrokeCurvesSerialization& expected : e.stroke.strokes) {
            expectCurvesEquivalent(expected.xCurve, curves->xCurve);
            expectCurvesEquivalent(expected.yCurve, curves->yCurve);
            expectCurvesEquivalent(expected.pressureCurve, curves->pressureCurve);
            ++curves;
        }
        break;
    }
    case Serialization::eRotoLayerChildTypeLayer:
        expectLayersEqual(e.layer, a.layer);
        break;
    }
}

void
expectLayersEqual(const RotoLayerSerialization& e,
                  const RotoLayerSerialization& a)
{
    SCOPED_TRACE("layer " + e.name);
    expectItemsEqual(e, a);
    ASSERT_EQ(e.children.size(), a.children.size());
    auto child = a.children.begin();
    for (const RotoLayerChildSerialization& expected : e.children) {
        expectChildrenEqual(expected, *child);
        ++child;
    }
}

void
expectRotoEqual(const RotoContextSerialization& e,
                const RotoContextSerialization& a)
{
    EXPECT_EQ(e.autoKeying, a.autoKeying);
    EXPECT_EQ(e.rippleEdit, a.rippleEdit);
    EXPECT_EQ(e.featherLink, a.featherLink);
    EXPECT_EQ(e.selectedItems, a.selectedItems);
    expectLayersEqual(e.baseLayer, a.baseLayer);
}

RotoContextSerialization
contextWith(const std::list<RotoLayerChildSerialization>& children)
{
    RotoContextSerialization roto;
    setItem(&roto.baseLayer, "Layer1", "");
    roto.baseLayer.children = children;
    roto.autoKeying = true;
    roto.featherLink = true;

    return roto;
}

RotoLayerChildSerialization
asChild(const BezierSerialization& bezier)
{
    RotoLayerChildSerialization child;
    child.type = Serialization::eRotoLayerChildTypeBezier;
    child.bezier = bezier;

    return child;
}

RotoLayerChildSerialization
asChild(const RotoStrokeItemSerialization& stroke)
{
    RotoLayerChildSerialization child;
    child.type = Serialization::eRotoLayerChildTypeStroke;
    child.stroke = stroke;

    return child;
}

RotoContextSerialization
roundTrip(const RotoContextSerialization& roto,
          Serialization::MemoryBlobStore* blobs,
          std::string* text)
{
    *text = Serialization::encodeRotoYaml(roto, "Group1.RotoPaint1", *blobs);
    YamlWarnings warnings;
    RotoContextSerialization decoded = Serialization::decodeRotoYaml(*text, *blobs, &warnings);
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(*text, Serialization::encodeRotoYaml(decoded, "Group1.RotoPaint1", *blobs = Serialization::MemoryBlobStore()));

    return decoded;
}

} // anonymous namespace

TEST(RotoYaml, LongStrokeIsOneBlobTable)
{
    const RotoContextSerialization roto = contextWith({ asChild(makeStroke("Stroke1", "Layer1", { 2000 }, 1)) });
    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeRotoYaml(roto, "Group1.RotoPaint1", blobs);

    ASSERT_EQ(1u, blobs.members().size());
    EXPECT_EQ("blobs/Group1.RotoPaint1/Stroke1.stroke.bin", blobs.members().begin()->first);
    EXPECT_NE(std::string::npos, text.find("segments: [2000]\n")) << text;
    EXPECT_NE(std::string::npos, text.find("interp: L\n")) << text;
    EXPECT_NE(std::string::npos, text.find("samples: {blob: blobs/Group1.RotoPaint1/Stroke1.stroke.bin, rows: 2000, columns: [")) << text;
    EXPECT_LT(text.size(), 1000u) << text;

    YamlWarnings warnings;
    expectRotoEqual(roto, Serialization::decodeRotoYaml(text, blobs, &warnings));
    EXPECT_TRUE(warnings.empty());
}

TEST(RotoYaml, AnimatedFeatheredBezierIsOneShapeTable)
{
    BezierSerialization bezier = makeBezier("Bezier1", "Layer1", 20, 100);
    std::list<BezierCPSerialization>::iterator point = bezier.controlPoints.begin();
    std::advance(point, 5);
    point->broken = true;
    std::list<BezierCPSerialization>::iterator feather = bezier.featherPoints.begin();
    std::advance(feather, 7);
    feather->broken = true;
    const RotoContextSerialization roto = contextWith({ asChild(bezier) });

    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeRotoYaml(roto, "Group1.RotoPaint1", blobs);
    ASSERT_EQ(1u, blobs.members().size());
    EXPECT_EQ("blobs/Group1.RotoPaint1/Bezier1.shape.bin", blobs.members().begin()->first);
    EXPECT_NE(std::string::npos, text.find("shape: {blob: blobs/Group1.RotoPaint1/Bezier1.shape.bin, rows: 2000, columns: [time:f64, cp:i32, x:f64, y:f64, lx:f64, ly:f64, rx:f64, ry:f64, fx:f64, fy:f64, flx:f64, fly:f64, frx:f64, fry:f64, interp:u8]}")) << text;
    EXPECT_NE(std::string::npos, text.find("points: {rows: 20, columns: [x:f64, y:f64, lx:f64, ly:f64, rx:f64, ry:f64, broken:u8, fx:f64, fy:f64, flx:f64, fly:f64, frx:f64, fry:f64, fbroken:u8], values: [")) << text;
    EXPECT_EQ(std::string::npos, text.find("curves:")) << text;

    YamlWarnings warnings;
    expectRotoEqual(roto, Serialization::decodeRotoYaml(text, blobs, &warnings));
    EXPECT_TRUE(warnings.empty());
}

TEST(RotoYaml, StaticBezierIsAnInlineList)
{
    BezierSerialization bezier = makeBezier("Static1", "Layer1", 4, 0);
    bezier.isOpenBezier = true;
    bezier.closed = false;
    bezier.label = "Garbage matte";
    bezier.locked = true;
    const RotoContextSerialization roto = contextWith({ asChild(bezier) });

    Serialization::MemoryBlobStore blobs;
    std::string text;
    const RotoContextSerialization decoded = roundTrip(roto, &blobs, &text);
    EXPECT_TRUE(blobs.members().empty());
    EXPECT_NE(std::string::npos, text.find("points: {rows: 4, columns: [x:f64, y:f64, lx:f64, ly:f64, rx:f64, ry:f64, fx:f64, fy:f64, flx:f64, fly:f64, frx:f64, fry:f64], values: [")) << text;
    EXPECT_EQ(std::string::npos, text.find("shape:")) << text;
    expectRotoEqual(roto, decoded);
}

TEST(RotoYaml, BezierWithAnOddKeySetKeepsItsCurvesInline)
{
    BezierSerialization bezier = makeBezier("Odd1", "Layer1", 6, 12);
    std::list<BezierCPSerialization>::iterator point = bezier.controlPoints.begin();
    std::advance(point, 3);
    point->xCurve.keyframes.erase(point->xCurve.keyframes.begin() + 4);
    std::list<BezierCPSerialization>::iterator feather = bezier.featherPoints.begin();
    feather->rightYCurve.keyframes[2].interpolation = 7;
    feather->rightYCurve.keyframes[2].leftDerivative = -0.75;
    feather->rightYCurve.keyframes[2].rightDerivative = 1.5;
    BezierSerialization freeKey = makeBezier("Free1", "Layer1", 3, 70);
    freeKey.controlPoints.front().yCurve.keyframes[10].interpolation = 6;
    freeKey.controlPoints.front().yCurve.keyframes[10].leftDerivative = 0.5;
    freeKey.controlPoints.front().yCurve.keyframes[10].rightDerivative = 0.5;
    const RotoContextSerialization roto = contextWith({ asChild(bezier), asChild(freeKey) });

    Serialization::MemoryBlobStore blobs;
    std::string text;
    const RotoContextSerialization decoded = roundTrip(roto, &blobs, &text);
    EXPECT_TRUE(blobs.members().empty());
    EXPECT_EQ(std::string::npos, text.find("shape:")) << text;
    EXPECT_NE(std::string::npos, text.find("curves:")) << text;
    EXPECT_NE(std::string::npos, text.find("featherCurves:")) << text;
    expectRotoEqual(roto, decoded);
}

TEST(RotoYaml, NestedLayersSmallStrokesAndFallbacks)
{
    RotoStrokeItemSerialization small = makeStroke("Small1", "Inner", { 10, 0, 5 }, 2);
    RotoStrokeItemSerialization mixed = makeStroke("Mixed1", "Inner", { 40, 40 }, 1);
    mixed.strokes.back().xCurve.keyframes[3].interpolation = 0;
    mixed.strokes.back().yCurve.keyframes[3].interpolation = 0;
    mixed.strokes.back().pressureCurve.keyframes[3].interpolation = 0;
    RotoStrokeItemSerialization unshared = makeStroke("Unshared1", "Elsewhere", { 8 }, 1);
    unshared.strokes.front().pressureCurve.keyframes.pop_back();

    RotoLayerChildSerialization inner;
    inner.type = Serialization::eRotoLayerChildTypeLayer;
    setItem(&inner.layer, "Inner", "Layer1");
    inner.layer.activated = false;
    inner.layer.children = { asChild(small), asChild(mixed), asChild(unshared) };
    RotoContextSerialization roto = contextWith({ inner, asChild(makeBezier("Empty1", "Layer1", 0, 0)) });
    roto.selectedItems = { "Small1", "Empty1" };
    roto.rippleEdit = true;

    Serialization::MemoryBlobStore blobs;
    std::string text;
    const RotoContextSerialization decoded = roundTrip(roto, &blobs, &text);
    ASSERT_EQ(1u, blobs.members().size());
    EXPECT_EQ("blobs/Group1.RotoPaint1/Mixed1.stroke.bin", blobs.members().begin()->first);
    EXPECT_NE(std::string::npos, text.find("segments: [10, 0, 5]\n")) << text;
    EXPECT_NE(std::string::npos, text.find("rows: 15, columns: [x:f64, y:f64, pressure:f64, time:f64], values: [")) << text;
    EXPECT_NE(std::string::npos, text.find("rows: 80, columns: [x:f64, y:f64, pressure:f64, time:f64, interp:u8]}")) << text;
    EXPECT_NE(std::string::npos, text.find("parent: \"Elsewhere\"")) << text;
    expectRotoEqual(roto, decoded);
}

TEST(RotoYaml, SelfContainedDocumentsInlineTheirBlobs)
{
    const RotoContextSerialization roto = contextWith({ asChild(makeStroke("Stroke1", "Layer1", { 300 }, 1)), asChild(makeBezier("Bezier1", "Layer1", 8, 10)) });
    Serialization::InlineBlobSink sink;
    const std::string text = Serialization::encodeRotoYaml(roto, "RotoPaint1", sink);
    EXPECT_NE(std::string::npos, text.find("data: !!binary")) << text;
    EXPECT_EQ(std::string::npos, text.find("blob:")) << text;

    Serialization::NoBlobSource source;
    YamlWarnings warnings;
    expectRotoEqual(roto, Serialization::decodeRotoYaml(text, source, &warnings));
    EXPECT_TRUE(warnings.empty());
}

TEST(RotoYaml, StrokeSegmentCountsMustExactlyCoverTheSampleRows)
{
    const RotoContextSerialization roto = contextWith({ asChild(makeStroke("Stroke1", "Layer1", { 3 }, 1)) });
    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeRotoYaml(roto, "RotoPaint1", blobs);
    const std::string original = "segments: [3]";
    ASSERT_NE(std::string::npos, text.find(original)) << text;

    YamlWarnings warnings;
    EXPECT_NO_THROW(Serialization::decodeRotoYaml(text, blobs, &warnings));

    // The first two would wrap a 64-bit sum around to the table's 3 rows.
    for (const std::string& segments : { std::string("segments: [9223372036854775807, 9223372036854775807, 5]"),
                                         std::string("segments: [-1, 4]"),
                                         std::string("segments: [2, 2]"),
                                         std::string("segments: [4]"),
                                         std::string("segments: [1, 1]") }) {
        SCOPED_TRACE(segments);
        std::string edited = text;
        edited.replace(edited.find(original), original.size(), segments);
        EXPECT_THROW(Serialization::decodeRotoYaml(edited, blobs, &warnings), Serialization::YamlError);
    }
}

TEST(RotoYaml, MissingBlobGivesAWarningAndAnEmptyStroke)
{
    const RotoContextSerialization roto = contextWith({ asChild(makeStroke("Stroke1", "Layer1", { 100 }, 1)) });
    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeRotoYaml(roto, "RotoPaint1", blobs);
    blobs.removeMember("blobs/RotoPaint1/Stroke1.stroke.bin");

    YamlWarnings warnings;
    const RotoContextSerialization decoded = Serialization::decodeRotoYaml(text, blobs, &warnings);
    ASSERT_EQ(1u, warnings.size());
    EXPECT_EQ("roto.layer.items[0].stroke.samples: the blob member \"blobs/RotoPaint1/Stroke1.stroke.bin\" is missing; its data is left empty", warnings[0]);
    ASSERT_EQ(1u, decoded.baseLayer.children.size());
    EXPECT_EQ("Stroke1", decoded.baseLayer.children.front().stroke.name);
    EXPECT_TRUE(decoded.baseLayer.children.front().stroke.strokes.empty());
}
