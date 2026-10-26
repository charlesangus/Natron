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

#ifndef Tests_Serialization_YamlTestHelpers_h
#define Tests_Serialization_YamlTestHelpers_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "Serialization/CurveSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/Yaml/CurveYaml.h"
#include "Serialization/Yaml/KnobYaml.h"

namespace YamlTest {

inline std::uint64_t
bitsOf(double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    return bits;
}

inline std::uint32_t
bitsOfFloat(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    return bits;
}

// Equal bit patterns, except that any NaN equals any NaN: the format keeps NaN, not its payload.
inline bool
sameDouble(double a,
           double b)
{
    if ((a != a) && (b != b)) {
        return true;
    }

    return bitsOf(a) == bitsOf(b);
}

#define EXPECT_SAME_DOUBLE(expected, actual) EXPECT_TRUE(YamlTest::sameDouble((expected), (actual))) << #actual << ": expected " << (expected) << " got " << (actual)

} // namespace YamlTest

inline void
expectCurvesBitIdentical(const NATRON_NAMESPACE::Serialization::CurveSerialization& expected,
                         const NATRON_NAMESPACE::Serialization::CurveSerialization& actual)
{
    ASSERT_EQ(expected.keyframes.size(), actual.keyframes.size());
    for (std::size_t i = 0; i < expected.keyframes.size(); ++i) {
        SCOPED_TRACE("keyframe " + std::to_string(i));
        const NATRON_NAMESPACE::Serialization::KeyFrameSerialization& e = expected.keyframes[i];
        const NATRON_NAMESPACE::Serialization::KeyFrameSerialization& a = actual.keyframes[i];
        EXPECT_SAME_DOUBLE(e.time, a.time);
        EXPECT_SAME_DOUBLE(e.value, a.value);
        EXPECT_EQ(e.interpolation, a.interpolation);
        EXPECT_SAME_DOUBLE(e.leftDerivative, a.leftDerivative);
        EXPECT_SAME_DOUBLE(e.rightDerivative, a.rightDerivative);
        EXPECT_SAME_DOUBLE(e.leftWeight, a.leftWeight);
        EXPECT_SAME_DOUBLE(e.rightWeight, a.rightWeight);
    }
}

// The curve as the YAML format keeps it, compared bit for bit.
inline void
expectCurvesEquivalent(const NATRON_NAMESPACE::Serialization::CurveSerialization& expected,
                       const NATRON_NAMESPACE::Serialization::CurveSerialization& actual)
{
    expectCurvesBitIdentical(NATRON_NAMESPACE::Serialization::canonicalCurveForYaml(expected), actual);
}

namespace YamlTest {

// What the YAML format keeps of a knob toSerialization() filled: a value under a curve, an
// expression or a link is recomputed on load, and for a knob that is not a user knob, what its
// plug-in defines (defaults, label, help, flags, the choice menu) is not saved.
inline NATRON_NAMESPACE::Serialization::KnobSerialization
canonicalKnob(const NATRON_NAMESPACE::Serialization::KnobSerialization& knob)
{
    using namespace NATRON_NAMESPACE::Serialization;
    KnobSerialization result = knob;
    for (KnobDimensionSerialization& dimension : result.dimensions) {
        if (dimension.curve || !dimension.expression.empty() || dimension.expressionHasReturnVariable || dimension.master) {
            dimension.value.reset();
        }
        if (dimension.curve) {
            dimension.curve = canonicalCurveForYaml(*dimension.curve);
        }
        if (!result.isUserKnob) {
            dimension.defaultValue.reset();
        }
    }
    if (result.parametricCurves) {
        for (CurveSerialization& curve : *result.parametricCurves) {
            curve = canonicalCurveForYaml(curve);
        }
    }
    if (!result.isUserKnob) {
        const KnobSerialization defaults;
        result.label = defaults.label;
        result.tooltip = defaults.tooltip;
        result.triggerNewLine = defaults.triggerNewLine;
        result.evaluatesOnChange = defaults.evaluatesOnChange;
        result.animationEnabled = defaults.animationEnabled;
        result.isPersistent = defaults.isPersistent;
        result.extraData = defaults.extraData;
        result.useHostOverlayHandle = defaults.useHostOverlayHandle;
    }

    return result;
}

inline std::string
describeValue(const std::optional<NATRON_NAMESPACE::Serialization::KnobValueSerialization>& value)
{
    if (!value) {
        return "(none)";
    }
    if (const bool* b = std::get_if<bool>(&(*value))) {
        return std::string("bool ") + (*b ? "true" : "false");
    }
    if (const int* i = std::get_if<int>(&(*value))) {
        return "int " + std::to_string(*i);
    }
    if (const double* d = std::get_if<double>(&(*value))) {
        return "double " + std::to_string(*d);
    }

    return "string \"" + std::get<std::string>(*value) + "\"";
}

inline bool
sameValue(const std::optional<NATRON_NAMESPACE::Serialization::KnobValueSerialization>& a,
          const std::optional<NATRON_NAMESPACE::Serialization::KnobValueSerialization>& b)
{
    if (a.has_value() != b.has_value()) {
        return false;
    }
    if (!a) {
        return true;
    }
    if (a->index() != b->index()) {
        return false;
    }
    if (const double* d = std::get_if<double>(&(*a))) {
        return sameDouble(*d, std::get<double>(*b));
    }

    return *a == *b;
}

} // namespace YamlTest

