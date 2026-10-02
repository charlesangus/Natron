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

#include <algorithm>
#include <bitset>
#include <list>
#include <map>
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
#include "MultiplanarTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/RemoveLayers.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

// The fixture (Tests/fixtures/flat-three-layers.exr) is an 8x8 half EXR whose every pixel is
// Color (1, 0, 0, 1), diffuse (0, 1, 0) and specular (0, 0, 1), so one pixel stands for all.
const int32_t kCheckX = 1;
const int32_t kCheckY = 1;
const char* const kGradePluginID = "net.sf.openfx.GradePlugin";
const char* const kBlurPluginID = "net.sf.cimg.CImgBlur";
const char* const kMergePluginID = "net.sf.openfx.MergePlugin";

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
float
valueAt(const FlatExrImage& image,
        const std::string& channel)
{
    return image.at(image.x1 + kCheckX, image.y1 + kCheckY, channel);
}

void
expectChannel(const FlatExrImage& image,
              const std::string& channel,
              float expected)
{
    EXPECT_NEAR(expected, valueAt(image, channel), 1e-4f) << channel;
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

// Read -> RemoveLayers -> Write, the Write set up to write every layer into a single-part,
// uncompressed 32-bit float EXR.
class RemoveLayersRenderTest
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
    NodePtr createTimeVaryingReadSequence()
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-seq-layers.####.exr"));
        NodePtr reader = getApp()->createNode(readerArgs);
        EXPECT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

        return reader;
    }

    // Carries diffuse at frame 1 only: `which` picks flat-three-layers.exr there and
    // flat-rgba-only.exr at frame 2.
    NodePtr createTimeVaryingSwitch()
    {
        NodePtr readerA = createReader("flat-three-layers.exr");
        NodePtr readerB = createReader("flat-rgba-only.exr");

        NodePtr switchNode = createNode(QString::fromUtf8("net.sf.openfx.switchPlugin"));
        EXPECT_TRUE(bool(switchNode));
        connectNodes(readerA, switchNode, 0, true);
        connectNodes(readerB, switchNode, 1, true);

        KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName("which"));
        EXPECT_TRUE(bool(which));
        if (which) {
            which->setValueAtTime(1, 0, ViewSpec::all(), 0);
            which->setValueAtTime(2, 1, ViewSpec::all(), 0);
        }

        return switchNode;
    }

    void createRemoveOn(const NodePtr& source)
    {
        _remove = createNode(QString::fromUtf8(PLUGINID_NATRON_REMOVELAYERS));
        ASSERT_TRUE(bool(_remove));
        connectNodes(source, _remove, 0, true);

        _channels = std::dynamic_pointer_cast<KnobChannelSet>(_remove->getKnobByName(kRemoveLayersParamChannels));
        ASSERT_TRUE(bool(_channels));
        _last = _remove;
    }

    void createRemoveOnFixture(const std::string& fixture = "flat-three-layers.exr")
    {
        NodePtr reader = createReader(fixture);
        ASSERT_TRUE(bool(reader));
        createRemoveOn(reader);
    }

    void setKeep()
    {
        KnobChoicePtr operation = std::dynamic_pointer_cast<KnobChoice>(_remove->getKnobByName(kRemoveLayersParamOperation));
        ASSERT_TRUE(bool(operation));
        operation->setValue((int)RemoveLayers::eOperationKeep);
    }

    NodePtr appendGrade(KnobChannelSetPtr* gradeChannels)
    {
        NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
        EXPECT_TRUE(bool(grade));
        connectNodes(_last, grade, 0, true);
        *gradeChannels = std::dynamic_pointer_cast<KnobChannelSet>(grade->getKnobByName(kNodeParamChannelSet));
        EXPECT_TRUE(bool(*gradeChannels));

        KnobColor* offset = dynamic_cast<KnobColor*>(grade->getKnobByName("offset").get());
        EXPECT_TRUE(offset != NULL);
        if (offset) {
            offset->setValues(0.25, 0., 0., 0.25, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        }
        _last = grade;

        return grade;
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

    // Each render parks the timeline on the other frame, so reading the current frame instead of
    // the render's time fails either way.
    void expectDiffuseRemovedPerFrame(const NodePtr& source)
    {
        createRemoveOn(source);
        if (HasFatalFailure()) {
            return;
        }
        _channels->setRegex(0, "diff.*");

        getApp()->getTimeLine()->seekFrame(2, false, NULL, eTimelineChangeReasonOtherSeek);
        FlatExrImage image1;
        render("frame1.exr", &image1, 1);
        if (HasFatalFailure()) {
            return;
        }
        EXPECT_EQ(names({ "R", "G", "B", "A", "specular.R", "specular.G", "specular.B" }), channelSet(image1));
        expectPlane(image1, "", 1.f, 0.f, 0.f);
        expectPlane(image1, "specular.", 0.f, 0.f, 1.f);

        getApp()->getTimeLine()->seekFrame(1, false, NULL, eTimelineChangeReasonOtherSeek);
        FlatExrImage image2;
        render("frame2.exr", &image2, 2);
        if (HasFatalFailure()) {
            return;
        }
        EXPECT_EQ(names({ "R", "G", "B", "A" }), channelSet(image2));
        expectPlane(image2, "", 1.f, 0.f, 0.f);
    }

    // A constant plane blurred with nearest-pixel borders is itself, so the Blur really renders
    // (it is not an identity) and still leaves every value unchanged.
    NodePtr appendBlurOnAll()
    {
        NodePtr blur = createNode(QString::fromUtf8(kBlurPluginID));
        EXPECT_TRUE(bool(blur));
        if (!blur) {
            return blur;
        }
        connectNodes(_last, blur, 0, true);

        KnobDouble* size = dynamic_cast<KnobDouble*>(blur->getKnobByName("size").get());
        EXPECT_TRUE(size != NULL);
        if (size) {
            size->setValues(3., 3., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        }
        KnobChoice* boundary = dynamic_cast<KnobChoice*>(blur->getKnobByName("boundary").get());
        EXPECT_TRUE(boundary != NULL);
        if (boundary) {
            boundary->setValueFromID("nearest", 0);
        }
        KnobBool* expandRoD = dynamic_cast<KnobBool*>(blur->getKnobByName("expandRoD").get());
        EXPECT_TRUE(expandRoD != NULL);
        if (expandRoD) {
            expandRoD->setValue(false);
        }
        KnobChannelSetPtr blurChannels = std::dynamic_pointer_cast<KnobChannelSet>(blur->getKnobByName(kNodeParamChannelSet));
        EXPECT_TRUE(bool(blurChannels));
        if (blurChannels) {
            blurChannels->setAll();
        }
        _last = blur;

        return blur;
    }

    static void expectNoPersistentMessage(const NodePtr& node)
    {
        ASSERT_TRUE(bool(node));
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type);
        EXPECT_FALSE(node->hasPersistentMessage()) << node->getScriptName() << ": " << message.toStdString();
    }

    // Asks node for one plane over its whole RoD the way ViewerInstance::renderViewer_internal()
    // does: a plain renderRoI() under a request pass. An Ok render with no image is what the
    // viewer shows as black.
    EffectInstance::RenderRoIRetCode renderPlaneLikeTheViewer(const NodePtr& node,
                                                              const ImageLayerDesc& plane,
                                                              ImagePtr* image)
    {
        image->reset();

        const double time = 1.;
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstancePtr effect = node->getEffectInstance();

        RectD rod;
        bool isProjectFormat = false;
        if (effect->getRegionOfDefinition_public(node->getHashValue(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }

        FrameRequestMap request;
        if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, rod, node, request) == eStatusFailed) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        frameRenderArgs.updateNodesRequest(request);

        std::list<ImageLayerDesc> components;
        components.push_back(plane);
        EffectInstance::RenderRoIArgs args(time,
                                           RenderScale::identity,
                                           0 /*mipmapLevel*/,
                                           ViewIdx(0),
                                           false /*byPassCache*/,
                                           rod.toPixelEnclosing(0, effect->getAspectRatio(-1)),
                                           rod,
                                           components,
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           time);
        std::map<ImageLayerDesc, ImagePtr> layers;
        const EffectInstance::RenderRoIRetCode code = effect->renderRoI(args, &layers);
        if ((code == EffectInstance::eRenderRoIRetCodeOk) && !layers.empty()) {
            *image = layers.begin()->second;
        }

        return code;
    }

    QTemporaryDir _tmp;
    NodePtr _remove;
    NodePtr _last;
    NodePtr _writer;
    KnobChannelSetPtr _channels;
};

TEST_F(RemoveLayersRenderTest, RemovingANonColorLayerWritesTheRestUnchanged)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, "diffuse", NULL);

    FlatExrImage image;
    render("remove_diffuse.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "R", "G", "B", "A", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectPlane(image, "", 1.f, 0.f, 0.f);
    expectChannel(image, "A", 1.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(RemoveLayersRenderTest, RemovingAlphaWritesNoAlphaChannel)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewAlpha, NULL);

    FlatExrImage image;
    render("remove_alpha.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "R", "G", "B", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectPlane(image, "", 1.f, 0.f, 0.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(RemoveLayersRenderTest, RemovingRgbWritesOnlyAlphaOfTheColorPlane)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewRGB, NULL);

    FlatExrImage image;
    render("remove_rgb.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectChannel(image, "A", 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(RemoveLayersRenderTest, KeepingAlphaAndSpecularWritesOnlyThose)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    setKeep();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewAlpha, NULL);
    _channels->addRegex("spec.*");

    FlatExrImage image;
    render("keep_alpha_specular.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "A", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectChannel(image, "A", 1.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

// The offset is (0.25, 0, 0, 0.25), so a red of 1.25 shows the Grade ran on the narrowed plane.
TEST_F(RemoveLayersRenderTest, GradeOnAllChannelsOverANarrowedPlaneStaysRgb)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewAlpha, NULL);

    KnobChannelSetPtr gradeChannels;
    appendGrade(&gradeChannels);
    if (HasFatalFailure()) {
        return;
    }
    gradeChannels->setAll();

    FlatExrImage image;
    render("remove_alpha_grade_all.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_GE(image.channelIndex("R"), 0);
    EXPECT_EQ(-1, image.channelIndex("A")) << "the Grade's output widened the colour plane back to RGBA";
    expectPlane(image, "", 1.25f, 0.f, 0.f);
}

TEST_F(RemoveLayersRenderTest, GradeOnRgbaOverANarrowedPlaneWidensAndGradesAlphaFromZero)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewAlpha, NULL);

    KnobChannelSetPtr gradeChannels;
    appendGrade(&gradeChannels);
    if (HasFatalFailure()) {
        return;
    }
    gradeChannels->setLayer(0, kNatronColorViewRGBA, NULL);

    FlatExrImage image;
    render("remove_alpha_grade_rgba.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    ASSERT_GE(image.channelIndex("A"), 0) << "the Grade on rgba did not widen the plane";
    expectPlane(image, "", 1.25f, 0.f, 0.f);
    expectChannel(image, "A", 0.25f);
}

TEST_F(RemoveLayersRenderTest, RegexRemovalVariesPerFrameOverASequence)
{
    expectDiffuseRemovedPerFrame(createTimeVaryingReadSequence());
}

TEST_F(RemoveLayersRenderTest, RegexRemovalVariesPerFrameOverASwitch)
{
    expectDiffuseRemovedPerFrame(createTimeVaryingSwitch());
}

TEST_F(RemoveLayersRenderTest, RemovingRgbaWritesNoColourChannels)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);

    FlatExrImage image;
    render("remove_rgba.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
    expectNoPersistentMessage(_remove);
    expectNoPersistentMessage(_writer);
}

// An All row selects the layers the stream has, so over a colourless stream it must not bring
// back a colour plane for the Write to pick up.
TEST_F(RemoveLayersRenderTest, BlurOnAllOverAColourlessStreamWritesNoColourChannels)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);

    NodePtr blur = appendBlurOnAll();
    if (HasFatalFailure() || !blur) {
        return;
    }

    FlatExrImage image;
    render("remove_rgba_blur_all.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
    expectNoPersistentMessage(blur);
    expectNoPersistentMessage(_writer);
}

