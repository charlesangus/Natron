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

#include <limits>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Serialization/KnobSerialization.h"
#include "Serialization/Yaml/KnobYaml.h"
#include "YamlTestHelpers.h"

NATRON_NAMESPACE_USING
using Serialization::CurveSerialization;
using Serialization::KeyFrameSerialization;
using Serialization::KnobDimensionSerialization;
using Serialization::KnobSerialization;
using Serialization::KnobValueSerialization;
using Serialization::YamlError;
using Serialization::YamlWarnings;

namespace {

// Every type a project file can create a knob of, with the dimension count it is tested with.
struct KnobTypeCase {
    const char* typeName;
    int dimension;
};

const KnobTypeCase kKnobTypes[] = {
    { "Int", 1 }, { "Bool", 1 }, { "Double", 2 }, { "Choice", 1 }, { "String", 1 }, { "Parametric", 3 }, { "Color", 4 }, { "Path", 1 }, { "Layers", 1 }, { "ChannelSet", 1 }, { "LayerSelect", 1 }, { "ChannelSelect", 1 }, { "ShuffleMap", 1 }, { "InputFile", 1 }, { "OutputFile", 1 }, { "Button", 1 }, { "Separator", 1 }, { "Group", 1 }, { "Page", 1 }
};

enum KnobVariantEnum {
    eKnobVariantStatic = 0,
    eKnobVariantAnimated,
    eKnobVariantExpression,
    eKnobVariantLinked,
    eKnobVariantUserKnob
};

const KnobVariantEnum kKnobVariants[] = {
    eKnobVariantStatic, eKnobVariantAnimated, eKnobVariantExpression, eKnobVariantLinked, eKnobVariantUserKnob
};

const char*
variantName(KnobVariantEnum variant)
{
    switch (variant) {
    case eKnobVariantStatic:
        return "Static";
    case eKnobVariantAnimated:
        return "Animated";
    case eKnobVariantExpression:
        return "Expression";
    case eKnobVariantLinked:
        return "Linked";
    case eKnobVariantUserKnob:
        return "UserKnob";
    }

    return "";
}

// The string encodings Engine gives these knobs' values.
std::string
stringValueFor(const std::string& typeName)
{
    if (typeName == "ChannelSet") {
        return "rgba spec.*";
    } else if (typeName == "LayerSelect") {
        return "diffuse";
    } else if (typeName == "ShuffleMap") {
        return "r=A.r g=B.g b=0 a=1";
    } else if (typeName == "ChannelSelect") {
        return "B.alpha";
    } else if (typeName == "Layers") {
        return "<Layer><Name>diffuse</Name><NbComps>3</NbComps></Layer>";
    }

    return "saved \"value\"\nwith a newline, a tab\t and \xc3\xa9";
}

CurveSerialization
makeCurve(int keys,
          double scale,
          int interpolation)
{
    CurveSerialization curve;
    for (int i = 0; i < keys; ++i) {
        KeyFrameSerialization key;
        key.time = 1. + i * 5.;
        key.value = scale * (i + 0.125);
        key.interpolation = interpolation;
        key.leftDerivative = (interpolation == 7) ? -0.5 * i : 0.;
        key.rightDerivative = (interpolation == 6 || interpolation == 7) ? 0.25 * i : 0.;
        if (interpolation == 6) {
            key.leftDerivative = key.rightDerivative;
        }
        curve.keyframes.push_back(key);
    }

    return curve;
}

// What Engine's toSerialization() fills for a knob of the given type and variant.
KnobSerialization
makeKnob(const KnobTypeCase& type,
         KnobVariantEnum variant)
{
    const std::string typeName(type.typeName);
    const Serialization::KnobTypeTraits traits = Serialization::getKnobTypeTraits(typeName);
    KnobSerialization knob;
    knob.name = std::string("knob") + typeName + variantName(variant);
    knob.typeName = typeName;
    knob.dimension = type.dimension;
    knob.dimensions.resize(type.dimension);
    knob.label = "Plug-in label";
    knob.tooltip = "Plug-in help";
    knob.triggerNewLine = true;
    knob.evaluatesOnChange = true;
    knob.animationEnabled = true;
    knob.isPersistent = true;

    for (int i = 0; i < type.dimension; ++i) {
        KnobDimensionSerialization& dimension = knob.dimensions[i];
        if (!traits.hasValue) {
            continue;
        }
        switch (traits.storageType) {
        case Serialization::eKnobStorageTypeBool:
            dimension.value = KnobValueSerialization(std::in_place_type<bool>, true);
            dimension.defaultValue = KnobValueSerialization(std::in_place_type<bool>, false);
            break;
        case Serialization::eKnobStorageTypeInt:
            dimension.value = KnobValueSerialization(std::in_place_type<int>, traits.isChoice ? 2 : 7 + i);
            dimension.defaultValue = KnobValueSerialization(std::in_place_type<int>, traits.isChoice ? 0 : -3);
            break;
        case Serialization::eKnobStorageTypeDouble:
            dimension.value = KnobValueSerialization(std::in_place_type<double>, 0.25 + i);
            dimension.defaultValue = KnobValueSerialization(std::in_place_type<double>, i == 0 ? 1. : 0.1);
            break;
        case Serialization::eKnobStorageTypeString:
            dimension.value = KnobValueSerialization(std::in_place_type<std::string>, stringValueFor(typeName));
            dimension.defaultValue = KnobValueSerialization(std::in_place_type<std::string>, std::string());
            break;
        case Serialization::eKnobStorageTypeNone:
            break;
        }
    }
    if (traits.isChoice) {
        Serialization::KnobChoiceExtraSerialization choice;
        choice.entries = { "alpha", "beta", "gamma" };
        choice.helpStrings = { "The first entry", "The second entry", "The third entry" };
        knob.extraData = choice;
        knob.choiceOptionID = "gamma";
    }
    if (traits.isParametric) {
        knob.parametricCurves.emplace();
        for (int i = 0; i < type.dimension; ++i) {
            knob.parametricCurves->push_back(makeCurve(2 + i, 1. + i, i == 1 ? 2 : 1));
        }
    }
    if (traits.hasStringAnimation) {
        knob.stringAnimation.emplace();
    }

    switch (variant) {
    case eKnobVariantStatic:
        break;
    case eKnobVariantAnimated:
        if (traits.hasStringAnimation) {
            (*knob.stringAnimation)[1] = "first";
            (*knob.stringAnimation)[12] = "second \"quoted\"";
            CurveSerialization curve;
            double index = 0.;
            for (const auto& entry : *knob.stringAnimation) {
                KeyFrameSerialization key;
                key.time = entry.first;
                key.value = index;
                key.interpolation = 0;
                curve.keyframes.push_back(key);
                index += 1.;
            }
            knob.dimensions[0].curve = curve;
        } else {
            for (int i = 0; i < type.dimension; ++i) {
                knob.dimensions[i].curve = makeCurve(3 + i, 2. + i, (i % 2) ? 7 : 2);
            }
        }
        break;
    case eKnobVariantExpression:
        knob.dimensions[0].expression = (typeName == "Int") ? "3 + 4" : "ret = thisNode.size.get()\n# done";
        knob.dimensions[0].expressionHasReturnVariable = (typeName != "Int");
        break;
    case eKnobVariantLinked: {
        Serialization::KnobMasterSerialization master;
        master.masterDimension = 0;
        master.masterNodeName = "Master1";
        master.masterNodeNameFull = "Group1.Master1";
        master.masterKnobName = std::string("master") + knob.name;
        knob.dimensions[0].master = master;
        break;
    }
    case eKnobVariantUserKnob:
        knob.isUserKnob = true;
        knob.label = "User label";
        knob.tooltip = "A user knob";
        knob.triggerNewLine = false;
        knob.evaluatesOnChange = true;
        knob.animationEnabled = false;
        knob.isPersistent = true;
        if (traits.isString) {
            Serialization::KnobTextExtraSerialization text;
            text.isMultiLine = true;
            text.usesRichText = true;
            knob.extraData = text;
        } else if (traits.isDouble || traits.isInt || traits.isColor) {
            Serialization::KnobValueRangeSerialization range;
            range.minimum = -5.;
            range.maximum = std::numeric_limits<double>::max();
            range.displayMinimum = -2.;
            range.displayMaximum = 2.5;
            knob.extraData = range;
        } else if (traits.isInputFile || traits.isOutputFile) {
            Serialization::KnobFileExtraSerialization file;
            file.useSequences = true;
            knob.extraData = file;
        } else if (traits.isPath) {
            Serialization::KnobPathExtraSerialization path;
            path.multiPath = true;
            knob.extraData = path;
        } else if (traits.isLayerSelect) {
            Serialization::KnobLayerSelectExtraSerialization layerSelect;
            layerSelect.withChannelButtons = true;
            knob.extraData = layerSelect;
        }
        if (traits.isDouble && (type.dimension == 2)) {
            knob.useHostOverlayHandle = true;
        }
        break;
    }

    return knob;
}

std::vector<std::string>
choiceEntriesOf(const KnobSerialization& knob)
{
    const Serialization::KnobChoiceExtraSerialization* choice = std::get_if<Serialization::KnobChoiceExtraSerialization>(&knob.extraData);

    return choice ? choice->entries : std::vector<std::string>();
}

KnobSerialization
decodeOne(const std::string& text,
          YamlWarnings* warnings = nullptr)
{
    std::list<KnobSerialization> knobs = Serialization::decodeKnobsYaml(text, warnings);
    EXPECT_EQ(1u, knobs.size());

    return knobs.empty() ? KnobSerialization() : knobs.front();
}

void
expectDecodeError(const std::string& text,
                  const std::string& expectedFragment)
{
    SCOPED_TRACE(text);
    try {
        Serialization::decodeKnobsYaml(text);
        ADD_FAILURE() << "decoding did not throw";
    } catch (const YamlError& e) {
        EXPECT_NE(std::string(e.what()).find(expectedFragment), std::string::npos) << e.what();
    }
}

} // anonymous namespace

