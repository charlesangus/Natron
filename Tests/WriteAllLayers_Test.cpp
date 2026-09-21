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
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/WriteNode.h"

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
expectColorPixels(const FlatExrImage& image)
{
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "R"), 1e-4f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "G"), 1e-4f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "B"), 1e-4f);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "A"), 1e-4f);
}

void
expectDiffusePixels(const FlatExrImage& image)
{
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "diffuse.R"), 1e-4f);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "diffuse.G"), 1e-4f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "diffuse.B"), 1e-4f);
}

void
expectRgbaOnly(const FlatExrImage& image)
{
    static const std::set<std::string> expected = { "R", "G", "B", "A" };

    EXPECT_EQ(expected, channelSet(image));
    expectColorPixels(image);
}

void
expectColorAndDiffusePixels(const FlatExrImage& image)
{
    static const std::set<std::string> expected = { "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B" };

    EXPECT_EQ(expected, channelSet(image));
    expectColorPixels(image);
    expectDiffusePixels(image);
}

void
expectAllLayerPixels(const FlatExrImage& image)
{
    static const std::set<std::string> expected = {
        "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B"
    };

    EXPECT_EQ(expected, channelSet(image));
    expectColorPixels(image);
    expectDiffusePixels(image);

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

// A Write container over WriteOIIO fed by the three-layer fixture, set up to write a
// single-part, uncompressed 32-bit float EXR. The container's channel set is the only knob a
// test changes between renders, so nothing else can bump the container's knobsAge in between.
class WriteAllLayersTest
    : public BaseTest {
protected:
    void createFixtureWriter()
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
        NodePtr reader = getApp()->createNode(readerArgs);
        ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

        _writer = createNode(_writeOIIOPluginID);
        ASSERT_TRUE(bool(_writer));

        connectNodes(reader, _writer, 0, true);

        KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(_writer->getKnobByName("partSplitting").get());
        ASSERT_TRUE(partSplitting != NULL);
        partSplitting->setValueFromID("single", 0);

        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(_writer->getKnobByName("bitDepth").get());
        ASSERT_TRUE(bitDepth != NULL);
        bitDepth->setValueFromID("32f", 0);

        KnobChoice* compression = dynamic_cast<KnobChoice*>(_writer->getKnobByName("compression").get());
        ASSERT_TRUE(compression != NULL);
        compression->setValueFromID("none", 0);

        _channels = dynamic_cast<KnobChannelSet*>(_writer->getKnobByName(kNodeParamChannelSet).get());
        ASSERT_TRUE(_channels != NULL) << "the Write container has no channel set knob";
    }

    NodePtr getEmbeddedEncoder() const
    {
        WriteNode* container = dynamic_cast<WriteNode*>(_writer->getEffectInstance().get());

        return container ? container->getEmbeddedWriter() : NodePtr();
    }

    NodePtr _writer;
    KnobChannelSet* _channels = NULL;
};

// Switching the container's channel set after a first render must reach the embedded encoder's
// hash, so that its cached components-needed plane set is invalidated and the next render
// fetches (and writes) the newly selected layers, not the plane set it cached the first time.
TEST_F(WriteAllLayersTest, WriteAllLayersToggleAfterRenderWritesEveryLayer)
{
    createFixtureWriter();
    if (HasFatalFailure()) {
        return;
    }

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const std::string path = (tmp.path() + QLatin1String("/out.exr")).toStdString();
    _writer->setOutputFilesForWriter(path);

    AppInstancePtr app = getApp();

    {
        QFile::remove(QString::fromStdString(path));

        FlatExrImage image;
        std::string error;
        ASSERT_TRUE(renderAndRead(app, _writer, path, &image, &error)) << error;
        expectRgbaOnly(image);
    }

    {
        _channels->setAll();
        QFile::remove(QString::fromStdString(path));

        FlatExrImage image;
        std::string error;
        ASSERT_TRUE(renderAndRead(app, _writer, path, &image, &error)) << error;
        expectAllLayerPixels(image);
    }

    {
        _channels->setLayer(0, kNatronColorLayerID, NULL);
        QFile::remove(QString::fromStdString(path));

        FlatExrImage image;
        std::string error;
        ASSERT_TRUE(renderAndRead(app, _writer, path, &image, &error)) << error;
        expectRgbaOnly(image);
    }

    QFile::remove(QString::fromStdString(path));
} // TEST_F(WriteAllLayersTest, WriteAllLayersToggleAfterRenderWritesEveryLayer)

