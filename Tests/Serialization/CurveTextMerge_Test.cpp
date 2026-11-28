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

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Serialization/Merge/MergeHandler.h"

NATRON_NAMESPACE_USING

namespace {

constexpr int kSmooth = 2;
constexpr int kFree = 6;

Serialization::CurveSerialization
curve(const std::map<double, double>& keys,
      int interpolation = kSmooth)
{
    Serialization::CurveSerialization result;
    for (const std::pair<const double, double>& entry : keys) {
        Serialization::KeyFrameSerialization key;
        key.time = entry.first;
        key.value = entry.second;
        key.interpolation = interpolation;
        result.keyframes.push_back(key);
    }

    return result;
}

std::map<double, double>
keysOf(const Serialization::CurveSerialization& curve)
{
    std::map<double, double> result;
    for (const Serialization::KeyFrameSerialization& key : curve.keyframes) {
        result[key.time] = key.value;
    }

    return result;
}

// A one-dimension Double knob defaulting to 1: animated when keys are given, else at value.
Serialization::KnobSerialization
doubleKnob(const std::map<double, double>& keys,
           double value = 1.)
{
    Serialization::KnobSerialization knob;
    knob.name = "multiply";
    knob.typeName = "Double";
    knob.dimension = 1;
    knob.dimensions.resize(1);
    knob.dimensions[0].defaultValue = Serialization::KnobValueSerialization(1.);
    knob.dimensions[0].value = Serialization::KnobValueSerialization(value);
    if (!keys.empty()) {
        knob.dimensions[0].curve = curve(keys);
    }

    return knob;
}

Serialization::KnobSerialization
stringKnob(const std::string& value)
{
    Serialization::KnobSerialization knob;
    knob.name = "script";
    knob.typeName = "String";
    knob.dimension = 1;
    knob.dimensions.resize(1);
    knob.dimensions[0].defaultValue = Serialization::KnobValueSerialization(std::string());
    knob.dimensions[0].value = Serialization::KnobValueSerialization(value);

    return knob;
}

std::string
stringOf(const std::optional<Serialization::KnobSerialization>& knob)
{
    return std::get<std::string>(*knob->dimensions[0].value);
}

} // namespace

TEST(CurveMerge, KeysMergeByTimeAndConflictOnlyWhereBothChangedOneKey)
{
    std::vector<Serialization::CurveKeyConflict> conflicts;
    const Serialization::CurveSerialization merged = Serialization::mergeCurves(curve({ { 1., 1. }, { 10., 2. } }),
                                                                                curve({ { 1., 1. }, { 10., 2.5 }, { 20., 3. } }),
                                                                                curve({ { 1., 1.2 }, { 10., 1.8 }, { 30., 0.5 } }),
                                                                                &conflicts);
    const std::map<double, double> expected = { { 1., 1.2 }, { 10., 2.5 }, { 20., 3. }, { 30., 0.5 } };
    EXPECT_EQ(keysOf(merged), expected);
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].time, 10.);
    ASSERT_TRUE(conflicts[0].base && conflicts[0].mine && conflicts[0].theirs);
    EXPECT_EQ(conflicts[0].base->value, 2.);
    EXPECT_EQ(conflicts[0].mine->value, 2.5);
    EXPECT_EQ(conflicts[0].theirs->value, 1.8);
}

TEST(CurveMerge, AKeyOneSideRemovedAndTheOtherLeftIsRemoved)
{
    std::vector<Serialization::CurveKeyConflict> conflicts;
    const Serialization::CurveSerialization merged = Serialization::mergeCurves(curve({ { 1., 1. }, { 10., 2. } }), curve({ { 1., 1. } }), curve({ { 1., 1. }, { 10., 2. } }), &conflicts);
    EXPECT_EQ(keysOf(merged), (std::map<double, double> { { 1., 1. } }));
    EXPECT_TRUE(conflicts.empty());
}

TEST(CurveMerge, RemovingAKeyTheOtherSideChangedConflictsAndKeepsMine)
{
    std::vector<Serialization::CurveKeyConflict> conflicts;
    const Serialization::CurveSerialization merged = Serialization::mergeCurves(curve({ { 1., 1. }, { 10., 2. } }), curve({ { 1., 1. } }), curve({ { 1., 1. }, { 10., 4. } }), &conflicts);
    EXPECT_EQ(keysOf(merged), (std::map<double, double> { { 1., 1. } }));
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_FALSE(conflicts[0].mine.has_value());
    ASSERT_TRUE(conflicts[0].theirs.has_value());
    EXPECT_EQ(conflicts[0].theirs->value, 4.);
}