// Field by field, expected being what the format keeps (see YamlTest::canonicalKnob).
inline void
expectKnobsEqual(const NATRON_NAMESPACE::Serialization::KnobSerialization& expected,
                 const NATRON_NAMESPACE::Serialization::KnobSerialization& actual)
{
    using namespace NATRON_NAMESPACE::Serialization;
    SCOPED_TRACE("knob " + expected.name);
    EXPECT_EQ(expected.name, actual.name);
    EXPECT_EQ(expected.typeName, actual.typeName);
    EXPECT_EQ(expected.dimension, actual.dimension);
    EXPECT_EQ(expected.secret, actual.secret);
    EXPECT_EQ(expected.masterIsAlias, actual.masterIsAlias);
    ASSERT_EQ(expected.dimensions.size(), actual.dimensions.size());
    for (std::size_t i = 0; i < expected.dimensions.size(); ++i) {
        SCOPED_TRACE("dimension " + std::to_string(i));
        const KnobDimensionSerialization& e = expected.dimensions[i];
        const KnobDimensionSerialization& a = actual.dimensions[i];
        EXPECT_EQ(e.enabled, a.enabled);
        ASSERT_EQ(e.curve.has_value(), a.curve.has_value());
        if (e.curve) {
            expectCurvesBitIdentical(*e.curve, *a.curve);
        }
        EXPECT_TRUE(YamlTest::sameValue(e.value, a.value)) << "value: expected " << YamlTest::describeValue(e.value) << ", got " << YamlTest::describeValue(a.value);
        EXPECT_TRUE(YamlTest::sameValue(e.defaultValue, a.defaultValue)) << "default: expected " << YamlTest::describeValue(e.defaultValue) << ", got " << YamlTest::describeValue(a.defaultValue);
        EXPECT_EQ(e.master, a.master);
        EXPECT_EQ(e.expression, a.expression);
        EXPECT_EQ(e.expressionHasReturnVariable, a.expressionHasReturnVariable);
    }
    ASSERT_EQ(expected.parametricCurves.has_value(), actual.parametricCurves.has_value());
    if (expected.parametricCurves) {
        ASSERT_EQ(expected.parametricCurves->size(), actual.parametricCurves->size());
        auto a = actual.parametricCurves->begin();
        for (auto e = expected.parametricCurves->begin(); e != expected.parametricCurves->end(); ++e, ++a) {
            expectCurvesBitIdentical(*e, *a);
        }
    }
    EXPECT_EQ(expected.stringAnimation, actual.stringAnimation);
    EXPECT_EQ(expected.choiceOptionID, actual.choiceOptionID);
    EXPECT_EQ(expected.isUserKnob, actual.isUserKnob);
    EXPECT_EQ(expected.label, actual.label);
    EXPECT_EQ(expected.tooltip, actual.tooltip);
    EXPECT_EQ(expected.triggerNewLine, actual.triggerNewLine);
    EXPECT_EQ(expected.evaluatesOnChange, actual.evaluatesOnChange);
    EXPECT_EQ(expected.animationEnabled, actual.animationEnabled);
    EXPECT_EQ(expected.isPersistent, actual.isPersistent);
    ASSERT_EQ(expected.extraData.index(), actual.extraData.index());
    if (const KnobChoiceExtraSerialization* e = std::get_if<KnobChoiceExtraSerialization>(&expected.extraData)) {
        const KnobChoiceExtraSerialization& a = std::get<KnobChoiceExtraSerialization>(actual.extraData);
        EXPECT_EQ(e->entries, a.entries);
        EXPECT_EQ(e->helpStrings, a.helpStrings);
    } else if (const KnobValueRangeSerialization* e = std::get_if<KnobValueRangeSerialization>(&expected.extraData)) {
        const KnobValueRangeSerialization& a = std::get<KnobValueRangeSerialization>(actual.extraData);
        EXPECT_SAME_DOUBLE(e->minimum, a.minimum);
        EXPECT_SAME_DOUBLE(e->maximum, a.maximum);
        EXPECT_SAME_DOUBLE(e->displayMinimum, a.displayMinimum);
        EXPECT_SAME_DOUBLE(e->displayMaximum, a.displayMaximum);
    } else if (const KnobFileExtraSerialization* e = std::get_if<KnobFileExtraSerialization>(&expected.extraData)) {
        EXPECT_EQ(e->useSequences, std::get<KnobFileExtraSerialization>(actual.extraData).useSequences);
    } else if (const KnobPathExtraSerialization* e = std::get_if<KnobPathExtraSerialization>(&expected.extraData)) {
        EXPECT_EQ(e->multiPath, std::get<KnobPathExtraSerialization>(actual.extraData).multiPath);
    } else if (const KnobLayerSelectExtraSerialization* e = std::get_if<KnobLayerSelectExtraSerialization>(&expected.extraData)) {
        EXPECT_EQ(e->withChannelButtons, std::get<KnobLayerSelectExtraSerialization>(actual.extraData).withChannelButtons);
    } else if (const KnobTextExtraSerialization* e = std::get_if<KnobTextExtraSerialization>(&expected.extraData)) {
        const KnobTextExtraSerialization& a = std::get<KnobTextExtraSerialization>(actual.extraData);
        EXPECT_EQ(e->isLabel, a.isLabel);
        EXPECT_EQ(e->isMultiLine, a.isMultiLine);
        EXPECT_EQ(e->usesRichText, a.usesRichText);
    }
    EXPECT_EQ(expected.useHostOverlayHandle, actual.useHostOverlayHandle);
}