TEST(KnobYaml, EveryKnobTypeAndVariantRoundTrips)
{
    std::list<KnobSerialization> knobs;
    for (const KnobTypeCase& type : kKnobTypes) {
        for (KnobVariantEnum variant : kKnobVariants) {
            knobs.push_back(makeKnob(type, variant));
        }
    }

    const std::string text = Serialization::encodeKnobsYaml(knobs);
    YamlWarnings warnings;
    std::list<KnobSerialization> decoded = Serialization::decodeKnobsYaml(text, &warnings);
    EXPECT_TRUE(warnings.empty());
    ASSERT_EQ(knobs.size(), decoded.size());
    EXPECT_EQ(text, Serialization::encodeKnobsYaml(decoded));

    std::list<KnobSerialization>::iterator it = decoded.begin();
    for (const KnobSerialization& original : knobs) {
        if (!original.isUserKnob) {
            EXPECT_TRUE(it->typeName.empty()) << original.name;
            const std::vector<std::string> entries = choiceEntriesOf(original);
            Serialization::completeKnobSerialization(&(*it), original.typeName, original.dimension, &entries);
        }
        expectKnobsEqual(YamlTest::canonicalKnob(original), *it);
        ++it;
    }
}

TEST(KnobYaml, CompactForms)
{
    std::list<KnobSerialization> knobs;

    KnobSerialization size = makeKnob(kKnobTypes[2], eKnobVariantStatic);
    size.name = "size";
    size.dimensions.resize(1);
    size.dimensions[0].value = KnobValueSerialization(std::in_place_type<double>, 3.);
    knobs.push_back(size);

    KnobSerialization translate = makeKnob(kKnobTypes[2], eKnobVariantStatic);
    translate.name = "translate";
    knobs.push_back(translate);

    KnobSerialization operation = makeKnob(kKnobTypes[3], eKnobVariantStatic);
    operation.name = "operation";
    knobs.push_back(operation);

    KnobSerialization mix = makeKnob(kKnobTypes[2], eKnobVariantExpression);
    mix.name = "mix";
    mix.dimensions.resize(1);
    knobs.push_back(mix);

    KnobSerialization linked = makeKnob(kKnobTypes[0], eKnobVariantLinked);
    linked.name = "count";
    knobs.push_back(linked);

    KnobSerialization disabled = makeKnob(kKnobTypes[1], eKnobVariantStatic);
    disabled.name = "premult";
    disabled.dimensions[0].enabled = false;
    knobs.push_back(disabled);

    KnobSerialization filename = makeKnob(kKnobTypes[13], eKnobVariantAnimated);
    filename.name = "filename";
    knobs.push_back(filename);

    KnobSerialization hidden = makeKnob(kKnobTypes[0], eKnobVariantStatic);
    hidden.name = "hidden";
    hidden.secret = true;
    knobs.push_back(hidden);

    KnobSerialization animated = makeKnob(kKnobTypes[0], eKnobVariantStatic);
    animated.name = "frame";
    animated.dimensions[0].curve = makeCurve(3, 1., 1);
    knobs.push_back(animated);

    EXPECT_EQ("size: 3\n"
              "translate: [0.25, 1.25]\n"
              "operation: \"gamma\"\n"
              "mix: {expr: \"ret = thisNode.size.get()\\n# done\", hasRet: true}\n"
              "count: {link: \"Group1.Master1.masterknobIntLinked.0\"}\n"
              "premult: {value: true, enabled: false}\n"
              "filename: {curve: [1, \"first\", 12, \"second \\\"quoted\\\"\"]}\n"
              "hidden: {value: 7, secret: true}\n"
              "frame: {curve: [L, 1, 0.125, 6, 1.125, 11, 2.125]}\n",
              Serialization::encodeKnobsYaml(knobs));
}