// The multiply is 2 so the Grade really renders; its source colour reads zero, so does its output.
TEST_F(RemoveLayersRenderTest, GradeOnRgbaOverAColourlessStreamWritesAZeroColourPlane)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);

    KnobChannelSetPtr gradeChannels;
    NodePtr grade = appendGrade(&gradeChannels);
    if (HasFatalFailure() || !grade || !gradeChannels) {
        return;
    }
    gradeChannels->setLayer(0, kNatronColorViewRGBA, NULL);
    KnobColor* offset = dynamic_cast<KnobColor*>(grade->getKnobByName("offset").get());
    ASSERT_TRUE(offset != NULL);
    offset->setValues(0., 0., 0., 0., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);
    multiply->setValues(2., 2., 2., 2., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    FlatExrImage image;
    render("remove_rgba_grade_rgba.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectPlane(image, "", 0.f, 0.f, 0.f);
    expectChannel(image, "A", 0.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
    expectNoPersistentMessage(grade);
    expectNoPersistentMessage(_writer);
}

TEST_F(RemoveLayersRenderTest, ViewerRenderOfAColourlessStreamIsBlackOnRgbaAndGreenOnDiffuse)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);

    ImagePtr colorImage;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderPlaneLikeTheViewer(_remove, ImageLayerDesc::getRGBAComponents(), &colorImage));
    EXPECT_FALSE(bool(colorImage)) << "the colour plane the stream lacks was rendered";
    expectNoPersistentMessage(_remove);

    std::list<ImageLayerDesc> present;
    _remove->getEffectInstance()->getPresentLayers(1., ViewIdx(0), -1, &present);
    ImageLayerDesc diffuse;
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (it->getLayerID() == "diffuse") {
            diffuse = *it;
        }
    }
    ASSERT_EQ(3, diffuse.getNumComponents()) << "the stream lost its diffuse layer";

    ImagePtr diffuseImage;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderPlaneLikeTheViewer(_remove, diffuse, &diffuseImage));
    ASSERT_TRUE(bool(diffuseImage));
    ASSERT_EQ(eImageBitDepthFloat, diffuseImage->getBitDepth());
    ASSERT_EQ(3, (int)diffuseImage->getComponentsCount());
    const RectI bounds = diffuseImage->getBounds();
    Image::ReadAccess access = diffuseImage->getReadRights();
    const float* pixel = (const float*)access.pixelAt(bounds.x1 + kCheckX, bounds.y1 + kCheckY);
    ASSERT_TRUE(pixel != NULL);
    EXPECT_NEAR(0.f, pixel[0], 1e-4f);
    EXPECT_NEAR(1.f, pixel[1], 1e-4f);
    EXPECT_NEAR(0.f, pixel[2], 1e-4f);
}