// Gives decoded knobs the type the live knobs would, taken from the knobs that were encoded.
inline void
completeLike(std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>* decoded,
             const std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>& original)
{
    using namespace NATRON_NAMESPACE::Serialization;
    for (KnobSerialization& knob : *decoded) {
        for (const KnobSerialization& source : original) {
            if (source.name != knob.name) {
                continue;
            }
            const KnobChoiceExtraSerialization* choice = std::get_if<KnobChoiceExtraSerialization>(&source.extraData);
            completeKnobSerialization(&knob, source.typeName, source.dimension, choice ? &choice->entries : nullptr);
            break;
        }
    }
}

inline void
expectKnobListsEqual(const std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>& expected,
                     const std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    auto a = actual.begin();
    for (auto e = expected.begin(); e != expected.end(); ++e, ++a) {
        expectKnobsEqual(*e, *a);
    }
}

inline void
expectExpectedKnobs(const std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>& original,
                    std::list<NATRON_NAMESPACE::Serialization::KnobSerialization> decoded)
{
    std::list<NATRON_NAMESPACE::Serialization::KnobSerialization> expected;
    for (const NATRON_NAMESPACE::Serialization::KnobSerialization& knob : original) {
        expected.push_back(YamlTest::canonicalKnob(knob));
    }
    completeLike(&decoded, original);
    expectKnobListsEqual(expected, decoded);
}

inline void
expectGroupKnobsEqual(const NATRON_NAMESPACE::Serialization::GroupKnobSerialization& expected,
                      const NATRON_NAMESPACE::Serialization::GroupKnobSerialization& actual)
{
    SCOPED_TRACE("user page or group " + expected.name);
    EXPECT_EQ(expected.name, actual.name);
    EXPECT_EQ(expected.label, actual.label);
    EXPECT_EQ(expected.secret, actual.secret);
    EXPECT_EQ(expected.isSetAsTab, actual.isSetAsTab);
    EXPECT_EQ(expected.isOpened, actual.isOpened);
    EXPECT_EQ(expected.isRFVGroup, actual.isRFVGroup);
    ASSERT_EQ(expected.children.size(), actual.children.size());
    auto a = actual.children.begin();
    for (auto e = expected.children.begin(); e != expected.children.end(); ++e, ++a) {
        ASSERT_EQ(e->isGroup, a->isGroup);
        if (e->isGroup) {
            expectGroupKnobsEqual(e->group, a->group);
        } else {
            expectKnobsEqual(YamlTest::canonicalKnob(e->knob), a->knob);
        }
    }
}

#endif // Tests_Serialization_YamlTestHelpers_h