// The same graph as above, but every layer is selected before the very first render, so the
// embedded encoder's components-needed plane set is correct from the start and never needs to
// be invalidated by a later knob change.
TEST_F(WriteAllLayersTest, WriteAllLayersCheckedBeforeFirstRenderWritesEveryLayer)
{
    createFixtureWriter();
    if (HasFatalFailure()) {
        return;
    }

    _channels->setAll();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QLatin1String("/checked_first.exr")).toStdString();
    _writer->setOutputFilesForWriter(path);

    AppInstancePtr app = getApp();
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAndRead(app, _writer, path, &image, &error)) << error;
    expectAllLayerPixels(image);

    QFile::remove(QString::fromStdString(path));
} // TEST_F(WriteAllLayersTest, WriteAllLayersCheckedBeforeFirstRenderWritesEveryLayer)

// A partial selection (Color plus one of the two extra layers) reaches the file as exactly those
// planes: the encoder enumerates "all planes present" on its input, and the host narrows that
// list to the container's channel set. The encoder's own plane selection is forced to "all" and
// hidden, since the host's answer is the selection.
TEST_F(WriteAllLayersTest, WriteColorAndOneLayerWritesOnlyThose)
{
    createFixtureWriter();
    if (HasFatalFailure()) {
        return;
    }

    NodePtr encoder = getEmbeddedEncoder();
    ASSERT_TRUE(bool(encoder));
    KnobBool* processAllLayers = dynamic_cast<KnobBool*>(encoder->getKnobByName("processAllLayers").get());
    ASSERT_TRUE(processAllLayers != NULL);
    EXPECT_TRUE(processAllLayers->getValue());
    EXPECT_TRUE(processAllLayers->getIsSecret());
    EXPECT_FALSE(processAllLayers->getIsPersistent());
    KnobIPtr outputChannels = encoder->getKnobByName("outputChannels");
    ASSERT_TRUE(bool(outputChannels));
    EXPECT_TRUE(outputChannels->getIsSecret());

    _channels->setLayer(0, kNatronColorLayerID, NULL);
    _channels->addLayer("diffuse", NULL);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QLatin1String("/color_and_diffuse.exr")).toStdString();
    _writer->setOutputFilesForWriter(path);

    AppInstancePtr app = getApp();
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAndRead(app, _writer, path, &image, &error)) << error;
    expectColorAndDiffusePixels(image);

    QFile::remove(QString::fromStdString(path));
} // TEST_F(WriteAllLayersTest, WriteColorAndOneLayerWritesOnlyThose)

// The encoder's colour path has fixed component counts, so a Color row that enables only R, G
// and B maps to the RGB superset through the encoder's (hidden) outputComponents choice: the
// file carries R, G and B and no alpha channel.
TEST_F(WriteAllLayersTest, WriteColorRgbSubsetWritesRgbOnly)
{
    createFixtureWriter();
    if (HasFatalFailure()) {
        return;
    }

    NodePtr encoder = getEmbeddedEncoder();
    ASSERT_TRUE(bool(encoder));
    KnobChoice* outputComponents = dynamic_cast<KnobChoice*>(encoder->getKnobByName("outputComponents").get());
    ASSERT_TRUE(outputComponents != NULL);
    EXPECT_EQ("RGBA", outputComponents->getActiveEntry().id);

    const std::vector<std::string> rgb = { "R", "G", "B" };
    _channels->setLayer(0, kNatronColorLayerID, &rgb);
    EXPECT_EQ("RGB", outputComponents->getActiveEntry().id);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QLatin1String("/rgb_only.exr")).toStdString();
    _writer->setOutputFilesForWriter(path);

    AppInstancePtr app = getApp();
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAndRead(app, _writer, path, &image, &error)) << error;

    static const std::set<std::string> expected = { "R", "G", "B" };
    EXPECT_EQ(expected, channelSet(image));
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "R"), 1e-4f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "G"), 1e-4f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "B"), 1e-4f);

    QFile::remove(QString::fromStdString(path));
} // TEST_F(WriteAllLayersTest, WriteColorRgbSubsetWritesRgbOnly)