TEST(KnobYaml, FoldedKnobKeepsOneSavedDimension)
{
    KnobSerialization color = makeKnob(kKnobTypes[6], eKnobVariantStatic);
    color.dimensions.resize(1);

    const std::string text = Serialization::encodeKnobsYaml({ color });
    EXPECT_EQ("knobColorStatic: 0.25\n", text);
    KnobSerialization decoded = decodeOne(text);
    ASSERT_EQ(1u, decoded.dimensions.size());
    ASSERT_TRUE(std::holds_alternative<int>(*decoded.dimensions[0].value) || std::holds_alternative<double>(*decoded.dimensions[0].value));
    Serialization::completeKnobSerialization(&decoded, "Color", 4);
    expectKnobsEqual(YamlTest::canonicalKnob(color), decoded);
}

TEST(KnobYaml, DriverRoundTripsAndKeepsTheOwnValue)
{
    KnobSerialization gain = makeKnob(kKnobTypes[2], eKnobVariantStatic);
    gain.name = "gain";
    gain.dimensions[0].value = KnobValueSerialization(std::in_place_type<double>, 0.5);
    Serialization::KnobDriverSerialization linear;
    linear.variable = "loop.t";
    linear.start = 1.;
    linear.end = 2.;
    linear.easing = "linear";
    gain.dimensions[0].driver = linear;
    Serialization::KnobDriverSerialization curved;
    curved.variable = "loops.outer.t";
    curved.start = -0.25;
    curved.end = 4.;
    curved.easing = "curve";
    curved.curve.emplace();
    for (int i = 0; i < 3; ++i) {
        KeyFrameSerialization key;
        key.time = 0.5 * i;
        key.value = (i == 1) ? 0.8 : 0.5 * i;
        key.interpolation = 1;
        curved.curve->keyframes.push_back(key);
    }
    gain.dimensions[1].driver = curved;

    const std::string text = Serialization::encodeKnobsYaml({ gain });
    EXPECT_NE(text.find("driver: {variable: \"loop.t\""), std::string::npos) << text;
    EXPECT_NE(text.find("value: 0.5"), std::string::npos) << text;
    YamlWarnings warnings;
    KnobSerialization decoded = decodeOne(text, &warnings);
    EXPECT_TRUE(warnings.empty());
    Serialization::completeKnobSerialization(&decoded, "Double", 2);
    expectKnobsEqual(YamlTest::canonicalKnob(gain), decoded);
    EXPECT_EQ(text, Serialization::encodeKnobsYaml({ decoded }));

    // An unknown key in a driver is skipped with a warning.
    KnobSerialization unknownKey = decodeOne("gain: {driver: {variable: \"loop.t\", start: 0, end: 1, easing: \"linear\", wobble: 1}}\n", &warnings);
    ASSERT_EQ(1u, unknownKey.dimensions.size());
    ASSERT_TRUE(unknownKey.dimensions[0].driver);
    EXPECT_FALSE(warnings.empty());
}

