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
#include <list>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "Serialization/NodeSerialization.h"
#include "Serialization/RotoSerialization.h"
#include "Serialization/Yaml/PresetYaml.h"
#include "Serialization/Yaml/YamlScalars.h"
#include "YamlTestHelpers.h"

NATRON_NAMESPACE_USING
using Serialization::KeyFrameSerialization;
using Serialization::NodeSerialization;
using Serialization::NodeSerializationPtr;
using Serialization::RotoLayerChildSerialization;
using Serialization::RotoStrokeCurvesSerialization;
using Serialization::YamlError;
using Serialization::YamlWarnings;

namespace {

const int kHeavyStrokeSamples = 5000;

NodeSerializationPtr
makeNode(const std::string& scriptName,
         const std::string& pluginID)
{
    NodeSerializationPtr node = std::make_shared<NodeSerialization>();
    node->scriptName = scriptName;
    node->label = scriptName;
    node->pluginID = pluginID;
    node->pluginMajorVersion = 1;
    node->pluginMinorVersion = 0;

    return node;
}

KeyFrameSerialization
key(double time,
    double value)
{
    KeyFrameSerialization result;
    result.time = time;
    result.value = value;
    result.interpolation = 1;

    return result;
}

// Values with full mantissas, so a lossy text round trip of the samples would show.
NodeSerializationPtr
rotoPaintWithHeavyStroke()
{
    NodeSerializationPtr node = makeNode("RotoPaint1", "RotoPaint");
    Serialization::KnobSerialization opacity;
    opacity.name = "opacity";
    opacity.typeName = "Double";
    opacity.dimension = 1;
    opacity.dimensions.resize(1);
    opacity.dimensions[0].value = Serialization::KnobValueSerialization(std::in_place_type<double>, 0.75);
    node->knobs.push_back(opacity);

    node->hasRotoContext = true;
    Serialization::RotoContextSerialization& roto = node->rotoContext;
    roto.baseLayer.name = "Layer1";
    roto.baseLayer.label = "Layer1";
    roto.baseLayer.activated = true;

    RotoLayerChildSerialization child;
    child.type = Serialization::eRotoLayerChildTypeStroke;
    child.stroke.name = "Stroke1";
    child.stroke.label = "Stroke1";
    child.stroke.activated = true;
    child.stroke.parentLayerName = "Layer1";
    child.stroke.brushType = 3;
    RotoStrokeCurvesSerialization curves;
    for (int i = 0; i < kHeavyStrokeSamples; ++i) {
        const double time = 1. + i / 7.;
        curves.xCurve.keyframes.push_back(key(time, 960. + 400. * std::sin(i / 13.)));
        curves.yCurve.keyframes.push_back(key(time, 540. + 300. * std::cos(i / 17.)));
        curves.pressureCurve.keyframes.push_back(key(time, 0.5 + 0.5 * std::sin(i / 3.)));
    }
    child.stroke.strokes.push_back(curves);
    roto.baseLayer.children.push_back(child);

    return node;
}

void
expectSameKeys(const Serialization::CurveSerialization& expected,
               const Serialization::CurveSerialization& actual)
{
    ASSERT_EQ(expected.keyframes.size(), actual.keyframes.size());
    auto a = actual.keyframes.begin();
    for (const KeyFrameSerialization& e : expected.keyframes) {
        EXPECT_SAME_DOUBLE(e.time, a->time);
        EXPECT_SAME_DOUBLE(e.value, a->value);
        EXPECT_EQ(e.interpolation, a->interpolation);
        ++a;
    }
}

} // anonymous namespace

