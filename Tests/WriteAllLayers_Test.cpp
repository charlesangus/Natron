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
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <SequenceParsing.h>

#include "BaseTest.h"
#include "FlatExrReader.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

// The fixture (Tests/fixtures/flat-three-layers.exr) is an 8x8 half EXR with three flat
// (spatially constant) layers, so any in-bounds pixel carries the same values and there is
// nothing special about the one checked here.
const int32_t kCheckX = 1;
const int32_t kCheckY = 1;

std::set<std::string>
channelSet(const FlatExrImage& image)
{
    return std::set<std::string>(image.channels.begin(), image.channels.end());
}

std::set<std::string>
layerIDSet(const std::list<ImageLayerDesc>& layers)
{
    std::set<std::string> ret;
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        ret.insert(it->getLayerID());
    }
    return ret;
}

void
expectRgbaOnly(const FlatExrImage& image)
{
    static const std::set<std::string> expected = { "R", "G", "B", "A" };

    EXPECT_EQ(expected, channelSet(image));
}

void
expectAllLayerPixels(const FlatExrImage& image)
{
    static const std::set<std::string> expected = {
        "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B"
    };

    EXPECT_EQ(expected, channelSet(image));

    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "R"), 1e-4f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "G"), 1e-4f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "B"), 1e-4f);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "A"), 1e-4f);

    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "diffuse.R"), 1e-4f);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "diffuse.G"), 1e-4f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "diffuse.B"), 1e-4f);

    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "specular.R"), 1e-4f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "specular.G"), 1e-4f);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "specular.B"), 1e-4f);
}

// Renders frame 1 of `writer` to `path` and parses the result. `path` must already be set as
// the writer's sole output file (a fixed, unpadded filename, not a `####` sequence pattern),
// since every render in these tests is a single frame written to its own file.
bool
renderAndRead(const AppInstancePtr& app,
              const NodePtr& writer,
              const std::string& path,
              FlatExrImage* out,
              std::string* error)
{
    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    if (!writerEffect) {
        *error = "writer has no OutputEffectInstance";

        return false;
    }

    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
    app->startWritersRendering(false, works);

    if (!QFile::exists(QString::fromStdString(path))) {
        *error = "frame was not rendered: " + path;

        return false;
    }

    return readFlatExr(path, out, error);
}

} // namespace

// A Write node is a WriteNode container wrapping an embedded encoder (here, WriteOIIO). Toggling
// "All Layers" on the container after a first render must reach the embedded encoder's hash, so
// that its cached components-needed plane set is invalidated and the second render fetches (and
// writes) every layer, not just the color plane it cached the first time around.
TEST_F(BaseTest, WriteAllLayersToggleAfterRenderWritesEveryLayer)
{
    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(writer));

    connectNodes(reader, writer, 0, true);

    KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(writer->getKnobByName("partSplitting").get());
    ASSERT_TRUE(partSplitting != NULL);
    partSplitting->setValueFromID("single", 0);

    KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
    ASSERT_TRUE(bitDepth != NULL);
    bitDepth->setValueFromID("32f", 0);

    KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
    ASSERT_TRUE(compression != NULL);
    compression->setValueFromID("none", 0);

    KnobBool* processAllLayers = dynamic_cast<KnobBool*>(writer->getKnobByName(kNodeParamProcessAllLayers).get());
    ASSERT_TRUE(processAllLayers != NULL);
    ASSERT_FALSE(processAllLayers->getValue()) << "All Layers must start unchecked for this test to exercise the toggle";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // A single fixed output path for all three renders: the only knob that changes between them
    // is processAllLayers, so the container's knobsAge cannot be bumped by anything else (such as
    // a changed output filename) between checks.
    const std::string path = (tmp.path() + QLatin1String("/out.exr")).toStdString();
    writer->setOutputFilesForWriter(path);

    AppInstancePtr app = getApp();

    {
        QFile::remove(QString::fromStdString(path));

        FlatExrImage image;
        std::string error;
        ASSERT_TRUE(renderAndRead(app, writer, path, &image, &error)) << error;
        expectRgbaOnly(image);
    }

    {
        processAllLayers->setValue(true);
        QFile::remove(QString::fromStdString(path));

        FlatExrImage image;
        std::string error;
        ASSERT_TRUE(renderAndRead(app, writer, path, &image, &error)) << error;
        expectAllLayerPixels(image);
    }

    {
        processAllLayers->setValue(false);
        QFile::remove(QString::fromStdString(path));

        FlatExrImage image;
        std::string error;
        ASSERT_TRUE(renderAndRead(app, writer, path, &image, &error)) << error;
        expectRgbaOnly(image);
    }

    QFile::remove(QString::fromStdString(path));
} // TEST_F(BaseTest, WriteAllLayersToggleAfterRenderWritesEveryLayer)