TEST(KnobYaml, ChoiceWhoseIndexDoesNotMatchItsOptionKeepsBoth)
{
    KnobSerialization choice = makeKnob(kKnobTypes[3], eKnobVariantStatic);
    choice.choiceOptionID = "delta";

    const std::string text = Serialization::encodeKnobsYaml({ choice });
    EXPECT_EQ("knobChoiceStatic: {value: 2, option: \"delta\"}\n", text);
    KnobSerialization decoded = decodeOne(text);
    const std::vector<std::string> entries = choiceEntriesOf(choice);
    Serialization::completeKnobSerialization(&decoded, "Choice", 1, &entries);
    expectKnobsEqual(YamlTest::canonicalKnob(choice), decoded);

    KnobSerialization unknownOption = decodeOne("operation: \"removed\"\n");
    Serialization::completeKnobSerialization(&unknownOption, "Choice", 1, &entries);
    EXPECT_EQ(std::string("removed"), *unknownOption.choiceOptionID);
    EXPECT_EQ(0, std::get<int>(*unknownOption.dimensions[0].value));
}

TEST(KnobYaml, StringAnimationThatIsNotTheImpliedCurveKeepsBoth)
{
    KnobSerialization text = makeKnob(kKnobTypes[4], eKnobVariantAnimated);
    text.dimensions[0].curve->keyframes[1].value = 5.;

    const std::string yaml = Serialization::encodeKnobsYaml({ text });
    EXPECT_NE(yaml.find("strings: [1, \"first\", 12,"), std::string::npos) << yaml;
    KnobSerialization decoded = decodeOne(yaml);
    Serialization::completeKnobSerialization(&decoded, "String", 1);
    expectKnobsEqual(YamlTest::canonicalKnob(text), decoded);
}