// The rows hidden behind a row-0 All select nothing, so an rgba row among them must not bring
// back a colour plane the stream lacks.
TEST_F(RemoveLayersRenderTest, BlurOnAllWithAHiddenRgbaRowOverAColourlessStreamWritesNoColourChannels)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);

    NodePtr blur = appendBlurOnAll();
    if (HasFatalFailure() || !blur) {
        return;
    }
    KnobChannelSetPtr blurChannels = std::dynamic_pointer_cast<KnobChannelSet>(blur->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(blurChannels));
    blurChannels->addLayer(kNatronColorViewRGBA, NULL);
    ASSERT_EQ(2u, blurChannels->getRows().size());
    EXPECT_EQ(ChannelSetRow::eModeAll, blurChannels->getRows()[0].mode);

    FlatExrImage image;
    render("remove_rgba_blur_all_hidden_rgba.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
    expectNoPersistentMessage(blur);
    expectNoPersistentMessage(_writer);
}

TEST_F(RemoveLayersRenderTest, BlurOnNoneOverAColourlessStreamProducesNoColourPlane)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);

    NodePtr blur = appendBlurOnAll();
    if (HasFatalFailure() || !blur) {
        return;
    }
    KnobChannelSetPtr blurChannels = std::dynamic_pointer_cast<KnobChannelSet>(blur->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(blurChannels));
    blurChannels->setNone();

    std::list<ImageLayerDesc> present;
    blur->getEffectInstance()->getPresentLayers(1., ViewIdx(0), -1, &present);
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        EXPECT_FALSE(it->isColorLayer()) << "the Blur presents a colour plane its input lacks";
    }

    FlatExrImage image;
    render("remove_rgba_blur_none.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    EXPECT_EQ(names({ "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
    expectNoPersistentMessage(blur);
    expectNoPersistentMessage(_writer);
}