TEST(PresetYaml, RoundTripsARotoPaintWithAHeavyStrokeInOneSelfContainedDocument)
{
    const NodeSerializationPtr node = rotoPaintWithHeavyStroke();
    const std::string text = Serialization::encodePresetYaml({ node });

    EXPECT_EQ(0u, text.find("format: natron-preset\nformatVersion: 1\n")) << text.substr(0, 200);
    EXPECT_NE(std::string::npos, text.find("!!binary")) << "the stroke samples are not inlined";
    EXPECT_EQ(std::string::npos, text.find("blob:")) << "the preset refers to a blob member it does not carry";

    YamlWarnings warnings;
    const std::list<NodeSerializationPtr> decoded = Serialization::decodePresetYaml(text, &warnings);
    EXPECT_TRUE(warnings.empty()) << (warnings.empty() ? std::string() : warnings.front());
    ASSERT_EQ(1u, decoded.size());
    const NodeSerialization& result = *decoded.front();
    EXPECT_EQ("RotoPaint1", result.scriptName);
    EXPECT_EQ("RotoPaint", result.pluginID);
    ASSERT_TRUE(result.hasRotoContext);
    ASSERT_EQ(1u, result.rotoContext.baseLayer.children.size());
    const Serialization::RotoStrokeItemSerialization& stroke = result.rotoContext.baseLayer.children.front().stroke;
    EXPECT_EQ("Stroke1", stroke.name);
    EXPECT_EQ(3, stroke.brushType);
    ASSERT_EQ(1u, stroke.strokes.size());
    const RotoStrokeCurvesSerialization& expected = node->rotoContext.baseLayer.children.front().stroke.strokes.front();
    expectSameKeys(expected.xCurve, stroke.strokes.front().xCurve);
    expectSameKeys(expected.yCurve, stroke.strokes.front().yCurve);
    expectSameKeys(expected.pressureCurve, stroke.strokes.front().pressureCurve);

    EXPECT_EQ(text, Serialization::encodePresetYaml(decoded));
}

TEST(PresetYaml, KeepsTheOrderOfAMultiInstanceNodeAndItsInstances)
{
    const NodeSerializationPtr tracker = makeNode("Tracker1", "Tracker");
    const NodeSerializationPtr second = makeNode("Tracker1_b", "fr.inria.built-in.TrackerPM");
    const NodeSerializationPtr first = makeNode("Tracker1_a", "fr.inria.built-in.TrackerPM");
    const std::string text = Serialization::encodePresetYaml({ tracker, second, first });

    const std::list<NodeSerializationPtr> decoded = Serialization::decodePresetYaml(text);
    ASSERT_EQ(3u, decoded.size());
    std::list<NodeSerializationPtr>::const_iterator it = decoded.begin();
    EXPECT_EQ("Tracker1", (*it++)->scriptName);
    EXPECT_EQ("Tracker1_b", (*it++)->scriptName);
    EXPECT_EQ("Tracker1_a", (*it++)->scriptName);
}

TEST(PresetYaml, AnOldXmlPresetIsRecognisedAndRefusedWithoutCrashing)
{
    const std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n"
                            "<!DOCTYPE boost_serialization>\n"
                            "<boost_serialization signature=\"serialization::archive\" version=\"19\">\n"
                            "<NodesCount>1</NodesCount>\n";
    EXPECT_TRUE(Serialization::isXmlDocument(xml));
    EXPECT_TRUE(Serialization::isXmlDocument("\xEF\xBB\xBF  \n" + xml));
    EXPECT_FALSE(Serialization::isXmlDocument(Serialization::encodePresetYaml({ makeNode("Blur1", "Blur") })));
    EXPECT_THROW(Serialization::decodePresetYaml(xml), YamlError);
}

TEST(PresetYaml, OtherDocumentsAreRefused)
{
    EXPECT_THROW(Serialization::decodePresetYaml(""), YamlError);
    EXPECT_THROW(Serialization::decodePresetYaml("format: natron-clipboard\nformatVersion: 1\nnodes: {}\n"), YamlError);
    EXPECT_THROW(Serialization::decodePresetYaml("format: natron-preset\nformatVersion: 1\nnodes: []\n"), YamlError);
    EXPECT_THROW(Serialization::decodePresetYaml("format: natron-preset\nformatVersion: 2\nnodes: []\n"), Serialization::YamlNewerFormatError);
}