TEST(KnobYaml, StringAnimationWithEndKeyDerivativesIsStillTheImpliedCurve)
{
    KnobSerialization text = makeKnob(kKnobTypes[4], eKnobVariantAnimated);
    text.name = "filename";
    text.dimensions[0].curve->keyframes[0].leftDerivative = 0.;
    text.dimensions[0].curve->keyframes[0].rightDerivative = 1.5;
    text.dimensions[0].curve->keyframes[1].leftDerivative = 1.5;
    text.dimensions[0].curve->keyframes[1].rightDerivative = 0.;

    const std::string yaml = Serialization::encodeKnobsYaml({ text });
    EXPECT_EQ("filename: {curve: [1, \"first\", 12, \"second \\\"quoted\\\"\"]}\n", yaml);
    KnobSerialization decoded = decodeOne(yaml);
    Serialization::completeKnobSerialization(&decoded, "String", 1);
    expectKnobsEqual(YamlTest::canonicalKnob(text), decoded);
    EXPECT_EQ(yaml, Serialization::encodeKnobsYaml({ decoded }));
}

TEST(KnobYaml, LinkFormsRoundTrip)
{
    KnobSerialization knob = makeKnob(kKnobTypes[2], eKnobVariantStatic);
    knob.masterIsAlias = true;
    Serialization::KnobMasterSerialization alias;
    alias.masterNodeName = "Blur1";
    alias.masterNodeNameFull = "Blur1";
    alias.masterKnobName = "size";
    knob.dimensions[0].master = alias;
    Serialization::KnobMasterSerialization track;
    track.masterDimension = 1;
    track.masterNodeName = "Tracker1";
    track.masterNodeNameFull = "Group1.Tracker1";
    track.masterTrackName = "track3";
    track.masterKnobName = "center";
    knob.dimensions[1].master = track;

    KnobSerialization unsaved = makeKnob(kKnobTypes[0], eKnobVariantStatic);
    unsaved.dimensions[0].master = Serialization::KnobMasterSerialization();

    const std::string text = Serialization::encodeKnobsYaml({ knob, unsaved });
    EXPECT_EQ("knobDoubleStatic: {value: [{link: \"Blur1.size\"}, {link: \"Group1.Tracker1.center.1\", track: \"track3\"}], alias: true}\n"
              "knobIntStatic: {link: \"\"}\n",
              text);
    std::list<KnobSerialization> decoded = Serialization::decodeKnobsYaml(text);
    ASSERT_EQ(2u, decoded.size());
    Serialization::completeKnobSerialization(&decoded.front(), "Double", 2);
    Serialization::completeKnobSerialization(&decoded.back(), "Int", 1);
    expectKnobsEqual(YamlTest::canonicalKnob(knob), decoded.front());
    expectKnobsEqual(YamlTest::canonicalKnob(unsaved), decoded.back());
}

TEST(KnobYaml, LinkGroupsRoundTrip)
{
    KnobSerialization knob = makeKnob(kKnobTypes[2], eKnobVariantStatic);
    Serialization::KnobLinkGroupSerialization nodeLink;
    nodeLink.id = "Blur1.size.0";
    nodeLink.nodeLink = true;
    knob.dimensions[0].linkGroup = nodeLink;
    Serialization::KnobLinkGroupSerialization unlinked;
    unlinked.id = "Group1.Blur2.size.1";
    unlinked.linked = false;
    knob.dimensions[1].linkGroup = unlinked;

    const std::string text = Serialization::encodeKnobsYaml({ knob });
    EXPECT_NE(text.find("linkGroup: \"Blur1.size.0\", nodeLink: true"), std::string::npos) << text;
    EXPECT_NE(text.find("linkGroup: \"Group1.Blur2.size.1\", unlinked: true"), std::string::npos) << text;
    std::list<KnobSerialization> decoded = Serialization::decodeKnobsYaml(text);
    ASSERT_EQ(1u, decoded.size());
    Serialization::completeKnobSerialization(&decoded.front(), "Double", 2);
    expectKnobsEqual(YamlTest::canonicalKnob(knob), decoded.front());
    // A linked dimension keeps its own value in the file.
    EXPECT_TRUE(decoded.front().dimensions[0].value.has_value());
    EXPECT_EQ(text, Serialization::encodeKnobsYaml(decoded));
}