// B is the colourless stream and Merge's pass-through input, but A brings colour: A over a zero
// B is A, (1, 0, 0, 1).
TEST_F(RemoveLayersRenderTest, MergeOnAllKeepsTheColourOfAOverAColourlessB)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);
    NodePtr colored = createReader("flat-rgba-only.exr");
    ASSERT_TRUE(bool(colored));

    NodePtr merge = createNode(QString::fromUtf8(kMergePluginID));
    ASSERT_TRUE(bool(merge));
    int inputA = -1;
    int inputB = -1;
    for (int i = 0; i < merge->getNInputs(); ++i) {
        if (merge->getInputLabel(i) == "A") {
            inputA = i;
        } else if (merge->getInputLabel(i) == "B") {
            inputB = i;
        }
    }
    ASSERT_GE(inputA, 0);
    ASSERT_GE(inputB, 0);
    connectNodes(_remove, merge, inputB, true);
    connectNodes(colored, merge, inputA, true);
    KnobChannelSetPtr mergeChannels = std::dynamic_pointer_cast<KnobChannelSet>(merge->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(mergeChannels));
    mergeChannels->setAll();
    _last = merge;

    FlatExrImage image;
    render("remove_rgba_merge_a.exr", &image);
    if (HasFatalFailure()) {
        return;
    }

    const std::set<std::string> written = channelSet(image);
    for (const char* channel : { "R", "G", "B", "A" }) {
        EXPECT_EQ(1u, written.count(channel)) << channel << " was not written";
    }
    expectPlane(image, "", 1.f, 0.f, 0.f);
    expectChannel(image, "A", 1.f);
    expectNoPersistentMessage(merge);
    expectNoPersistentMessage(_writer);
}

