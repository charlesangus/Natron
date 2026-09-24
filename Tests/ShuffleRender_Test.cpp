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
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "FlatExrReader.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

// The fixture (Tests/fixtures/flat-three-layers.exr) is an 8x8 half EXR whose every pixel is
// Color (1, 0, 0, 1), diffuse (0, 1, 0) and specular (0, 0, 1), so one pixel stands for all.
const int kSize = 8;
const int32_t kCheckX = 1;
const int32_t kCheckY = 1;

std::set<std::string>
channelSet(const FlatExrImage& image)
{
    return std::set<std::string>(image.channels.begin(), image.channels.end());
}

// Relative to the data window, which sits wherever the project format puts it in the file.
float
valueAt(const FlatExrImage& image,
        const std::string& channel)
{
    return image.at(image.x1 + kCheckX, image.y1 + kCheckY, channel);
}

void
expectPlane(const FlatExrImage& image,
            const std::string& prefix,
            float r,
            float g,
            float b)
{
    EXPECT_NEAR(r, valueAt(image, prefix + "R"), 1e-4f) << prefix << "R";
    EXPECT_NEAR(g, valueAt(image, prefix + "G"), 1e-4f) << prefix << "G";
    EXPECT_NEAR(b, valueAt(image, prefix + "B"), 1e-4f) << prefix << "B";
}

void
expectColor(const FlatExrImage& image,
            float r,
            float g,
            float b,
            float a)
{
    expectPlane(image, std::string(), r, g, b);
    EXPECT_NEAR(a, valueAt(image, "A"), 1e-4f) << "A";
}