TEST(CurveMerge, RecomputedTangentsAreNotAChangeButFreeTangentsAre)
{
    Serialization::CurveSerialization recomputed = curve({ { 1., 1. } });
    recomputed.keyframes[0].rightDerivative = 0.5;
    std::vector<Serialization::CurveKeyConflict> conflicts;
    Serialization::CurveSerialization merged = Serialization::mergeCurves(curve({ { 1., 1. } }), recomputed, curve({ { 1., 3. } }), &conflicts);
    EXPECT_EQ(keysOf(merged), (std::map<double, double> { { 1., 3. } }));
    EXPECT_TRUE(conflicts.empty());

    Serialization::CurveSerialization freeTangent = curve({ { 1., 1. } }, kFree);
    freeTangent.keyframes[0].rightDerivative = 0.5;
    merged = Serialization::mergeCurves(curve({ { 1., 1. } }, kFree), freeTangent, curve({ { 1., 3. } }, kFree), &conflicts);
    EXPECT_EQ(keysOf(merged), (std::map<double, double> { { 1., 1. } }));
    EXPECT_EQ(conflicts.size(), 1u);
}

TEST(CurveMerge, TheScalarHandlerMergesADimensionBothSidesAnimatedKeyByKey)
{
    const Serialization::KnobSerialization base = doubleKnob({ { 1., 1. }, { 10., 2. } });
    const Serialization::KnobSerialization mine = doubleKnob({ { 1., 1. }, { 10., 2.5 }, { 20., 3. } });
    const Serialization::KnobSerialization theirs = doubleKnob({ { 1., 1.2 }, { 10., 1.8 }, { 30., 0.5 } });
    std::optional<Serialization::KnobSerialization> merged;
    std::vector<Serialization::MergeConflict> conflicts;
    Serialization::MergeHandlerRegistry::defaults().handlerFor("Grade", "Double").merge(&base, &mine, &theirs, &merged, &conflicts);
    ASSERT_TRUE(merged && merged->dimensions[0].curve);
    EXPECT_EQ(keysOf(*merged->dimensions[0].curve), (std::map<double, double> { { 1., 1.2 }, { 10., 2.5 }, { 20., 3. }, { 30., 0.5 } }));
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].kind, Serialization::MergeConflictKind::eCurveKey);
    EXPECT_EQ(conflicts[0].knob, "multiply");
    EXPECT_EQ(conflicts[0].dimension, 0);
    ASSERT_TRUE(conflicts[0].time.has_value());
    EXPECT_EQ(*conflicts[0].time, 10.);
    ASSERT_TRUE(conflicts[0].mine.key && conflicts[0].theirs.key);
    EXPECT_EQ(conflicts[0].mine.key->value, 2.5);
    EXPECT_EQ(conflicts[0].theirs.key->value, 1.8);
}

TEST(CurveMerge, AnimatingADimensionTheOtherSideSetConflictsOnTheWholeDimension)
{
    const Serialization::KnobSerialization base = doubleKnob({}, 2.);
    const Serialization::KnobSerialization mine = doubleKnob({ { 1., 1. }, { 10., 3. } });
    const Serialization::KnobSerialization theirs = doubleKnob({}, 4.);
    std::optional<Serialization::KnobSerialization> merged;
    std::vector<Serialization::MergeConflict> conflicts;
    Serialization::MergeHandlerRegistry::defaults().handlerFor("Grade", "Double").merge(&base, &mine, &theirs, &merged, &conflicts);
    ASSERT_TRUE(merged && merged->dimensions[0].curve);
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].kind, Serialization::MergeConflictKind::eValue);
    EXPECT_EQ(conflicts[0].dimension, 0);
    EXPECT_FALSE(conflicts[0].time.has_value());
    ASSERT_TRUE(conflicts[0].base.knob && conflicts[0].mine.knob && conflicts[0].theirs.knob);
    EXPECT_EQ(*conflicts[0].theirs.knob, theirs);
}

TEST(CurveMerge, AnExpressionOnOneSideKeepsTheOtherSidesKeys)
{
    const Serialization::KnobSerialization base = doubleKnob({ { 1., 1. } });
    Serialization::KnobSerialization mine = doubleKnob({ { 1., 1. } });
    mine.dimensions[0].expression = "frame";
    const Serialization::KnobSerialization theirs = doubleKnob({ { 1., 5. } });
    std::optional<Serialization::KnobSerialization> merged;
    std::vector<Serialization::MergeConflict> conflicts;
    Serialization::MergeHandlerRegistry::defaults().handlerFor("Grade", "Double").merge(&base, &mine, &theirs, &merged, &conflicts);
    ASSERT_TRUE(merged && merged->dimensions[0].curve);
    EXPECT_EQ(merged->dimensions[0].expression, "frame");
    EXPECT_EQ(keysOf(*merged->dimensions[0].curve), (std::map<double, double> { { 1., 5. } }));
    EXPECT_TRUE(conflicts.empty());
}