TEST(KnobYaml, SpecialDoublesAndAwkwardStrings)
{
    KnobSerialization doubles = makeKnob(kKnobTypes[6], eKnobVariantStatic);
    const double values[] = { -0., std::numeric_limits<double>::denorm_min(), 1e300, -std::numeric_limits<double>::infinity() };
    for (int i = 0; i < 4; ++i) {
        doubles.dimensions[i].value = KnobValueSerialization(std::in_place_type<double>, values[i]);
    }
    KnobSerialization strings = makeKnob(kKnobTypes[4], eKnobVariantStatic);
    strings.name = "true";
    strings.dimensions[0].value = KnobValueSerialization(std::in_place_type<std::string>, std::string("3"));
    KnobSerialization empty = makeKnob(kKnobTypes[7], eKnobVariantStatic);
    empty.name = "12";
    empty.dimensions[0].value = KnobValueSerialization(std::in_place_type<std::string>, std::string());

    const std::string text = Serialization::encodeKnobsYaml({ doubles, strings, empty });
    EXPECT_EQ("knobColorStatic: [-0, 5e-324, 1e+300, -.inf]\n"
              "\"true\": \"3\"\n"
              "\"12\": \"\"\n",
              text);
    std::list<KnobSerialization> decoded = Serialization::decodeKnobsYaml(text);
    ASSERT_EQ(3u, decoded.size());
    std::list<KnobSerialization>::iterator it = decoded.begin();
    Serialization::completeKnobSerialization(&(*it), "Color", 4);
    expectKnobsEqual(YamlTest::canonicalKnob(doubles), *it);
    ++it;
    Serialization::completeKnobSerialization(&(*it), "String", 1);
    expectKnobsEqual(YamlTest::canonicalKnob(strings), *it);
    ++it;
    Serialization::completeKnobSerialization(&(*it), "Path", 1);
    expectKnobsEqual(YamlTest::canonicalKnob(empty), *it);
}

TEST(KnobYaml, UnknownKeysAreIgnoredWithWarnings)
{
    YamlWarnings warnings;
    std::list<KnobSerialization> knobs = Serialization::decodeKnobsYaml("size: {value: 3, bogus: 1}\n"
                                                                        "mix: {value: 0.5, secret: true, future: [1, 2]}\n",
                                                                        &warnings);
    ASSERT_EQ(2u, knobs.size());
    EXPECT_EQ(3, std::get<int>(*knobs.front().dimensions[0].value));
    EXPECT_TRUE(knobs.back().secret);
    ASSERT_EQ(2u, warnings.size());
    EXPECT_EQ("knobs.size: unknown key \"bogus\" ignored", warnings[0]);
    EXPECT_EQ("knobs.mix: unknown key \"future\" ignored", warnings[1]);
}

TEST(KnobYaml, MalformedKnobsThrowDescriptiveErrors)
{
    expectDecodeError("size: [1, [2]]\n", "knobs.size[1]: expected a value, ~ or a map");
    expectDecodeError("size: {link: \"nolink\"}\n", "knobs.size.link: a link is \"<node>.<knob>[.<dimension>]\"");
    expectDecodeError("size: {curve: [L, 1]}\n", "knobs.size.curve: the curve ends where it expects a keyframe value");
    expectDecodeError("size: 1\nsize: 2\n", "duplicate key \"size\"");
    expectDecodeError("[1, 2]\n", "knobs: expected a map");
    expectDecodeError("size: {value: 1, user: {type: \"Int\"}}\n", "knobs.size.user.dimension: expected an integer");
    expectDecodeError("size: {value: 1, enabled: maybe}\n", "knobs.size.enabled: expected true or false, found \"maybe\"");
}

TEST(KnobYaml, NoKnobs)
{
    EXPECT_EQ("{}\n", Serialization::encodeKnobsYaml({}));
    EXPECT_TRUE(Serialization::decodeKnobsYaml("{}").empty());
}
