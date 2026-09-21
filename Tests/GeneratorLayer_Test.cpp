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
#include <string>
#include <vector>

#include <gtest/gtest.h>

CLANG_DIAG_OFF(deprecated)
#include <QFile>
#include <QString>
#include <QTemporaryDir>
CLANG_DIAG_ON(deprecated)

#include "BaseTest.h"
#include "FlatExrReader.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>
#include <ofxNatron.h>

NATRON_NAMESPACE_USING

namespace {

// Tests/fixtures/flat-three-layers.exr: 8x8, every pixel Color (1, 0, 0, 1), diffuse (0, 1, 0),
// specular (0, 0, 1).
const int kSize = 8;

const double kConstantR = 0.75;
const double kConstantG = 0.5;
const double kConstantB = 0.25;
const double kConstantA = 0.125;

// EXR rows run top-down and the data window sits at the top of the display window when the
// project format is taller than the image, so Natron's y == 0 is the data window's last row.
float
at(const FlatExrImage& image,
   int x,
   int natronY,
   const std::string& channel)
{
    return image.at(x, image.y1 + image.height - 1 - natronY, channel);
}

std::string
channelList(const FlatExrImage& image)
{
    std::string list;

    for (std::size_t i = 0; i < image.channels.size(); ++i) {
        list += (i ? " " : "") + image.channels[i];
    }

    return list;
}

void
expectChannelEverywhere(const FlatExrImage& image,
                        const std::string& channel,
                        float value)
{
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            EXPECT_EQ(value, at(image, x, y, channel)) << channel << " at (" << x << ", " << y << ")";
        }
    }
}

void
expectFixtureColor(const FlatExrImage& image)
{
    expectChannelEverywhere(image, "R", 1.f);
    expectChannelEverywhere(image, "G", 0.f);
    expectChannelEverywhere(image, "B", 0.f);
    expectChannelEverywhere(image, "A", 1.f);
}

void
expectFixtureDiffuse(const FlatExrImage& image)
{
    expectChannelEverywhere(image, "diffuse.R", 0.f);
    expectChannelEverywhere(image, "diffuse.G", 1.f);
    expectChannelEverywhere(image, "diffuse.B", 0.f);
}

void
expectFixtureSpecular(const FlatExrImage& image)
{
    expectChannelEverywhere(image, "specular.R", 0.f);
    expectChannelEverywhere(image, "specular.G", 0.f);
    expectChannelEverywhere(image, "specular.B", 1.f);
}

void
setColor(const NodePtr& node,
         const char* knobName,
         double r,
         double g,
         double b,
         double a)
{
    KnobColor* color = dynamic_cast<KnobColor*>(node->getKnobByName(knobName).get());

    ASSERT_TRUE(color) << knobName;
    color->setValue(r, ViewSpec::all(), 0);
    color->setValue(g, ViewSpec::all(), 1);
    color->setValue(b, ViewSpec::all(), 2);
    color->setValue(a, ViewSpec::all(), 3);
}

} // namespace

class GeneratorLayerTest
    : public BaseTest {
protected:
    NodePtr createFixtureReader()
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());

        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));

        return getApp()->createNode(readerArgs);
    }

    NodePtr createGenerator(const char* pluginID,
                            KnobLayerSelectPtr* layer)
    {
        NodePtr generator = createNode(QString::fromUtf8(pluginID));

        if (!generator) {
            return NodePtr();
        }
        *layer = std::dynamic_pointer_cast<KnobLayerSelect>(generator->getLayerKnob());

        return generator;
    }

    // Appends a WriteOIIO writing every layer of `node` as one 32-bit float part and renders
    // frame 1 into `tmp`, parsing the result into `image`.
    bool renderAllLayers(const NodePtr& node,
                         const QTemporaryDir& tmp,
                         FlatExrImage* image,
                         std::string* error)
    {
        NodePtr writer = createNode(_writeOIIOPluginID);
        if (!writer) {
            *error = "writer creation failed";

            return false;
        }
        connectNodes(node, writer, 0, true);

        KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(writer->getKnobByName("partSplitting").get());
        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
        KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
        KnobChannelSet* writerChannels = dynamic_cast<KnobChannelSet*>(writer->getKnobByName(kNodeParamChannelSet).get());
        if (!partSplitting || !bitDepth || !compression || !writerChannels) {
            *error = "writer knobs missing";

            return false;
        }
        partSplitting->setValueFromID("single", 0);
        bitDepth->setValueFromID("32f", 0);
        compression->setValueFromID("none", 0);
        writerChannels->setAll();

        const std::string path = (tmp.path() + QLatin1String("/out.exr")).toStdString();
        writer->setOutputFilesForWriter(path);
        QFile::remove(QString::fromStdString(path));

        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
        if (!writerEffect) {
            *error = "writer has no OutputEffectInstance";

            return false;
        }
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
        getApp()->startWritersRendering(false, works);

        if (!QFile::exists(QString::fromStdString(path))) {
            *error = "frame was not rendered: " + path;

            return false;
        }

        return readFlatExr(path, image, error);
    }
};