// The same graph as above, but "All Layers" is checked before the very first render, so the
// embedded encoder's components-needed plane set is correct from the start and never needs to
// be invalidated by a later knob change.
TEST_F(BaseTest, WriteAllLayersCheckedBeforeFirstRenderWritesEveryLayer)
{
    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(writer));

    connectNodes(reader, writer, 0, true);

    KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(writer->getKnobByName("partSplitting").get());
    ASSERT_TRUE(partSplitting != NULL);
    partSplitting->setValueFromID("single", 0);

    KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
    ASSERT_TRUE(bitDepth != NULL);
    bitDepth->setValueFromID("32f", 0);

    KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
    ASSERT_TRUE(compression != NULL);
    compression->setValueFromID("none", 0);

    KnobBool* processAllLayers = dynamic_cast<KnobBool*>(writer->getKnobByName(kNodeParamProcessAllLayers).get());
    ASSERT_TRUE(processAllLayers != NULL);
    processAllLayers->setValue(true);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QLatin1String("/checked_first.exr")).toStdString();
    writer->setOutputFilesForWriter(path);

    AppInstancePtr app = getApp();
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAndRead(app, writer, path, &image, &error)) << error;
    expectAllLayerPixels(image);

    QFile::remove(QString::fromStdString(path));
} // TEST_F(BaseTest, WriteAllLayersCheckedBeforeFirstRenderWritesEveryLayer)

// getPresentLayers() reports only what the stream actually carries (produced union pass-through);
// getAvailableLayers() adds every layer registered at the project level (built-ins included) on
// top of that, on the output (inputNb == -1) query only.
TEST_F(BaseTest, PresentLayersAreStreamOnlyAvailableLayersIncludeRegistry)
{
    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

    EffectInstancePtr readerEffect = reader->getEffectInstance();
    ASSERT_TRUE(bool(readerEffect));

    static const std::set<std::string> expectedPresent = { kNatronColorLayerID, "diffuse", "specular" };

    {
        std::list<ImageLayerDesc> present;
        readerEffect->getPresentLayers(0, ViewIdx(0), -1, &present);
        EXPECT_EQ(expectedPresent, layerIDSet(present));
    }

    {
        std::list<ImageLayerDesc> available;
        readerEffect->getAvailableLayers(0, ViewIdx(0), -1, &available);
        std::set<std::string> availableIDs = layerIDSet(available);
        EXPECT_EQ(std::size_t(1), availableIDs.count(ImageLayerDesc::getBackwardMotionComponents().getLayerID()));
        EXPECT_EQ(std::size_t(1), availableIDs.count(ImageLayerDesc::getForwardMotionComponents().getLayerID()));
        EXPECT_EQ(std::size_t(1), availableIDs.count(ImageLayerDesc::getDisparityLeftComponents().getLayerID()));
        EXPECT_EQ(std::size_t(1), availableIDs.count(ImageLayerDesc::getDisparityRightComponents().getLayerID()));
        EXPECT_EQ(std::size_t(1), availableIDs.count(std::string("depth")));
    }

    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
    ASSERT_TRUE(bool(blur));

    connectNodes(reader, blur, 0, true);

    EffectInstancePtr blurEffect = blur->getEffectInstance();
    ASSERT_TRUE(bool(blurEffect));

    {
        std::list<ImageLayerDesc> present;
        blurEffect->getPresentLayers(0, ViewIdx(0), -1, &present);
        EXPECT_EQ(expectedPresent, layerIDSet(present));
    }
    {
        std::list<ImageLayerDesc> present0;
        blurEffect->getPresentLayers(0, ViewIdx(0), 0, &present0);
        EXPECT_EQ(expectedPresent, layerIDSet(present0));
    }
} // TEST_F(BaseTest, PresentLayersAreStreamOnlyAvailableLayersIncludeRegistry)
