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

#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kSize = 8;
const double kTime = 1.;

struct SourceChannel {
    const char* name;
    float value;
};

const SourceChannel kSourceChannels[] = {
    { "R", 0.125f }, { "G", 0.25f }, { "B", 0.375f }, { "A", 1.f }, { "diffuse.R", 0.5f }, { "diffuse.G", 0.625f }, { "diffuse.B", 0.75f }, { "specular.R", 0.0625f }, { "specular.G", 0.1875f }, { "specular.B", 0.3125f }, { "mask.A", 0.875f }
};

bool
writeExr(const std::string& path,
         const std::vector<std::string>& names,
         const std::vector<float>& values,
         int alphaChannel,
         const std::string& outputHash,
         std::string* error)
{
    const int n = (int)names.size();
    OIIO::ImageSpec spec(kSize, kSize, n, OIIO::TypeDesc::FLOAT);

    spec.channelnames = names;
    spec.alpha_channel = alphaChannel;
    spec.attribute("compression", "none");
    if (!outputHash.empty()) {
        spec.attribute(kNatronWriteOutputHashAttribute, outputHash);
    }
    std::vector<float> pixels((std::size_t)kSize * kSize * n);
    for (int p = 0; p < kSize * kSize; ++p) {
        for (int c = 0; c < n; ++c) {
            pixels[(std::size_t)p * n + c] = values[(std::size_t)c];
        }
    }
    std::unique_ptr<OIIO::ImageOutput> output = OIIO::ImageOutput::create(path);
    if (!output || !output->open(path, spec) || !output->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) || !output->close()) {
        *error = output ? output->geterror() : OIIO::geterror();

        return false;
    }

    return true;
}

bool
writeSource(const std::string& path,
            std::string* error)
{
    std::vector<std::string> names;
    std::vector<float> values;

    for (std::size_t c = 0; c < sizeof(kSourceChannels) / sizeof(kSourceChannels[0]); ++c) {
        names.push_back(kSourceChannels[c].name);
        values.push_back(kSourceChannels[c].value);
    }

    return writeExr(path, names, values, 3, std::string(), error);
}

std::set<std::string>
layerIDs(const std::list<ImageLayerDesc>& layers)
{
    std::set<std::string> ids;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        ids.insert(it->isColorLayer() ? std::string(kNatronColorLayerID) : it->getLayerID());
    }

    return ids;
}

// Every pixel of `pixels`, interleaved over `expected.size()` channels, holds `expected`.
void
expectEveryPixel(const std::vector<float>& pixels,
                 const std::vector<float>& expected)
{
    ASSERT_EQ((std::size_t)kSize * kSize * expected.size(), pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        EXPECT_NEAR(expected[i % expected.size()], pixels[i], 1e-6f) << "channel " << i % expected.size();
    }
}
} // namespace

class NativeWritePullTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kSize, kSize, "nativeWritePull", 1.));

        ASSERT_TRUE(_dir.isValid());
        std::string error;
        ASSERT_TRUE(writeSource(pathOf("source.exr"), &error)) << error;

        _reader = createWorkingSpaceRead(pathOf("source.exr"));
        ASSERT_TRUE(bool(_reader));
        _output = pathOf("out.exr");
        _writer = createNativeWriter(_output);
        ASSERT_TRUE(bool(_writer));
        _dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
        ASSERT_TRUE(bool(_dot));
        connectNodes(_reader, _writer, 0, true);
        connectNodes(_writer, _dot, 0, true);

        _channels = std::dynamic_pointer_cast<KnobChannelSet>(_writer->getKnobByName(kNodeParamChannelSet));
        ASSERT_TRUE(bool(_channels));
        _channels->setAll();
        setChoice(kNatronWriteParamBitDepth, "32f");
        setChoice(kNatronWriteParamCompression, "none");
    }

    virtual void TearDown() OVERRIDE
    {
        _dot.reset();
        _writer.reset();
        _reader.reset();
        _channels.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    std::string pathOf(const std::string& name) const
    {
        return (_dir.path() + QLatin1Char('/') + QString::fromStdString(name)).toStdString();
    }

    bool outputExists() const
    {
        return QFile::exists(QString::fromStdString(_output));
    }

    void setChoice(const char* name,
                   const std::string& id)
    {
        KnobChoice* choice = dynamic_cast<KnobChoice*>(_writer->getKnobByName(name).get());

        ASSERT_TRUE(choice != NULL) << name;
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

                return;
            }
        }
        ADD_FAILURE() << name << " has no option " << id;
    }

    void setReadBack(bool on)
    {
        KnobBool* knob = dynamic_cast<KnobBool*>(_writer->getKnobByName(kNatronWriteParamReadBack).get());

        ASSERT_TRUE(knob != NULL);
        knob->setValue(on, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    }

    void renderToDisk()
    {
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());

        ASSERT_TRUE(effect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);
    }

    void setFile(const std::string& name)
    {
        KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(_writer->getKnobByName(kOfxImageEffectFileParamName).get());

        ASSERT_TRUE(file != NULL);
        _output = pathOf(name);
        file->setValue(_output);
    }

    // Replaces the output with an EXR of `names` holding `values`, stamped with the Write's current
    // output content hash: read back shows a file only while it holds that hash.
    void overwriteOutput(const std::vector<std::string>& names,
                         const std::vector<float>& values)
    {
        std::string error;

        ASSERT_TRUE(writeExr(_output, names, values, 3, NativeWrite::outputHashText(dynamic_cast<NativeWrite*>(_writer->getEffectInstance().get())->getOutputContentHash()), &error)) << error;
        _writer->getEffectInstance()->purgeCaches();
    }

    bool pull(const NodePtr& node,
              const ImageLayerDesc& layer,
              std::vector<float>* pixels,
              std::vector<std::string>* channels = 0)
    {
        std::vector<RenderedPlane> planes;
        std::string error;

        if (!renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, RectI(0, 0, kSize, kSize), std::list<ImageLayerDesc>(1, layer), &planes, &error) || (planes.size() != 1)) {
            ADD_FAILURE() << "pull of " << layer.getLayerID() << " failed: " << error;

            return false;
        }
        *pixels = planes[0].pixels;
        if (channels) {
            *channels = planes[0].channels;
        }

        return true;
    }

    int fileChannelCount() const
    {
        OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(_output);
        if (!input) {
            return -1;
        }
        const int n = input->spec().nchannels;
        input->close();

        return n;
    }

    // Pulls the colour of `node` as a viewer's playback does: a sequential frame rooted at the node
    // pulling rather than at the Write.
    bool pullSequential(const NodePtr& node,
                        std::vector<float>* pixels)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        ParallelRenderArgsSetter frameRenderArgs(kTime,
                                                 ViewIdx(0),
                                                 false /*isRenderUserInteraction*/,
                                                 true /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstancePtr effect = node->getEffectInstance();
        const RectI roi(0, 0, kSize, kSize);
        FrameRequestMap request;
        if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0, roi.toCanonical_noClipping(0, effect->getTimeInvariantMetadata()->getPixelAspectRatio(-1)), node, request) == eStatusFailed) {
            ADD_FAILURE() << "request pass failed";

            return false;
        }
        frameRenderArgs.updateNodesRequest(request);

        const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
        EffectInstance::RenderRoIArgs args(kTime,
                                           RenderScale::identity,
                                           0,
                                           ViewIdx(0),
                                           true /*byPassCache*/,
                                           roi,
                                           RectD(),
                                           std::list<ImageLayerDesc>(1, rgba),
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           kTime);
        std::map<ImageLayerDesc, ImagePtr> layers;
        if (effect->renderRoI(args, &layers) != EffectInstance::eRenderRoIRetCodeOk) {
            ADD_FAILURE() << "renderRoI failed";

            return false;
        }
        std::map<ImageLayerDesc, ImagePtr>::const_iterator found = layers.find(rgba);
        if ((found == layers.end()) || !found->second || (found->second->getBitDepth() != eImageBitDepthFloat)) {
            ADD_FAILURE() << "no float colour plane";

            return false;
        }
        const ImagePtr& image = found->second;
        if (!image->getBounds().contains(roi)) {
            ADD_FAILURE() << "the plane does not cover the frame";

            return false;
        }
        const std::size_t nComps = image->getComponentsCount();
        pixels->assign((std::size_t)kSize * kSize * nComps, 0.f);
        Image::ReadAccess access(image.get());
        for (int y = 0; y < kSize; ++y) {
            std::memcpy(&(*pixels)[(std::size_t)y * kSize * nComps], access.pixelAt(0, y), (std::size_t)kSize * nComps * sizeof(float));
        }

        return true;
    }

    std::list<ImageLayerDesc> layersSeenDownstream()
    {
        std::list<ImageLayerDesc> layers;

        _dot->getEffectInstance()->getAvailableLayers(kTime, ViewIdx(0), 0, &layers);

        return layers;
    }

    NodePtr _reader;
    NodePtr _writer;
    NodePtr _dot;
    KnobChannelSetPtr _channels;
    std::string _output;
    QTemporaryDir _dir;
};

