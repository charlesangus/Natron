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
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/AddLayers.h"
#include "Engine/Nodes/Channel/RemoveLayers.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

// flat-three-layers.exr is an 8x8 half EXR whose every pixel is Color (1, 0, 0, 1),
// diffuse (0, 1, 0) and specular (0, 0, 1), so one pixel stands for all.
const int32_t kCheckX = 1;
const int32_t kCheckY = 1;
const char* const kGradePluginID = "net.sf.openfx.GradePlugin";

std::set<std::string>
channelSet(const FlatExrImage& image)
{
    return std::set<std::string>(image.channels.begin(), image.channels.end());
}

std::set<std::string>
names(std::initializer_list<const char*> list)
{
    std::set<std::string> result;

    for (const char* name : list) {
        result.insert(name);
    }

    return result;
}

// Relative to the data window, which sits wherever the project format puts it in the file.
void
expectChannel(const FlatExrImage& image,
              const std::string& channel,
              float expected)
{
    EXPECT_NEAR(expected, image.at(image.x1 + kCheckX, image.y1 + kCheckY, channel), 1e-4f) << channel;
}

void
expectPlane(const FlatExrImage& image,
            const std::string& prefix,
            float r,
            float g,
            float b)
{
    expectChannel(image, prefix + "R", r);
    expectChannel(image, prefix + "G", g);
    expectChannel(image, prefix + "B", b);
}

} // namespace

// Read -> AddLayers -> Write, the Write set up to write every layer into a single-part,
// uncompressed 32-bit float EXR.
class AddLayersRenderTest
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

    void registerLayer(const std::string& id,
                       const std::vector<std::string>& channels)
    {
        std::string error;
        const LayerRegistry::AddResultEnum result = getApp()->getProject()->addLayer(ImageLayerDesc(id, id, "", channels), LayerRegistryEntry::eOriginUser, &error);
        EXPECT_TRUE((result == LayerRegistry::eAddResultAdded) || (result == LayerRegistry::eAddResultGrown)) << id << ": " << error;
    }

    void registerMask()
    {
        registerLayer("mask", std::vector<std::string>(1, "A"));
    }

    NodePtr createReader(const std::string& fixture)
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);
        NodePtr reader = getApp()->createNode(readerArgs);
        EXPECT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

        return reader;
    }

    // Frame 1 of flat-seq-layers.####.exr carries RGBA + diffuse + specular (the same values as
    // flat-three-layers.exr), frame 2 carries RGBA only.
    NodePtr createReadSequence()
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-seq-layers.####.exr"));
        NodePtr reader = getApp()->createNode(readerArgs);
        EXPECT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

        return reader;
    }

    void createAddOn(const NodePtr& source)
    {
        _add = createNode(QString::fromUtf8(PLUGINID_NATRON_ADDLAYERS));
        ASSERT_TRUE(bool(_add));
        connectNodes(source, _add, 0, true);

        _layers = std::dynamic_pointer_cast<KnobChannelSet>(_add->getKnobByName(kAddLayersParamLayers));
        ASSERT_TRUE(bool(_layers));
        _last = _add;
    }

    void createAddOnFixture(const std::string& fixture)
    {
        NodePtr reader = createReader(fixture);
        ASSERT_TRUE(bool(reader));
        createAddOn(reader);
    }

    void createWriter()
    {
        _writer = createNode(_writeOIIOPluginID);
        ASSERT_TRUE(bool(_writer));

        connectNodes(_last, _writer, 0, true);

        KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(_writer->getKnobByName("partSplitting").get());
        ASSERT_TRUE(partSplitting != NULL);
        partSplitting->setValueFromID("single", 0);

        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(_writer->getKnobByName("bitDepth").get());
        ASSERT_TRUE(bitDepth != NULL);
        bitDepth->setValueFromID("32f", 0);

        KnobChoice* compression = dynamic_cast<KnobChoice*>(_writer->getKnobByName("compression").get());
        ASSERT_TRUE(compression != NULL);
        compression->setValueFromID("none", 0);

        KnobChannelSet* writerChannels = dynamic_cast<KnobChannelSet*>(_writer->getKnobByName(kNodeParamChannelSet).get());
        ASSERT_TRUE(writerChannels != NULL) << "the Write container has no channel set knob";
        writerChannels->setAll();
    }

    // Renders `frame` to its own file and parses the result.
    void render(const char* fileName,
                FlatExrImage* image,
                int frame = 1)
    {
        if (!_writer) {
            createWriter();
            if (HasFatalFailure()) {
                return;
            }
        }
        ASSERT_TRUE(_tmp.isValid());
        const std::string path = (_tmp.path() + QLatin1String("/") + QString::fromUtf8(fileName)).toStdString();
        _writer->setOutputFilesForWriter(path);
        QFile::remove(QString::fromStdString(path));

        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());
        ASSERT_TRUE(writerEffect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, frame, frame, 1, false));
        getApp()->startWritersRendering(false, works);

        ASSERT_TRUE(QFile::exists(QString::fromStdString(path))) << "frame was not rendered: " << path;
        std::string error;
        ASSERT_TRUE(readFlatExr(path, image, &error)) << error;
        QFile::remove(QString::fromStdString(path));
    }

    QTemporaryDir _tmp;
    NodePtr _add;
    NodePtr _last;
    NodePtr _writer;
    KnobChannelSetPtr _layers;
};