// The multiplanar effect produces the colour plane itself, so a colourless pass-through input
// does not take it away; only an encoder's colour comes from that input alone.
TEST_F(RemoveLayersRenderTest, AMultiplanarEffectOverAColourlessStreamStillProducesColour)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);

    NodePtr multiplanar = createNode(QString::fromUtf8(kTestPluginIDMultiplanarDiffuseOnly));
    ASSERT_TRUE(bool(multiplanar));
    connectNodes(_remove, multiplanar, 0, true);

    EffectInstancePtr effect = multiplanar->getEffectInstance();
    EffectInstance::ComponentsNeededMap comps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime = 0.;
    int passThroughView = 0;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInputNb = -1;
    effect->getComponentsNeededAndProduced_public(effect->getRenderHash(), 1., ViewIdx(0), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);
    ASSERT_EQ(0, passThroughInputNb);

    const std::list<ImageLayerDesc>& produced = comps[-1];
    EXPECT_TRUE(std::any_of(produced.begin(), produced.end(), [](const ImageLayerDesc& layer) {
        return layer.isColorLayer() && (layer.getNumComponents() > 0);
    })) << "the colour plane the effect produces was dropped";

    std::list<ImageLayerDesc> present;
    effect->getPresentLayers(1., ViewIdx(0), -1, &present);
    EXPECT_TRUE(std::any_of(present.begin(), present.end(), [](const ImageLayerDesc& layer) {
        return layer.isColorLayer();
    }));
}

// An upstream RemoveLayers takes alpha out on frame 1 only, being disabled on frame 2, so the
// stream's colour plane is RGB on frame 1 and RGBA on frame 2. Removing rgb then drops the plane on
// frame 1 and narrows it to alpha on frame 2. Metadata hold for every frame, so they follow the
// input's own RGB metadata whichever frame the timeline is on when they are computed.
TEST_F(RemoveLayersRenderTest, MetadataDoNotDependOnTheFrameTheTimelineIsOn)
{
    createRemoveOnFixture();
    if (HasFatalFailure()) {
        return;
    }
    NodePtr upstream = _remove;
    _channels->setLayer(0, kNatronColorViewAlpha, NULL);
    KnobBoolPtr disable = upstream->getDisabledKnob();
    ASSERT_TRUE(bool(disable));
    disable->setValueAtTime(1, false, ViewSpec::all(), 0);
    disable->setValueAtTime(2, true, ViewSpec::all(), 0);

    getApp()->getTimeLine()->seekFrame(2, false, NULL, eTimelineChangeReasonOtherSeek);
    createRemoveOn(upstream);
    if (HasFatalFailure()) {
        return;
    }
    _channels->setLayer(0, kNatronColorViewRGB, NULL);

    RemoveLayers* remove = dynamic_cast<RemoveLayers*>(_remove->getEffectInstance().get());
    ASSERT_TRUE(remove != NULL);
    ASSERT_EQ(RemoveLayers::ColorOutcome::eKindDropped, remove->computeColorOutcome(1., ViewIdx(0)).kind);
    ASSERT_EQ(RemoveLayers::ColorOutcome::eKindNarrowed, remove->computeColorOutcome(2., ViewIdx(0)).kind);

    remove->refreshMetadata_public(true);
    const int nCompsParkedOnFrame2 = remove->getMetadataNComps(-1);

    getApp()->getTimeLine()->seekFrame(1, false, NULL, eTimelineChangeReasonOtherSeek);
    remove->refreshMetadata_public(true);
    const int nCompsParkedOnFrame1 = remove->getMetadataNComps(-1);

    EXPECT_EQ(nCompsParkedOnFrame1, nCompsParkedOnFrame2);
    EXPECT_EQ(3, nCompsParkedOnFrame2);

    // The encoder's clip keeps the RGB layout the metadata advertise, so the frame whose colour
    // plane is alpha alone is written with its R, G and B read as zero.
    FlatExrImage image;
    render("frame2_parked_on_1.exr", &image, 2);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(names({ "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B" }), channelSet(image));
    expectPlane(image, "", 0.f, 0.f, 0.f);
    expectChannel(image, "A", 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
}