void
expectFixtureLayers(const FlatExrImage& image)
{
    static const std::set<std::string> expected = {
        "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B"
    };

    EXPECT_EQ(expected, channelSet(image));
    EXPECT_EQ(kSize, image.width);
    EXPECT_EQ(kSize, image.height);
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

// Read(flat-three-layers.exr) -> Shuffle or ShuffleCopy (on its main input) -> Write, the Write
// set up to write every layer into a single-part, uncompressed 32-bit float EXR.
class ShuffleRenderTest
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

    void createFixtureReader(NodePtr* reader,
                             const std::string& fixture = "flat-three-layers.exr")
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);
        *reader = getApp()->createNode(readerArgs);
        ASSERT_TRUE(bool(*reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();
    }

    void createShuffleOnFixture(const std::string& fixture = "flat-three-layers.exr",
                                const char* pluginID = PLUGINID_NATRON_SHUFFLE)
    {
        NodePtr reader;
        createFixtureReader(&reader, fixture);
        if (HasFatalFailure()) {
            return;
        }
        _reader = reader;
        _shuffle = createNode(QString::fromUtf8(pluginID));
        ASSERT_TRUE(bool(_shuffle)) << pluginID;
        connectNodes(reader, _shuffle, Shuffle::eInputMain, true);

        _mapping = std::dynamic_pointer_cast<KnobShuffleMap>(_shuffle->getKnobByName(kShuffleParamMapping));
        ASSERT_TRUE(bool(_mapping));

        createWriterOn(_shuffle);
    }

    // Switches the fixture reader's file in place and pushes the change downstream the same way
    // production code does (Gui's file-path widget triggers the same knobChanged path), so the
    // Shuffle node's channel selectors refresh and a stale persistent error can clear.
    void switchReaderFixture(const std::string& fixture)
    {
        KnobFilePtr fileKnob = std::dynamic_pointer_cast<KnobFile>(_reader->getKnobByName(kOfxImageEffectFileParamName));
        ASSERT_TRUE(bool(fileKnob));
        fileKnob->setValue(std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);
        _reader->forceRefreshAllInputRelatedData();
    }

    void createWriterOn(const NodePtr& input)
    {
        _writer = createNode(_writeOIIOPluginID);
        ASSERT_TRUE(bool(_writer));

        connectNodes(input, _writer, 0, true);

        KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(_writer->getKnobByName("partSplitting").get());
        ASSERT_TRUE(partSplitting != NULL);
        partSplitting->setValueFromID("single", 0);

        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(_writer->getKnobByName("bitDepth").get());
        ASSERT_TRUE(bitDepth != NULL);
        bitDepth->setValueFromID("32f", 0);

        KnobChoice* compression = dynamic_cast<KnobChoice*>(_writer->getKnobByName("compression").get());
        ASSERT_TRUE(compression != NULL);
        compression->setValueFromID("none", 0);

        KnobChannelSet* channels = dynamic_cast<KnobChannelSet*>(_writer->getKnobByName(kNodeParamChannelSet).get());
        ASSERT_TRUE(channels != NULL) << "the Write container has no channel set knob";
        channels->setAll();
    }

    // Sized like the fixture, so reading it does not grow the output's region of definition.
    void createConstant(double value,
                        NodePtr* constant)
    {
        *constant = createNode(QString::fromUtf8("net.sf.openfx.ConstantPlugin"));
        ASSERT_TRUE(bool(*constant));
        KnobColor* color = dynamic_cast<KnobColor*>((*constant)->getKnobByName("color").get());
        ASSERT_TRUE(color != NULL);
        color->setValues(value, value, value, value, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        KnobChoice* extent = dynamic_cast<KnobChoice*>((*constant)->getKnobByName("extent").get());
        KnobDouble* size = dynamic_cast<KnobDouble*>((*constant)->getKnobByName("size").get());
        KnobDouble* bottomLeft = dynamic_cast<KnobDouble*>((*constant)->getKnobByName("bottomLeft").get());
        ASSERT_TRUE(extent && size && bottomLeft);
        extent->setValueFromID("size", 0);
        bottomLeft->setValue(0., ViewSpec::all(), 0);
        bottomLeft->setValue(0., ViewSpec::all(), 1);
        size->setValue(kSize, ViewSpec::all(), 0);
        size->setValue(kSize, ViewSpec::all(), 1);
    }

    KnobLayerSelectPtr layerKnob(const char* name) const
    {
        return std::dynamic_pointer_cast<KnobLayerSelect>(_shuffle->getKnobByName(name));
    }

    void setLayer(const char* knobName,
                  const std::string& layerID)
    {
        KnobLayerSelectPtr knob = layerKnob(knobName);
        ASSERT_TRUE(bool(knob)) << knobName;
        knob->setLayer(layerID);
    }

    void wireStraight(int outSlot,
                      int inSlot,
                      int nChannels)
    {
        for (int c = 0; c < nChannels; ++c) {
            _mapping->setSource(outSlot, c, ShuffleSource::makeInput(inSlot, c));
        }
    }

    void render(const QTemporaryDir& tmp,
                const char* fileName,
                FlatExrImage* image)
    {
        const std::string path = (tmp.path() + QLatin1String("/") + QString::fromUtf8(fileName)).toStdString();
        _writer->setOutputFilesForWriter(path);
        QFile::remove(QString::fromStdString(path));

        std::string error;
        ASSERT_TRUE(renderAndRead(getApp(), _writer, path, image, &error)) << error;
        QFile::remove(QString::fromStdString(path));
    }

    NodePtr _reader;
    NodePtr _shuffle;
    NodePtr _writer;
    std::shared_ptr<KnobShuffleMap> _mapping;
};

// diffuse has no fourth channel, so Color's alpha reads 0 rather than keeping the input's 1.
TEST_F(ShuffleRenderTest, InputLayerIntoColorByDefaultZeroesTheAlphaItLacks)
{
    createShuffleOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    setLayer(kShuffleParamIn1, "diffuse");
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_TRUE(_mapping->getRows().empty());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    render(tmp, "diffuse_into_color.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    expectFixtureLayers(image);
    expectColor(image, 0.f, 1.f, 0.f, 0.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ShuffleRenderTest, TwoLayerSwapInOneNodeLeavesColorAlone)
{
    createShuffleOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    setLayer(kShuffleParamIn1, "diffuse");
    setLayer(kShuffleParamIn2, "specular");
    setLayer(kShuffleParamOut1, "diffuse");
    setLayer(kShuffleParamOut2, "specular");
    if (HasFatalFailure()) {
        return;
    }
    wireStraight(1, 2, 3);
    wireStraight(2, 1, 3);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    render(tmp, "swap.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    expectFixtureLayers(image);
    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 0.f, 1.f);
    expectPlane(image, "specular.", 0.f, 1.f, 0.f);
}

// specular's G is 0 like Color's, so its B carries the proof that in2 is read.
TEST_F(ShuffleRenderTest, OneOutputMixesChannelsOfBothSlots)
{
    createShuffleOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    setLayer(kShuffleParamIn2, "specular");
    if (HasFatalFailure()) {
        return;
    }
    _mapping->setSource(1, 0, ShuffleSource::makeInput(1, 0));
    _mapping->setSource(1, 1, ShuffleSource::makeInput(2, 1));
    _mapping->setSource(1, 2, ShuffleSource::makeInput(2, 2));
    EXPECT_FALSE(_mapping->hasExplicitSource(1, 0));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    render(tmp, "two_by_two.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    expectFixtureLayers(image);
    expectColor(image, 1.f, 0.f, 1.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

// R is already 1 in the fixture, so G carries the proof that a constant 1 is written.
TEST_F(ShuffleRenderTest, ConstantsOverwriteTheirChannelsAndTheRestReadTheirDefault)
{
    createShuffleOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _mapping->setSource(1, 3, ShuffleSource::makeZero());
    _mapping->setSource(1, 0, ShuffleSource::makeOne());
    _mapping->setSource(1, 1, ShuffleSource::makeOne());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    render(tmp, "constants.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    expectFixtureLayers(image);
    expectColor(image, 1.f, 1.f, 0.f, 0.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ShuffleRenderTest, ShuffleCopyTakesColorFromInput2AndAlphaFromInput1)
{
    createShuffleOnFixture("flat-three-layers.exr", PLUGINID_NATRON_SHUFFLECOPY);
    if (HasFatalFailure()) {
        return;
    }
    NodePtr constant;
    createConstant(0.5, &constant);
    if (HasFatalFailure()) {
        return;
    }
    connectNodes(constant, _shuffle, Shuffle::eInputCopy1, true);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    render(tmp, "copy_alpha.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    expectFixtureLayers(image);
    expectColor(image, 1.f, 0.f, 0.f, 0.5f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ShuffleRenderTest, ShuffleCopyWithInput1DisconnectedZeroesTheAlphaSilently)
{
    createShuffleOnFixture("flat-three-layers.exr", PLUGINID_NATRON_SHUFFLECOPY);
    if (HasFatalFailure()) {
        return;
    }
    ASSERT_FALSE(bool(_shuffle->getInput(Shuffle::eInputCopy1)));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    render(tmp, "copy_no_input1.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_FALSE(_shuffle->hasPersistentMessage());
    expectFixtureLayers(image);
    expectColor(image, 1.f, 0.f, 0.f, 0.f);
}

TEST_F(ShuffleRenderTest, SingleChannelOutputLayerIsCreatedAndColorPassesThrough)
{
    ProjectPtr project = getApp()->getProject();
    std::string error;
    const std::vector<std::string> alpha(1, "A");
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask", "mask", "", alpha), LayerRegistryEntry::eOriginUser, &error)) << error;

    createShuffleOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    setLayer(kShuffleParamOut1, "mask");
    if (HasFatalFailure()) {
        return;
    }

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    render(tmp, "mask.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    static const std::set<std::string> expected = {
        "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B", "mask.A"
    };
    EXPECT_EQ(expected, channelSet(image));
    // mask's only channel reads in1's first channel, Color's R, by default.
    EXPECT_NEAR(1.f, valueAt(image, "mask.A"), 1e-4f);
    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

// --- An explicit row whose source becomes unreadable upstream fails the render, naming it ----

TEST_F(ShuffleRenderTest, ExplicitRowAbsentFromConnectedInputFailsThenClearsOnFixtureSwitch)
{
    createShuffleOnFixture("flat-rgba-only.exr");
    if (HasFatalFailure()) {
        return;
    }
    setLayer(kShuffleParamIn1, "diffuse");
    if (HasFatalFailure()) {
        return;
    }
    _mapping->setSource(1, 0, ShuffleSource::makeInput(1, 1));
    ASSERT_TRUE(_mapping->hasExplicitSource(1, 0));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QLatin1String("/explicit_row_missing.exr")).toStdString();
    _writer->setOutputFilesForWriter(path);
    QFile::remove(QString::fromStdString(path));

    FlatExrImage image;
    std::string error;
    EXPECT_FALSE(renderAndRead(getApp(), _writer, path, &image, &error));
    ASSERT_TRUE(_shuffle->hasPersistentMessage());

    QString message;
    int type = 0;
    _shuffle->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("diffuse"))) << message.toStdString();

    // flat-three-layers.exr carries diffuse: the row becomes readable again, and the stale
    // error clears the same way a mask channel's does on reconnect.
    switchReaderFixture("flat-three-layers.exr");
    if (HasFatalFailure()) {
        return;
    }

    FlatExrImage image2;
    render(tmp, "explicit_row_recovered.exr", &image2);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_FALSE(_shuffle->hasPersistentMessage());
    expectFixtureLayers(image2);
    expectColor(image2, 1.f, 1.f, 0.f, 0.f);
    expectPlane(image2, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image2, "specular.", 0.f, 0.f, 1.f);
}

// --- Silent cases: an implicit default on a missing layer, and an explicit row on a None slot ---

TEST_F(ShuffleRenderTest, ImplicitDefaultOnAMissingLayerRendersZeroSilently)
{
    createShuffleOnFixture("flat-rgba-only.exr");
    if (HasFatalFailure()) {
        return;
    }
    setLayer(kShuffleParamIn1, "diffuse");
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_TRUE(_mapping->getRows().empty());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    render(tmp, "implicit_missing_layer.exr", &image);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_FALSE(_shuffle->hasPersistentMessage());
    expectColor(image, 0.f, 0.f, 0.f, 0.f);
}

TEST_F(ShuffleRenderTest, ExplicitRowOnANoneSlotRendersZeroSilently)
{
    createShuffleOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _mapping->setSource(1, 0, ShuffleSource::makeInput(2, 0));
    ASSERT_TRUE(_mapping->hasExplicitSource(1, 0));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    render(tmp, "none_slot_zero.exr", &image);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_FALSE(_shuffle->hasPersistentMessage());
    expectFixtureLayers(image);
    expectColor(image, 0.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}