// A non-Color row with some channels unchecked reaches the file as exactly those channels: the
// host lists the plane to the encoder with just the enabled channels, and the encoder's fetch of
// that plane renders the whole layer and extracts them.
TEST_F(WriteAllLayersTest, WriteColorAndDiffuseGreenWritesThatChannelOnly)
{
    createFixtureWriter();
    if (HasFatalFailure()) {
        return;
    }

    _channels->setLayer(0, kNatronColorLayerID, NULL);
    const std::vector<std::string> green = { "G" };
    _channels->addLayer("diffuse", &green);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QLatin1String("/color_and_diffuse_g.exr")).toStdString();
    _writer->setOutputFilesForWriter(path);

    AppInstancePtr app = getApp();
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAndRead(app, _writer, path, &image, &error)) << error;

    static const std::set<std::string> expected = { "R", "G", "B", "A", "diffuse.G" };
    EXPECT_EQ(expected, channelSet(image));
    expectColorPixels(image);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "diffuse.G"), 1e-4f);

    QFile::remove(QString::fromStdString(path));
} // TEST_F(WriteAllLayersTest, WriteColorAndDiffuseGreenWritesThatChannelOnly)

// A two-channel subset of a three-channel layer, alongside Color: the two channels keep their
// own names and values, with nothing padded in for the missing one.
TEST_F(WriteAllLayersTest, WriteColorAndSpecularRedBlueWritesThoseChannelsOnly)
{
    createFixtureWriter();
    if (HasFatalFailure()) {
        return;
    }

    _channels->setLayer(0, kNatronColorLayerID, NULL);
    const std::vector<std::string> redBlue = { "R", "B" };
    _channels->addLayer("specular", &redBlue);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QLatin1String("/color_and_specular_rb.exr")).toStdString();
    _writer->setOutputFilesForWriter(path);

    AppInstancePtr app = getApp();
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAndRead(app, _writer, path, &image, &error)) << error;

    static const std::set<std::string> expected = { "R", "G", "B", "A", "specular.R", "specular.B" };
    EXPECT_EQ(expected, channelSet(image));
    expectColorPixels(image);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "specular.R"), 1e-4f);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "specular.B"), 1e-4f);

    QFile::remove(QString::fromStdString(path));
} // TEST_F(WriteAllLayersTest, WriteColorAndSpecularRedBlueWritesThoseChannelsOnly)

// The encoder's own R/G/B/A quad is adopted by the container: forced on, non-persistent and
// locked hidden, since both GenericWriter and the host's own channel-quad refresh would
// otherwise re-show the boxes matching the Color component count after every metadata pass.
// Seen hidden, GenericWriter packs nothing, so the Color subset test above keeps its pixels.
TEST_F(WriteAllLayersTest, EncoderChannelQuadIsAdoptedAndStaysHidden)
{
    createFixtureWriter();
    if (HasFatalFailure()) {
        return;
    }

    NodePtr encoder = getEmbeddedEncoder();
    ASSERT_TRUE(bool(encoder));
    static const char* const quad[4] = { "NatronOfxParamProcessR", "NatronOfxParamProcessG", "NatronOfxParamProcessB", "NatronOfxParamProcessA" };
    KnobBool* channels[4];
    for (int i = 0; i < 4; ++i) {
        channels[i] = dynamic_cast<KnobBool*>(encoder->getKnobByName(quad[i]).get());
        ASSERT_TRUE(channels[i] != NULL) << quad[i];
        EXPECT_TRUE(channels[i]->getValue()) << quad[i];
        EXPECT_TRUE(channels[i]->getIsSecret()) << quad[i];
        EXPECT_TRUE(channels[i]->isSecretLocked()) << quad[i];
        EXPECT_FALSE(channels[i]->getIsPersistent()) << quad[i];
    }

    const std::vector<std::string> rgb = { "R", "G", "B" };
    _channels->setLayer(0, kNatronColorLayerID, &rgb);
    encoder->getEffectInstance()->refreshMetadata_public(true);
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(channels[i]->getIsSecret()) << quad[i];
    }
} // TEST_F(WriteAllLayersTest, EncoderChannelQuadIsAdoptedAndStaysHidden)

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