TEST_F(NativeWritePullTest, PullingTheWriteWritesNoFileAndReturnsTheInput)
{
    std::vector<float> pixels;

    ASSERT_TRUE(pull(_writer, ImageLayerDesc::getRGBAComponents(), &pixels));
    expectEveryPixel(pixels, { 0.125f, 0.25f, 0.375f, 1.f });
    EXPECT_FALSE(outputExists());
}

TEST_F(NativeWritePullTest, ADownstreamPullWritesNoFileAndReturnsTheInput)
{
    std::vector<float> pixels;

    ASSERT_TRUE(pull(_dot, ImageLayerDesc::getRGBAComponents(), &pixels));
    expectEveryPixel(pixels, { 0.125f, 0.25f, 0.375f, 1.f });
    EXPECT_FALSE(outputExists());
}

TEST_F(NativeWritePullTest, ASequentialPullLikeViewerPlaybackWritesNoFile)
{
    std::vector<float> pixels;

    ASSERT_TRUE(pullSequential(_dot, &pixels));
    expectEveryPixel(pixels, { 0.125f, 0.25f, 0.375f, 1.f });
    EXPECT_FALSE(outputExists());
}

TEST_F(NativeWritePullTest, ATaskGraphPullWritesNoFile)
{
    const RenderMismatch m = renderBothWaysDirect(_dot, kTime, ViewIdx(0), 0, RectI(0, 0, kSize, kSize), { 1, 4 });

    EXPECT_FALSE(m.any) << describe(m);
    EXPECT_FALSE(outputExists());
}

TEST_F(NativeWritePullTest, AllChannelsExposeEveryLayerDownstream)
{
    EXPECT_EQ(std::set<std::string>({ kNatronColorLayerID, "diffuse", "specular", "mask" }), layerIDs(layersSeenDownstream()));
}

TEST_F(NativeWritePullTest, RedAndGreenOnExrShowTheRgbTripleTheFileGets)
{
    const std::vector<std::string> redGreen = { "R", "G" };

    _channels->setLayer(0, kNatronColorViewRGBA, &redGreen);

    EXPECT_EQ(std::set<std::string>({ kNatronColorLayerID }), layerIDs(layersSeenDownstream()));
    std::vector<float> pixels;
    std::vector<std::string> channels;
    ASSERT_TRUE(pull(_dot, ImageLayerDesc::getRGBAComponents(), &pixels, &channels));
    EXPECT_EQ(std::vector<std::string>({ "R", "G", "B" }), channels);
    expectEveryPixel(pixels, { 0.125f, 0.25f, 0.375f });

    std::vector<float> sequential;
    ASSERT_TRUE(pullSequential(_dot, &sequential));
    expectEveryPixel(sequential, { 0.125f, 0.25f, 0.375f });
    EXPECT_FALSE(outputExists());
}