TEST(CurveMerge, ParametricCurvesMergeCurveByCurve)
{
    Serialization::KnobSerialization base;
    base.name = "curves";
    base.typeName = "Parametric";
    base.parametricCurves = std::list<Serialization::CurveSerialization> { curve({ { 0., 0. }, { 1., 1. } }), curve({ { 0., 0. }, { 1., 1. } }) };
    Serialization::KnobSerialization mine = base;
    mine.parametricCurves->front() = curve({ { 0., 0. }, { 0.5, 0.7 }, { 1., 1. } });
    mine.parametricCurves->back() = curve({ { 0., 0.1 }, { 1., 1. } });
    Serialization::KnobSerialization theirs = base;
    theirs.parametricCurves->back() = curve({ { 0., 0.2 }, { 1., 0.9 } });
    std::optional<Serialization::KnobSerialization> merged;
    std::vector<Serialization::MergeConflict> conflicts;
    Serialization::MergeHandlerRegistry::defaults().handlerFor("ColorLookup", "Parametric").merge(&base, &mine, &theirs, &merged, &conflicts);
    ASSERT_TRUE(merged && merged->parametricCurves);
    EXPECT_EQ(keysOf(merged->parametricCurves->front()), (std::map<double, double> { { 0., 0. }, { 0.5, 0.7 }, { 1., 1. } }));
    EXPECT_EQ(keysOf(merged->parametricCurves->back()), (std::map<double, double> { { 0., 0.1 }, { 1., 0.9 } }));
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].kind, Serialization::MergeConflictKind::eCurveKey);
    EXPECT_EQ(conflicts[0].dimension, 1);
    EXPECT_EQ(conflicts[0].time, std::optional<double>(0.));
}

TEST(TextMerge, EditsToDifferentLinesMerge)
{
    std::string merged;
    ASSERT_TRUE(Serialization::mergeTextLines("a\nb\nc\nd\n", "a\nB\nc\nd\n", "a\nb\nc\nD\ne", &merged));
    EXPECT_EQ(merged, "a\nB\nc\nD\ne");
    ASSERT_TRUE(Serialization::mergeTextLines("a\nb\n", "x\na\nb\n", "a\nb\n", &merged));
    EXPECT_EQ(merged, "x\na\nb\n");
    ASSERT_TRUE(Serialization::mergeTextLines("a\nb\nc\n", "a\nc\n", "a\nb\nc\nz\n", &merged));
    EXPECT_EQ(merged, "a\nc\nz\n");
}

TEST(TextMerge, EditsToOneLineOrInsertsAtOnePlaceConflict)
{
    std::string merged = "untouched";
    EXPECT_FALSE(Serialization::mergeTextLines("a\nb\nc\n", "a\nX\nc\n", "a\nY\nc\n", &merged));
    EXPECT_FALSE(Serialization::mergeTextLines("a\nb\n", "a\nb\nx\n", "a\nb\ny\n", &merged));
    EXPECT_EQ(merged, "untouched");
    ASSERT_TRUE(Serialization::mergeTextLines("a\n", "a\nx\n", "a\nx\n", &merged));
    EXPECT_EQ(merged, "a\nx\n");
}

TEST(TextMerge, StringKnobsMergeLineByLine)
{
    const Serialization::KnobSerialization base = stringKnob("first\nsecond\nthird\n");
    const Serialization::KnobSerialization mine = stringKnob("first!\nsecond\nthird\n");
    const Serialization::KnobSerialization theirs = stringKnob("first\nsecond\nthird!\n");
    const Serialization::KnobMergeHandler& handler = Serialization::MergeHandlerRegistry::defaults().handlerFor("Text", "String");
    std::optional<Serialization::KnobSerialization> merged;
    std::vector<Serialization::MergeConflict> conflicts;
    handler.merge(&base, &mine, &theirs, &merged, &conflicts);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(stringOf(merged), "first!\nsecond\nthird!\n");
    EXPECT_TRUE(conflicts.empty());

    const Serialization::KnobSerialization clash = stringKnob("first?\nsecond\nthird\n");
    handler.merge(&base, &mine, &clash, &merged, &conflicts);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(stringOf(merged), "first!\nsecond\nthird\n");
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].kind, Serialization::MergeConflictKind::eValue);
    EXPECT_EQ(conflicts[0].dimension, 0);
}

TEST(TextMerge, AStringBothSidesSetFromTheDefaultMergesFromAnEmptyBase)
{
    const Serialization::KnobSerialization mine = stringKnob("mine\n");
    const Serialization::KnobSerialization theirs = stringKnob("theirs\n");
    std::optional<Serialization::KnobSerialization> merged;
    std::vector<Serialization::MergeConflict> conflicts;
    Serialization::MergeHandlerRegistry::defaults().handlerFor("Text", "String").merge(nullptr, &mine, &theirs, &merged, &conflicts);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(stringOf(merged), "mine\n");
    EXPECT_EQ(conflicts.size(), 1u);
}