TEST_F(AddLayersRenderTest, AddingMaskWritesZerosAndLeavesTheExistingLayersUntouched)
{
    registerMask();
    createAddOnFixture("flat-three-layers.exr");
    if (HasFatalFailure()) {
        return;
    }
    _layers->setLayer(0, "mask", NULL);
    _layers->addLayer("diffuse", NULL);

    FlatExrImage image;
    render("add_mask.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B", "mask.A" }), channelSet(image));
    expectPlane(image, "", 1.f, 0.f, 0.f);
    expectChannel(image, "A", 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
    expectChannel(image, "mask.A", 0.f);
}

TEST_F(AddLayersRenderTest, AddingAlphaToAnRgbInputWidensAndKeepsTheColourChannels)
{
    createAddOnFixture("flat-rgb-only.exr");
    if (HasFatalFailure()) {
        return;
    }
    _layers->setLayer(0, kNatronColorViewAlpha, NULL);

    FlatExrImage image;
    render("add_alpha.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "R", "G", "B", "A" }), channelSet(image));
    expectPlane(image, "", 1.f, 0.f, 0.f);
    expectChannel(image, "A", 0.f);
}

TEST_F(AddLayersRenderTest, AddingRgbToAColourlessStreamWritesZeroColourAndTheOtherLayers)
{
    NodePtr reader = createReader("flat-three-layers.exr");
    ASSERT_TRUE(bool(reader));
    NodePtr remove = createNode(QString::fromUtf8(PLUGINID_NATRON_REMOVELAYERS));
    ASSERT_TRUE(bool(remove));
    connectNodes(reader, remove, 0, true);
    createAddOn(remove);
    if (HasFatalFailure()) {
        return;
    }
    KnobChannelSetPtr removed = std::dynamic_pointer_cast<KnobChannelSet>(remove->getKnobByName(kRemoveLayersParamChannels));
    ASSERT_TRUE(bool(removed));
    removed->setLayer(0, kNatronColorViewRGBA, NULL);
    _layers->setLayer(0, kNatronColorViewRGB, NULL);

    FlatExrImage image;
    render("add_rgb_colourless.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "R", "G", "B", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectPlane(image, "", 0.f, 0.f, 0.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

// Each render parks the timeline on the other frame, so reading the current frame instead of
// the render's time fails either way.
TEST_F(AddLayersRenderTest, DiffuseVariesPerFrameOverASequence)
{
    registerLayer("diffuse", std::vector<std::string>({ "R", "G", "B" }));
    createAddOn(createReadSequence());
    if (HasFatalFailure()) {
        return;
    }
    _layers->setLayer(0, "diffuse", NULL);

    getApp()->getTimeLine()->seekFrame(2, false, NULL, eTimelineChangeReasonOtherSeek);
    FlatExrImage image1;
    render("frame1.exr", &image1, 1);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(names({ "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }), channelSet(image1));
    expectPlane(image1, "diffuse.", 0.f, 1.f, 0.f);

    getApp()->getTimeLine()->seekFrame(1, false, NULL, eTimelineChangeReasonOtherSeek);
    FlatExrImage image2;
    render("frame2.exr", &image2, 2);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(names({ "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B" }), channelSet(image2));
    expectPlane(image2, "", 1.f, 0.f, 0.f);
    expectPlane(image2, "diffuse.", 0.f, 0.f, 0.f);
}

// All four offset components are 0.25 so the result does not depend on which Grade parameter
// the single-channel layer maps onto.
TEST_F(AddLayersRenderTest, AGradeCanSelectTheAddedLayer)
{
    registerMask();
    createAddOnFixture("flat-three-layers.exr");
    if (HasFatalFailure()) {
        return;
    }
    _layers->setLayer(0, "mask", NULL);

    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(grade));
    connectNodes(_last, grade, 0, true);
    _last = grade;
    KnobChannelSetPtr gradeChannels = std::dynamic_pointer_cast<KnobChannelSet>(grade->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(gradeChannels));
    gradeChannels->setLayer(0, "mask", NULL);
    KnobColor* offset = dynamic_cast<KnobColor*>(grade->getKnobByName("offset").get());
    ASSERT_TRUE(offset != NULL);
    offset->setValues(0.25, 0.25, 0.25, 0.25, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    FlatExrImage image;
    render("grade_mask.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_GE(image.channelIndex("mask.A"), 0);
    expectChannel(image, "mask.A", 0.25f);
    expectPlane(image, "", 1.f, 0.f, 0.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
}

TEST_F(AddLayersRenderTest, RegisteringTheNamedLayerBetweenRendersProducesIt)
{
    createAddOnFixture("flat-three-layers.exr");
    if (HasFatalFailure()) {
        return;
    }
    _layers->setLayer(0, "mask", NULL);

    FlatExrImage before;
    render("before_register.exr", &before);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(-1, before.channelIndex("mask.A"));

    registerMask();

    FlatExrImage after;
    render("after_register.exr", &after);
    if (HasFatalFailure()) {
        return;
    }
    ASSERT_GE(after.channelIndex("mask.A"), 0) << "the render served the result cached before the layer was registered";
    expectChannel(after, "mask.A", 0.f);
    expectPlane(after, "diffuse.", 0.f, 1.f, 0.f);
}