TEST_F(NativeWritePullTest, OneLayerOfSeveralIsTheOnlyLayerDownstream)
{
    _channels->setLayer(0, "diffuse", NULL);

    const std::list<ImageLayerDesc> layers = layersSeenDownstream();
    ASSERT_EQ(std::set<std::string>({ "diffuse" }), layerIDs(layers));
    std::vector<float> pixels;
    ASSERT_TRUE(pull(_dot, layers.front(), &pixels));
    expectEveryPixel(pixels, { 0.5f, 0.625f, 0.75f });
    EXPECT_FALSE(outputExists());
}

TEST_F(NativeWritePullTest, OneChannelOfALayerIsTheOnlyChannelDownstream)
{
    const std::vector<std::string> green = { "G" };

    _channels->setLayer(0, "specular", &green);

    const std::list<ImageLayerDesc> layers = layersSeenDownstream();
    ASSERT_EQ(std::set<std::string>({ "specular" }), layerIDs(layers));
    EXPECT_EQ(green, layers.front().getChannels());
    std::vector<float> pixels;
    std::vector<std::string> channels;
    ASSERT_TRUE(pull(_dot, layers.front(), &pixels, &channels));
    EXPECT_EQ(green, channels);
    expectEveryPixel(pixels, { 0.1875f });
    EXPECT_FALSE(outputExists());

    renderToDisk();
    ASSERT_TRUE(outputExists());
    EXPECT_EQ(1, fileChannelCount());
}

TEST_F(NativeWritePullTest, AnExplicitRenderWritesTheFileAndALaterPullDoesNotRewriteIt)
{
    const std::vector<std::string> redGreen = { "R", "G" };

    _channels->setLayer(0, kNatronColorViewRGBA, &redGreen);
    renderToDisk();
    ASSERT_TRUE(outputExists());
    EXPECT_EQ(3, fileChannelCount());

    ASSERT_TRUE(QFile::remove(QString::fromStdString(_output)));
    std::vector<float> pixels;
    std::vector<std::string> channels;
    ASSERT_TRUE(pull(_dot, ImageLayerDesc::getRGBAComponents(), &pixels, &channels));
    EXPECT_EQ(3u, channels.size());
    expectEveryPixel(pixels, { 0.125f, 0.25f, 0.375f });
    EXPECT_FALSE(outputExists());
}

TEST_F(NativeWritePullTest, ReadBackThroughADownstreamNodeIsTheFile)
{
    renderToDisk();
    ASSERT_TRUE(outputExists());
    overwriteOutput({ "R", "G", "B", "A" }, { 0.5f, 0.5f, 0.5f, 1.f });

    setReadBack(true);

    std::vector<float> pixels;
    ASSERT_TRUE(pull(_dot, ImageLayerDesc::getRGBAComponents(), &pixels));
    expectEveryPixel(pixels, { 0.5f, 0.5f, 0.5f, 1.f });
}