// Without a Source there is nothing to pass through: the output holds the target plane alone,
// and the one-channel plane takes the plug-in's first output channel.
TEST_F(GeneratorLayerTest, ConstantWithoutSourceWritesDepthOnly)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    KnobLayerSelectPtr layer;
    NodePtr constant = createGenerator("net.sf.openfx.ConstantPlugin", &layer);
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(layer));
    EXPECT_TRUE(constant->isTargetLayerKnob(layer));
    setColor(constant, "color", kConstantR, kConstantG, kConstantB, kConstantA);

    KnobChoice* extent = dynamic_cast<KnobChoice*>(constant->getKnobByName("extent").get());
    KnobDouble* size = dynamic_cast<KnobDouble*>(constant->getKnobByName("size").get());
    KnobDouble* bottomLeft = dynamic_cast<KnobDouble*>(constant->getKnobByName("bottomLeft").get());
    ASSERT_TRUE(extent && size && bottomLeft);
    extent->setValueFromID("size", 0);
    ASSERT_EQ(std::string("size"), extent->getActiveEntry().id);
    bottomLeft->setValue(0., ViewSpec::all(), 0);
    bottomLeft->setValue(0., ViewSpec::all(), 1);
    size->setValue(kSize, ViewSpec::all(), 0);
    size->setValue(kSize, ViewSpec::all(), 1);

    layer->setLayer("depth");

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(constant, tmp, &image, &error)) << error;

    ASSERT_EQ(std::vector<std::string>(1, "depth.Z"), image.channels) << channelList(image);
    EXPECT_EQ(kSize, image.width);
    EXPECT_EQ(kSize, image.height);
    expectChannelEverywhere(image, "depth.Z", (float)kConstantR);
    EXPECT_EQ(-1, image.channelIndex("R"));
    EXPECT_EQ(-1, image.channelIndex("A"));

    project->reset(false, true);
}

TEST_F(GeneratorLayerTest, ConstantOverSourceWritesDepthAndPassesTheRestThrough)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr reader = createFixtureReader();
    ASSERT_TRUE(bool(reader));
    KnobLayerSelectPtr layer;
    NodePtr constant = createGenerator("net.sf.openfx.ConstantPlugin", &layer);
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(layer));
    connectNodes(reader, constant, 0, true);
    setColor(constant, "color", kConstantR, kConstantG, kConstantB, kConstantA);

    layer->setLayer("depth");

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(constant, tmp, &image, &error)) << error;

    ASSERT_NE(-1, image.channelIndex("depth.Z")) << channelList(image);
    expectChannelEverywhere(image, "depth.Z", (float)kConstantR);
    expectFixtureColor(image);
    expectFixtureDiffuse(image);
    expectFixtureSpecular(image);

    project->reset(false, true);
}

// Ramp carries its own R/G/B/A quad: the host adopts it, forces it on and masks the plane
// itself, so only the selected channel of the target plane changes.
TEST_F(GeneratorLayerTest, RampOverSourceWritesOneChannelOfDiffuse)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr reader = createFixtureReader();
    ASSERT_TRUE(bool(reader));
    KnobLayerSelectPtr layer;
    NodePtr ramp = createGenerator("net.sf.openfx.Ramp", &layer);
    ASSERT_TRUE(bool(ramp));
    ASSERT_TRUE(bool(layer));
    EXPECT_TRUE(ramp->isTargetLayerKnob(layer));
    connectNodes(reader, ramp, 0, true);

    static const char* const quad[4] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
    for (int i = 0; i < 4; ++i) {
        KnobBoolPtr process = std::dynamic_pointer_cast<KnobBool>(ramp->getKnobByName(quad[i]));
        ASSERT_TRUE(bool(process)) << quad[i];
        EXPECT_TRUE(process->getIsSecret()) << quad[i];
        EXPECT_TRUE(process->getValue()) << quad[i];
        EXPECT_FALSE(process->getIsPersistent()) << quad[i];
    }

    // Equal end colors make the ramp flat, whatever its geometry.
    setColor(ramp, "color0", kConstantR, kConstantG, kConstantB, kConstantA);
    setColor(ramp, "color1", kConstantR, kConstantG, kConstantB, kConstantA);

    layer->setLayer("diffuse");
    layer->setChannels(std::vector<std::string>(1, "R"));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(ramp, tmp, &image, &error)) << error;

    expectChannelEverywhere(image, "diffuse.R", (float)kConstantR);
    expectChannelEverywhere(image, "diffuse.G", 1.f);
    expectChannelEverywhere(image, "diffuse.B", 0.f);
    expectFixtureColor(image);
    expectFixtureSpecular(image);

    project->reset(false, true);
}

// Constant has no quad of its own: the host's buttons alone decide which channels it writes.
TEST_F(GeneratorLayerTest, ConstantOverSourceWritesTwoChannelsOfColor)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr reader = createFixtureReader();
    ASSERT_TRUE(bool(reader));
    KnobLayerSelectPtr layer;
    NodePtr constant = createGenerator("net.sf.openfx.ConstantPlugin", &layer);
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(layer));
    EXPECT_FALSE(bool(constant->getKnobByName(kNatronOfxParamProcessR)));
    connectNodes(reader, constant, 0, true);
    setColor(constant, "color", kConstantR, kConstantG, kConstantB, kConstantA);

    EXPECT_EQ(std::string(kNatronColorLayerID), layer->getLayer());
    std::vector<std::string> rg;
    rg.push_back("R");
    rg.push_back("G");
    layer->setChannels(rg);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(constant, tmp, &image, &error)) << error;

    expectChannelEverywhere(image, "R", (float)kConstantR);
    expectChannelEverywhere(image, "G", (float)kConstantG);
    expectChannelEverywhere(image, "B", 0.f);
    expectChannelEverywhere(image, "A", 1.f);
    expectFixtureDiffuse(image);
    expectFixtureSpecular(image);

    project->reset(false, true);
}