TEST_F(NativeWritePullTest, ReadBackShowsTheFilesLayersWhateverTheInputOffers)
{
    renderToDisk();
    ASSERT_TRUE(outputExists());
    setReadBack(true);

    // Each edit below moves the Write's hash, which turns away a file stamped before it, so the
    // file is stamped again: these check that what downstream sees of a current file is the file.
    const auto expectTheFile = [&](const char* when) {
        SCOPED_TRACE(when);
        // Values the source does not have, and a specular of one channel and no mask, so that
        // what downstream sees can only come from the file.
        overwriteOutput({ "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.G" }, { 0.5f, 0.25f, 0.125f, 1.f, 0.75f, 0.5f, 0.25f, 0.625f });
        const std::list<ImageLayerDesc> layers = layersSeenDownstream();
        ASSERT_EQ(std::set<std::string>({ kNatronColorLayerID, "diffuse", "specular" }), layerIDs(layers));
        std::vector<float> pixels;
        std::vector<std::string> channels;
        ASSERT_TRUE(pull(_dot, ImageLayerDesc::getRGBAComponents(), &pixels, &channels));
        EXPECT_EQ(std::vector<std::string>({ "R", "G", "B", "A" }), channels);
        expectEveryPixel(pixels, { 0.5f, 0.25f, 0.125f, 1.f });
        for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
            if (it->getLayerID() == "diffuse") {
                ASSERT_TRUE(pull(_dot, *it, &pixels, &channels));
                EXPECT_EQ(std::vector<std::string>({ "R", "G", "B" }), channels);
                expectEveryPixel(pixels, { 0.75f, 0.5f, 0.25f });
            } else if (it->getLayerID() == "specular") {
                ASSERT_TRUE(pull(_dot, *it, &pixels, &channels));
                EXPECT_EQ(std::vector<std::string>({ "G" }), channels);
                expectEveryPixel(pixels, { 0.625f });
            }
        }
    };

    expectTheFile("as written");

    _channels->setLayer(0, "diffuse", NULL);
    _channels->addLayer("mask", NULL);
    expectTheFile("after the channel set changed");

    disconnectNodes(_reader, _writer, true);
    expectTheFile("after the input was disconnected");
    EXPECT_TRUE(outputExists());
}

TEST_F(NativeWritePullTest, AJpegShowsNeitherTheAlphaNorTheLayerItCannotHold)
{
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);
    _channels->addLayer("diffuse", NULL);
    setFile("out.jpg");
    setChoice(kNatronWriteParamBitDepth, kNatronWriteBitDepthAuto);

    EXPECT_EQ(std::set<std::string>({ kNatronColorLayerID }), layerIDs(layersSeenDownstream()));
    std::vector<float> pixels;
    std::vector<std::string> channels;
    ASSERT_TRUE(pull(_dot, ImageLayerDesc::getRGBAComponents(), &pixels, &channels));
    EXPECT_EQ(std::vector<std::string>({ "R", "G", "B" }), channels);
    expectEveryPixel(pixels, { 0.125f, 0.25f, 0.375f });
    EXPECT_FALSE(outputExists());

    renderToDisk();
    ASSERT_TRUE(outputExists());
    EXPECT_EQ((int)channels.size(), fileChannelCount());
}

TEST_F(NativeWritePullTest, APngKeepsTheAlphaButNotTheLayer)
{
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);
    _channels->addLayer("diffuse", NULL);
    setFile("out.png");
    setChoice(kNatronWriteParamBitDepth, kNatronWriteBitDepthAuto);

    EXPECT_EQ(std::set<std::string>({ kNatronColorLayerID }), layerIDs(layersSeenDownstream()));
    std::vector<float> pixels;
    std::vector<std::string> channels;
    ASSERT_TRUE(pull(_dot, ImageLayerDesc::getRGBAComponents(), &pixels, &channels));
    EXPECT_EQ(std::vector<std::string>({ "R", "G", "B", "A" }), channels);
    expectEveryPixel(pixels, { 0.125f, 0.25f, 0.375f, 1.f });
    EXPECT_FALSE(outputExists());
}

TEST_F(NativeWritePullTest, ARenderToDiskWithNothingTheFormatCanHoldFailsWithAMessage)
{
    _channels->setLayer(0, "diffuse", NULL);
    setFile("aov.png");
    setChoice(kNatronWriteParamBitDepth, kNatronWriteBitDepthAuto);

    EXPECT_TRUE(layersSeenDownstream().empty());
    renderToDisk();

    EXPECT_FALSE(outputExists());
    ASSERT_TRUE(_writer->hasPersistentMessage());
    QString message;
    int type = 0;
    _writer->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("can hold only the colour channels"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("diffuse"))) << message.toStdString();
}
