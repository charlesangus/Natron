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

// The colour views (rgba, rgb, alpha, xy) exist only where a user names or reads a layer; the
// engine's plane lists stay at the storage level, and a view selection reaches the render as the
// storage plane plus a channel mask. An explicit view row that writes a colour channel the stream
// lacks widens the node to RGBA, with the missing channel reading zero; an All row never does.

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

CLANG_DIAG_OFF(deprecated)
#include <QFile>
#include <QString>
#include <QTemporaryDir>
CLANG_DIAG_ON(deprecated)

#include "BaseTest.h"
#include "FlatExrReader.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/PyNode.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>
#include <ofxNatron.h>

NATRON_NAMESPACE_USING

namespace {

const int32_t kCheckX = 1;
const int32_t kCheckY = 1;

const char* const kGradePluginID = "net.sf.openfx.GradePlugin";
const char* const kBlurPluginID = "net.sf.cimg.CImgBlur";
const char* const kMergePluginID = "net.sf.openfx.MergePlugin";
const char* const kIDistortPluginID = "net.sf.openfx.IDistort";
const char* const kPremultPluginID = "net.sf.openfx.Premult";
const char* const kWritePNGPluginID = "fr.inria.openfx.WritePNG";
const char* const kTimeBufferReadPluginID = "net.sf.openfx.TimeBufferRead";
const char* const kTimeBufferWritePluginID = "net.sf.openfx.TimeBufferWrite";

std::string
layerIDsString(const std::list<ImageLayerDesc>& layers)
{
    std::string result;
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (!result.empty()) {
            result += ", ";
        }
        result += it->getLayerID();
    }

    return "[" + result + "]";
}

bool
containsColorViewID(const std::list<ImageLayerDesc>& layers)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (ImageLayerDesc::isColorViewID(it->getLayerID())) {
            return true;
        }
    }

    return false;
}

void
expectNoUserFacingColor(const std::string& text)
{
    EXPECT_EQ(std::string::npos, text.find(kNatronColorStorageLabel)) << text;
    EXPECT_EQ(std::string::npos, text.find(kNatronColorLayerID)) << text;
}

int
countChannel(const FlatExrImage& image,
             const std::string& name)
{
    int count = 0;
    for (std::size_t i = 0; i < image.channels.size(); ++i) {
        if (image.channels[i] == name) {
            ++count;
        }
    }

    return count;
}

void
expectColor(const FlatExrImage& image,
            float r,
            float g,
            float b,
            float a)
{
    EXPECT_NEAR(r, image.at(kCheckX, kCheckY, "R"), 1e-5f) << "R";
    EXPECT_NEAR(g, image.at(kCheckX, kCheckY, "G"), 1e-5f) << "G";
    EXPECT_NEAR(b, image.at(kCheckX, kCheckY, "B"), 1e-5f) << "B";
    EXPECT_NEAR(a, image.at(kCheckX, kCheckY, "A"), 1e-5f) << "A";
}

} // namespace

// flat-three-layers.exr: Color (1, 0, 0, 1), diffuse (0, 1, 0), specular (0, 0, 1).
// flat-rgb-only.exr: Color (1, 0, 0) and no alpha.
// flat-alpha-only.exr: Color has only A (1), no R/G/B.
class ColorViewsRenderTest
    : public BaseTest {
protected:
    NodePtr createReader(const std::string& fixture)
    {
        return createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);
    }

    NodePtr createEffectOnReader(const char* pluginID,
                                 const std::string& fixture,
                                 KnobChannelSetPtr* channels)
    {
        NodePtr reader = createReader(fixture);
        if (!reader) {
            return NodePtr();
        }
        NodePtr effect = createNode(QString::fromUtf8(pluginID));
        if (!effect) {
            return NodePtr();
        }
        connectNodes(reader, effect, 0, true);
        *channels = std::dynamic_pointer_cast<KnobChannelSet>(effect->getKnobByName(kNodeParamChannelSet));

        return effect;
    }

    // Appends a WriteOIIO writing a single-part, uncompressed 32-bit float EXR of `node`, with
    // its channel set chosen by `setChannels`, renders `frame` and parses the result.
    bool render(const NodePtr& node,
                const std::function<void(KnobChannelSet*)>& setChannels,
                FlatExrImage* image,
                std::string* error,
                int frame = 1)
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
        setChannels(writerChannels);

        if (!_tmp.isValid()) {
            *error = "no temporary directory";

            return false;
        }
        const std::string path = (_tmp.path() + QString::fromUtf8("/out") + QString::number(_renderCount++) + QString::fromUtf8(".exr")).toStdString();
        writer->setOutputFilesForWriter(path);

        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
        if (!writerEffect) {
            *error = "writer has no OutputEffectInstance";

            return false;
        }
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, frame, frame, 1, false));
        getApp()->startWritersRendering(false, works);

        if (!QFile::exists(QString::fromStdString(path))) {
            *error = "frame was not rendered: " + path;

            return false;
        }

        return readFlatExr(path, image, error);
    }

    static void writeAll(KnobChannelSet* channels)
    {
        channels->setAll();
    }

    void createTimeBufferPair(const NodePtr& readSource, const NodePtr& writeSource, NodePtr* bufferRead, NodePtr* bufferWrite);

    QTemporaryDir _tmp;
    int _renderCount = 0;
};

TEST_F(ColorViewsRenderTest, PresentLayersNeverContainAColorViewID)
{
    const char* fixtures[] = { "flat-three-layers.exr", "flat-rgb-only.exr" };

    for (std::size_t f = 0; f < sizeof(fixtures) / sizeof(fixtures[0]); ++f) {
        SCOPED_TRACE(fixtures[f]);

        KnobChannelSetPtr gradeChannels;
        NodePtr grade = createEffectOnReader(kGradePluginID, fixtures[f], &gradeChannels);
        ASSERT_TRUE(bool(grade));
        ASSERT_TRUE(bool(gradeChannels));
        gradeChannels->setLayer(0, kNatronColorViewAlpha, NULL);

        NodePtr blur = createNode(QString::fromUtf8(kBlurPluginID));
        ASSERT_TRUE(bool(blur));
        connectNodes(grade, blur, 0, true);

        const NodePtr nodes[] = { grade, blur };
        for (std::size_t n = 0; n < sizeof(nodes) / sizeof(nodes[0]); ++n) {
            EffectInstancePtr effect = nodes[n]->getEffectInstance();
            ASSERT_TRUE(bool(effect));

            for (int inputNb = -1; inputNb <= 0; ++inputNb) {
                std::list<ImageLayerDesc> present;
                effect->getPresentLayers(1, ViewIdx(0), inputNb, &present);
                EXPECT_FALSE(present.empty()) << nodes[n]->getScriptName() << " input " << inputNb;
                EXPECT_FALSE(containsColorViewID(present)) << nodes[n]->getScriptName() << " input " << inputNb << " present=" << layerIDsString(present);

                std::list<ImageLayerDesc> available;
                effect->getAvailableLayers(1, ViewIdx(0), inputNb, &available);
                EXPECT_FALSE(containsColorViewID(available)) << nodes[n]->getScriptName() << " input " << inputNb << " available=" << layerIDsString(available);
            }

            std::list<ImageLayerDesc> listed;
            nodes[n]->listLayersForKnob(nodes[n]->getLayerKnob(), 1, ViewIdx(0), &listed);
            EXPECT_FALSE(containsColorViewID(listed)) << nodes[n]->getScriptName() << " listed=" << layerIDsString(listed);
        }
    }
}

TEST_F(ColorViewsRenderTest, ListLayerViewsForKnobOnRgbOnlyInputListsRgbaRgbAlpha)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-rgb-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));

    std::list<ImageLayerDesc> views;
    grade->listLayerViewsForKnob(grade->getLayerKnob(), 1, ViewIdx(0), &views);

    std::vector<std::string> ids;
    for (std::list<ImageLayerDesc>::const_iterator it = views.begin(); it != views.end(); ++it) {
        ids.push_back(it->getLayerID());
    }
    const std::vector<std::string> expected = { kNatronColorViewRGBA, kNatronColorViewRGB, kNatronColorViewAlpha };
    EXPECT_EQ(expected, ids) << layerIDsString(views);

    std::list<ImageLayerDesc> storage;
    grade->listLayersForKnob(grade->getLayerKnob(), 1, ViewIdx(0), &storage);
    ASSERT_EQ(1u, storage.size()) << layerIDsString(storage);
    EXPECT_EQ(std::string(kNatronColorLayerID), storage.front().getLayerID());
}

TEST_F(ColorViewsRenderTest, ListChannelViewsForKnobOnRgbOnlyInputListsOnlyRgb)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-rgb-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));

    std::list<ImageLayerDesc> views;
    grade->listChannelViewsForKnob(grade->getLayerKnob(), &views);

    ASSERT_EQ(1u, views.size()) << layerIDsString(views);
    EXPECT_EQ(std::string(kNatronColorViewRGB), views.front().getLayerID());
}

TEST_F(ColorViewsRenderTest, GradeRgbGain2DoublesRgbAndKeepsAlpha)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-three-layers.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));

    KnobColor* gain = dynamic_cast<KnobColor*>(grade->getKnobByName("white").get());
    ASSERT_TRUE(gain != NULL);
    gain->setValues(2., 2., 2., 2., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setLayer(0, kNatronColorViewRGB, NULL);

    EXPECT_EQ(4, grade->getEffectInstance()->getMetadataNComps(-1));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    expectColor(image, 2.f, 0.f, 0.f, 1.f);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "diffuse.G"), 1e-5f);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "specular.B"), 1e-5f);
}

TEST_F(ColorViewsRenderTest, GradeAlphaChangesOnlyAlpha)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-three-layers.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));

    KnobColor* gain = dynamic_cast<KnobColor*>(grade->getKnobByName("white").get());
    ASSERT_TRUE(gain != NULL);
    gain->setValues(2., 2., 2., 2., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setLayer(0, kNatronColorViewAlpha, NULL);

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 2.f);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "diffuse.G"), 1e-5f);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "specular.B"), 1e-5f);
}

TEST_F(ColorViewsRenderTest, WriteAllWritesEachColourChannelOnce)
{
    NodePtr reader = createReader("flat-three-layers.exr");
    ASSERT_TRUE(bool(reader));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(reader, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    const char* colourChannels[] = { "R", "G", "B", "A" };
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(1, countChannel(image, colourChannels[i])) << colourChannels[i];
    }
    for (std::size_t i = 0; i < image.channels.size(); ++i) {
        const std::string& name = image.channels[i];
        const std::string::size_type dot = name.find('.');
        if (dot != std::string::npos) {
            EXPECT_FALSE(ImageLayerDesc::isColorViewID(name.substr(0, dot))) << name;
        }
    }
    expectColor(image, 1.f, 0.f, 0.f, 1.f);
}

TEST_F(ColorViewsRenderTest, WriteRgbViewWritesRgbOnly)
{
    NodePtr reader = createReader("flat-three-layers.exr");
    ASSERT_TRUE(bool(reader));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(reader, [](KnobChannelSet* channels) { channels->setLayer(0, kNatronColorViewRGB, NULL); }, &image, &error)) << error;

    const std::vector<std::string> expected = { "B", "G", "R" };
    std::vector<std::string> written = image.channels;
    std::sort(written.begin(), written.end());
    EXPECT_EQ(expected, written);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "R"), 1e-5f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "G"), 1e-5f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "B"), 1e-5f);
}

// Offset 0.25 on alpha shows what alpha the Grade read: 0.25 from the zero alpha an RGB stream
// reads, 1.25 from an alpha of 1.
TEST_F(ColorViewsRenderTest, GradeRgbaOverRgbWidensAndReadsAlphaAsZero)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-rgb-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));
    EffectInstancePtr effect = grade->getEffectInstance();

    EXPECT_EQ(3, effect->getMetadataNComps(-1)) << "the default row (rgba R, G, B) writes no missing channel";

    KnobColor* offset = dynamic_cast<KnobColor*>(grade->getKnobByName("offset").get());
    ASSERT_TRUE(offset != NULL);
    offset->setValues(0., 0., 0., 0.25, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setLayer(0, kNatronColorViewRGBA, NULL);

    EXPECT_EQ(4, effect->getMetadataNComps(-1));
    EXPECT_EQ(4, effect->getMetadataNComps(0));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    ASSERT_GE(image.channelIndex("A"), 0) << "the output kept the input's RGB layout";
    expectColor(image, 1.f, 0.f, 0.f, 0.25f);
}

TEST_F(ColorViewsRenderTest, GradeAlphaOverRgbCopiesRgbAndGradesAlphaFromZero)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-rgb-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));

    KnobColor* offset = dynamic_cast<KnobColor*>(grade->getKnobByName("offset").get());
    ASSERT_TRUE(offset != NULL);
    offset->setValues(0.25, 0.25, 0.25, 0.25, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setLayer(0, kNatronColorViewAlpha, NULL);

    EXPECT_EQ(4, grade->getEffectInstance()->getMetadataNComps(-1));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 0.25f);
    EXPECT_EQ(1.f, image.at(kCheckX, kCheckY, "R"));
    EXPECT_EQ(0.f, image.at(kCheckX, kCheckY, "G"));
    EXPECT_EQ(0.f, image.at(kCheckX, kCheckY, "B"));
}

TEST_F(ColorViewsRenderTest, BlurOnAllOverRgbDoesNotWiden)
{
    KnobChannelSetPtr channels;
    NodePtr blur = createEffectOnReader(kBlurPluginID, "flat-rgb-only.exr", &channels);
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(bool(channels));

    std::vector<ChannelSetRow> rows = channels->getRows();
    ASSERT_EQ(1u, rows.size());
    ASSERT_EQ(ChannelSetRow::eModeAll, rows[0].mode);

    EffectInstancePtr effect = blur->getEffectInstance();
    EXPECT_EQ(3, effect->getMetadataNComps(-1));
    EXPECT_EQ(3, effect->getMetadataNComps(0));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(blur, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    EXPECT_LT(image.channelIndex("A"), 0) << "an All row gave an RGB stream an alpha";
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "R"), 1e-5f);
}

// A Grade on rgba over RGB with no clamp and default values is an identity, so the render forwards
// the widened RGBA request straight to the reader instead of running the Grade: the alpha the RGB
// stream lacks must still read zero, as it does when the Grade processes the pixels.
TEST_F(ColorViewsRenderTest, IdentityGradeRgbaOverRgbReadsAlphaAsZero)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-rgb-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));
    KnobBool* clampBlack = dynamic_cast<KnobBool*>(grade->getKnobByName("clampBlack").get());
    ASSERT_TRUE(clampBlack != NULL);
    clampBlack->setValue(false);
    channels->setLayer(0, kNatronColorViewRGBA, NULL);

    EffectInstancePtr effect = grade->getEffectInstance();
    EXPECT_EQ(4, effect->getMetadataNComps(-1));

    double identityTime = 0.;
    ViewIdx identityView(0);
    int identityInputNb = -1;
    const RectI window(0, 0, 4, 4);
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), 1, RenderScale::identity, window, ViewIdx(0), &identityTime, &identityView, &identityInputNb));
    EXPECT_EQ(0, identityInputNb);

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    ASSERT_GE(image.channelIndex("A"), 0) << "the output kept the input's RGB layout";
    expectColor(image, 1.f, 0.f, 0.f, 0.f);
}

// Merge's A over B is A + B * (1 - A.alpha). With B = (1, 0, 0, 1), an A input read as
// (1, 0, 0, 0) gives (2, 0, 0, 1); an alpha of 1 on A would give (1, 0, 0, 1).
TEST_F(ColorViewsRenderTest, MergeReadsAlphaOfRgbAInputAsZero)
{
    NodePtr rgba = createReader("flat-three-layers.exr");
    NodePtr rgb = createReader("flat-rgb-only.exr");
    ASSERT_TRUE(rgba && rgb);

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
    connectNodes(rgba, merge, inputB, true);
    connectNodes(rgb, merge, inputA, true);

    EXPECT_EQ(4, merge->getEffectInstance()->getMetadataNComps(-1));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(merge, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    expectColor(image, 2.f, 0.f, 0.f, 1.f);
}

// An alpha-only A input over B = (1, 0, 0, 1): A's alpha of 1 hides B entirely, so the result is
// A's R, G and B, which read zero. Replicating A's alpha into them would give (1, 1, 1, 1).
TEST_F(ColorViewsRenderTest, MergeReadsRgbOfAlphaOnlyAInputAsZero)
{
    NodePtr rgba = createReader("flat-three-layers.exr");
    NodePtr alpha = createReader("flat-alpha-only.exr");
    ASSERT_TRUE(rgba && alpha);

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
    connectNodes(rgba, merge, inputB, true);
    connectNodes(alpha, merge, inputA, true);

    EXPECT_EQ(4, merge->getEffectInstance()->getMetadataNComps(-1));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(merge, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    expectColor(image, 0.f, 0.f, 0.f, 1.f);
}

TEST_F(ColorViewsRenderTest, ListLayerViewsForKnobOnAlphaOnlyInputListsRgbaRgbAlpha)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-alpha-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));

    std::list<ImageLayerDesc> views;
    grade->listLayerViewsForKnob(grade->getLayerKnob(), 1, ViewIdx(0), &views);

    std::vector<std::string> ids;
    for (std::list<ImageLayerDesc>::const_iterator it = views.begin(); it != views.end(); ++it) {
        ids.push_back(it->getLayerID());
    }
    const std::vector<std::string> expected = { kNatronColorViewRGBA, kNatronColorViewRGB, kNatronColorViewAlpha };
    EXPECT_EQ(expected, ids) << layerIDsString(views);

    std::list<ImageLayerDesc> storage;
    grade->listLayersForKnob(grade->getLayerKnob(), 1, ViewIdx(0), &storage);
    ASSERT_EQ(1u, storage.size()) << layerIDsString(storage);
    EXPECT_EQ(std::string(kNatronColorLayerID), storage.front().getLayerID());
}

// Grade's rgb view writes bits {0, 1, 2}, which is not a subset of the alpha-only storage's bit
// {3}, so checkMetadata widens the output to RGBA (widen-on-write): RGB
// is graded from a zero fill, and A passes through unchanged (the "Grade rgb over Alpha" example).
TEST_F(ColorViewsRenderTest, GradeRgbOverAlphaWidensAndGradesRgbFromZero)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-alpha-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));
    EffectInstancePtr effect = grade->getEffectInstance();

    KnobColor* offset = dynamic_cast<KnobColor*>(grade->getKnobByName("offset").get());
    ASSERT_TRUE(offset != NULL);
    offset->setValues(0.3, 0.3, 0.3, 0., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setLayer(0, kNatronColorViewRGB, NULL);

    EXPECT_EQ(4, effect->getMetadataNComps(-1));
    EXPECT_EQ(4, effect->getMetadataNComps(0));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    ASSERT_GE(image.channelIndex("A"), 0) << "the output widened to RGBA";
    expectColor(image, 0.3f, 0.3f, 0.3f, 1.f);
}

// Grade's clip declares Alpha support, so it renders an Alpha stream as Alpha
// directly, with no colour channel created: the output it advertises and stores stays Alpha,
// with A graded.
TEST_F(ColorViewsRenderTest, GradeAlphaOverAlphaStaysAlphaOnly)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-alpha-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));
    EffectInstancePtr effect = grade->getEffectInstance();

    KnobColor* gain = dynamic_cast<KnobColor*>(grade->getKnobByName("white").get());
    ASSERT_TRUE(gain != NULL);
    gain->setValues(2., 2., 2., 2., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setLayer(0, kNatronColorViewAlpha, NULL);

    EXPECT_EQ(1, effect->getMetadataNComps(-1));
    EXPECT_EQ(1, effect->getMetadataNComps(0)) << "the Alpha stream reaches Grade as Alpha, since Grade's clip now declares Alpha support";

    std::list<ImageLayerDesc> present;
    effect->getPresentLayers(1, ViewIdx(0), -1, &present);
    bool foundColor = false;
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (it->isColorLayer()) {
            foundColor = true;
            EXPECT_EQ(1, it->getNumComponents()) << layerIDsString(present);
        }
    }
    EXPECT_TRUE(foundColor) << layerIDsString(present);

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    EXPECT_LT(image.channelIndex("R"), 0) << "Grade on alpha created R";
    EXPECT_LT(image.channelIndex("G"), 0) << "Grade on alpha created G";
    EXPECT_LT(image.channelIndex("B"), 0) << "Grade on alpha created B";
    ASSERT_GE(image.channelIndex("A"), 0);
    EXPECT_NEAR(2.f, image.at(kCheckX, kCheckY, "A"), 1e-5f);
}

TEST_F(ColorViewsRenderTest, BlurOnAllOverAlphaStaysAlphaOnly)
{
    KnobChannelSetPtr channels;
    NodePtr blur = createEffectOnReader(kBlurPluginID, "flat-alpha-only.exr", &channels);
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(bool(channels));

    std::vector<ChannelSetRow> rows = channels->getRows();
    ASSERT_EQ(1u, rows.size());
    ASSERT_EQ(ChannelSetRow::eModeAll, rows[0].mode);

    EXPECT_EQ(1, blur->getEffectInstance()->getMetadataNComps(-1));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(blur, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    EXPECT_LT(image.channelIndex("R"), 0) << "an All row gave an Alpha stream an R";
    ASSERT_GE(image.channelIndex("A"), 0);
    EXPECT_EQ(1.f, image.at(kCheckX, kCheckY, "A"));
}

// The default row writes R, G and B, which the Alpha stream lacks, so the output widens to RGBA:
// RGB are graded from zero (zero again at default values) and the unwritten A is the input's.
TEST_F(ColorViewsRenderTest, DefaultGradeOverAlphaKeepsAlphaExactly)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-alpha-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));
    EffectInstancePtr effect = grade->getEffectInstance();

    EXPECT_EQ(4, effect->getMetadataNComps(-1));
    EXPECT_EQ(4, effect->getMetadataNComps(0));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    ASSERT_GE(image.channelIndex("A"), 0);
    EXPECT_EQ(1.f, image.at(kCheckX, kCheckY, "A"));
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "R"), 1e-5f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "G"), 1e-5f);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "B"), 1e-5f);
}

// None of this suite's EXR fixtures carry a base layer with exactly two channels that aren't an
// I+A (luminance+alpha) pair, so a Read never reports
// ePixelComponentXY for them: there is no genuine 2-channel XY colour clip to render here. This
// instead exercises the exact composition Node::listLayerViewsForKnob uses,
// expandColorViews(listLayersForKnob(...)), directly on an XY storage desc, at the engine level.
TEST_F(ColorViewsRenderTest, ExpandColorViewsOnXYStorageListsAllFourViews)
{
    std::list<ImageLayerDesc> storage;
    storage.push_back(ImageLayerDesc::getXYComponents());

    ImageLayerDesc::expandColorViews(&storage);

    std::vector<std::string> ids;
    for (std::list<ImageLayerDesc>::const_iterator it = storage.begin(); it != storage.end(); ++it) {
        ids.push_back(it->getLayerID());
    }
    const std::vector<std::string> expected = { kNatronColorViewRGBA, kNatronColorViewRGB, kNatronColorViewAlpha, kNatronColorViewXY };
    EXPECT_EQ(expected, ids) << layerIDsString(storage);
}

// The storage plane's raw ID/label (kNatronColorLayerID / kNatronColorStorageLabel, "Color")
// must never reach a surface the user reads: the registry snapshot, listLayerViewsForKnob, the
// three colour-naming knobs' summaries at their defaults, Shuffle's sublabel, and Python's
// Effect.getAvailableLayers(). Every one of these must speak in views (rgba/rgb/alpha/xy).
TEST_F(ColorViewsRenderTest, NoUserFacingColor)
{
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = getApp()->getProject()->getLayerRegistrySnapshot();
    for (std::vector<LayerRegistryEntry>::const_iterator it = snapshot->begin(); it != snapshot->end(); ++it) {
        EXPECT_NE(std::string(kNatronColorLayerID), it->desc.getLayerID());
        expectNoUserFacingColor(it->desc.getLayerLabel());
    }

    const char* fixtures[] = { "flat-three-layers.exr", "flat-rgb-only.exr" };
    for (std::size_t f = 0; f < sizeof(fixtures) / sizeof(fixtures[0]); ++f) {
        SCOPED_TRACE(fixtures[f]);

        KnobChannelSetPtr readerChannels;
        NodePtr grade = createEffectOnReader(kGradePluginID, fixtures[f], &readerChannels);
        ASSERT_TRUE(bool(grade));

        std::list<ImageLayerDesc> views;
        grade->listLayerViewsForKnob(grade->getLayerKnob(), 1, ViewIdx(0), &views);
        EXPECT_FALSE(views.empty());
        for (std::list<ImageLayerDesc>::const_iterator it = views.begin(); it != views.end(); ++it) {
            EXPECT_NE(std::string(kNatronColorLayerID), it->getLayerID());
            expectNoUserFacingColor(it->getLayerLabel());
        }
    }

    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    NodePtr groupNode = getApp()->createNode(groupArgs);
    ASSERT_TRUE(bool(groupNode));
    EffectInstancePtr groupEffect = groupNode->getEffectInstance();
    ASSERT_TRUE(bool(groupEffect));

    KnobChannelSetPtr channelSet = groupEffect->createChannelSetKnob("noUserFacingColorChannelSet", "ChannelSet");
    ASSERT_TRUE(bool(channelSet));
    expectNoUserFacingColor(channelSet->getSummary());

    KnobLayerSelectPtr layerSelect = groupEffect->createLayerSelectKnob("noUserFacingColorLayerSelect", "LayerSelect", false);
    ASSERT_TRUE(bool(layerSelect));
    expectNoUserFacingColor(layerSelect->getSummary());

    KnobChannelSelectPtr channelSelect = groupEffect->createChannelSelectKnob("noUserFacingColorChannelSelect", "ChannelSelect");
    ASSERT_TRUE(bool(channelSelect));
    expectNoUserFacingColor(channelSelect->getSummary());

    NodePtr reader = createReader("flat-three-layers.exr");
    ASSERT_TRUE(bool(reader));
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(bool(shuffle));
    connectNodes(reader, shuffle, Shuffle::eInputMain, true);

    KnobLayerSelectPtr in2 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(kShuffleParamIn2));
    KnobLayerSelectPtr out2 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(kShuffleParamOut2));
    ASSERT_TRUE(bool(in2));
    ASSERT_TRUE(bool(out2));
    in2->setLayer("specular");
    out2->setLayer("diffuse");

    KnobStringPtr sublabel = std::dynamic_pointer_cast<KnobString>(shuffle->getKnobByName(kNatronOfxParamStringSublabelName));
    ASSERT_TRUE(bool(sublabel));
    expectNoUserFacingColor(sublabel->getValue());

    Natron::Python::Effect effect(shuffle);
    std::list<Natron::Python::ImageLayer> available = effect.getAvailableLayers(-1);
    EXPECT_FALSE(available.empty());
    for (std::list<Natron::Python::ImageLayer>::const_iterator it = available.begin(); it != available.end(); ++it) {
        expectNoUserFacingColor(it->getLayerName().toStdString());
    }
}

// The viewer's info bar (Image::getFormatString()) and a node's Info tab read the image or the
// layer list at the storage level, so each names the colour storage plane after the view whose
// channels it carries: an RGB-only stream reads rgb.RGB, not rgba.
TEST_F(ColorViewsRenderTest, InfoTextsNameTheStorageView)
{
    EXPECT_EQ(std::string("rgba.RGBA32f"), Image::getFormatString(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat));
    EXPECT_EQ(std::string("rgb.RGB16f"), Image::getFormatString(ImageLayerDesc::getRGBComponents(), eImageBitDepthHalf));
    EXPECT_EQ(std::string("alpha.Alpha8u"), Image::getFormatString(ImageLayerDesc::getAlphaComponents(), eImageBitDepthByte));
    EXPECT_EQ(std::string("xy.XY32f"), Image::getFormatString(ImageLayerDesc::getXYComponents(), eImageBitDepthFloat));

    const std::vector<std::string> z(1, "Z");
    const ImageLayerDesc depth("depth", "depth", "", z);
    EXPECT_EQ(std::string("depth.Z32f"), Image::getFormatString(depth, eImageBitDepthFloat));
    EXPECT_EQ(std::string(kNatronColorViewAlpha), ImageLayerDesc::getColorView(kNatronColorViewAlpha).getUserFacingLabel());

    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-rgb-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    KnobButtonPtr refreshInfo = std::dynamic_pointer_cast<KnobButton>(grade->getKnobByName("refreshButton"));
    KnobStringPtr nodeInfos = std::dynamic_pointer_cast<KnobString>(grade->getKnobByName("nodeInfos"));
    ASSERT_TRUE(bool(refreshInfo));
    ASSERT_TRUE(bool(nodeInfos));

    refreshInfo->trigger();
    const std::string text = nodeInfos->getValue();
    EXPECT_NE(std::string::npos, text.find("rgb.RGB")) << text;
    expectNoUserFacingColor(text);
}

// IDistort's channelU/V/A pickers are Natron's hostChannelU/V/A twins, which push the SupportExt
// library's own option ID into the hidden plugin menu: every colour view shares that library's
// single colour plane, so rgba.A arrives as kNatronColorLayerID + ".A".
TEST_F(ColorViewsRenderTest, IDistortReadsAMissingUVAlphaChannelAsZero)
{
    NodePtr source = createReader("flat-three-layers.exr");
    NodePtr uv = createReader("flat-rgb-only.exr");
    ASSERT_TRUE(bool(source) && bool(uv));

    NodePtr idistort = createNode(QString::fromUtf8(kIDistortPluginID));
    ASSERT_TRUE(bool(idistort));

    int sourceInput = -1;
    int uvInput = -1;
    for (int i = 0; i < idistort->getNInputs(); ++i) {
        const std::string label = idistort->getInputLabel(i);
        if (label == "Source") {
            sourceInput = i;
        } else if (label == "UV") {
            uvInput = i;
        }
    }
    ASSERT_GE(sourceInput, 0);
    ASSERT_GE(uvInput, 0);
    connectNodes(source, idistort, sourceInput, true);
    connectNodes(uv, idistort, uvInput, true);

    KnobChannelSelectPtr hostChannelA = std::dynamic_pointer_cast<KnobChannelSelect>(idistort->getKnobByName("hostChannelA"));
    ASSERT_TRUE(bool(hostChannelA));
    KnobChoice* channelA = dynamic_cast<KnobChoice*>(idistort->getKnobByName("channelA").get());
    ASSERT_TRUE(channelA != NULL);
    hostChannelA->set(std::string(kNatronColorViewRGBA) + ".R");
    EXPECT_EQ(std::string(kNatronColorLayerID) + ".R", channelA->getActiveEntry().id);
    hostChannelA->set(std::string(kNatronColorViewRGBA) + ".A");
    EXPECT_EQ(std::string(kNatronColorLayerID) + ".A", channelA->getActiveEntry().id);

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(idistort, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    ASSERT_GE(image.channelIndex("A"), 0);
    EXPECT_NEAR(0.f, image.at(kCheckX, kCheckY, "A"), 1e-5f);
}

// With every UV channel None, IDistort is handed its "not used" constants -- no offset and an
// opaque alpha -- so it leaves the source as it was.
TEST_F(ColorViewsRenderTest, IDistortWithNoneUVChannelsPassesTheSourceThrough)
{
    NodePtr source = createReader("flat-three-layers.exr");
    NodePtr uv = createReader("flat-rgb-only.exr");
    ASSERT_TRUE(bool(source) && bool(uv));

    NodePtr idistort = createNode(QString::fromUtf8(kIDistortPluginID));
    ASSERT_TRUE(bool(idistort));

    int sourceInput = -1;
    int uvInput = -1;
    for (int i = 0; i < idistort->getNInputs(); ++i) {
        const std::string label = idistort->getInputLabel(i);
        if (label == "Source") {
            sourceInput = i;
        } else if (label == "UV") {
            uvInput = i;
        }
    }
    ASSERT_GE(sourceInput, 0);
    ASSERT_GE(uvInput, 0);
    connectNodes(source, idistort, sourceInput, true);
    connectNodes(uv, idistort, uvInput, true);

    const char* const twins[] = { "hostChannelU", "hostChannelV", "hostChannelA" };
    for (std::size_t i = 0; i < sizeof(twins) / sizeof(twins[0]); ++i) {
        KnobChannelSelectPtr twin = std::dynamic_pointer_cast<KnobChannelSelect>(idistort->getKnobByName(twins[i]));
        ASSERT_TRUE(bool(twin)) << twins[i];
        twin->setNone();
    }

    FlatExrImage expected;
    std::string error;
    ASSERT_TRUE(render(source, &ColorViewsRenderTest::writeAll, &expected, &error)) << error;
    FlatExrImage image;
    ASSERT_TRUE(render(idistort, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    const char* const channels[] = { "R", "G", "B", "A" };
    for (std::size_t c = 0; c < sizeof(channels) / sizeof(channels[0]); ++c) {
        ASSERT_GE(expected.channelIndex(channels[c]), 0) << channels[c];
        ASSERT_GE(image.channelIndex(channels[c]), 0) << channels[c];
        EXPECT_NEAR(expected.at(kCheckX, kCheckY, channels[c]), image.at(kCheckX, kCheckY, channels[c]), 1e-5f) << channels[c];
    }
}

// Grade's own clip preferences declare Alpha support directly (rather than relying only on
// the host narrowing an internally-widened RGBA render back down), so an alpha-only stream's
// single channel round-trips through the file itself, not just through the metadata Natron
// derives from it.
TEST_F(ColorViewsRenderTest, GradeAlphaOnlyGainDoublesTheSoleChannel)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(kGradePluginID, "flat-alpha-only.exr", &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));
    channels->setLayer(0, kNatronColorViewAlpha, NULL);

    KnobColor* gain = dynamic_cast<KnobColor*>(grade->getKnobByName("white").get());
    ASSERT_TRUE(gain != NULL);
    gain->setValues(2., 2., 2., 2., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    ASSERT_EQ(1u, image.channels.size());
    EXPECT_EQ(std::string("A"), image.channels.front());
    EXPECT_NEAR(2.f, image.at(kCheckX, kCheckY, "A"), 1e-5f);
}

// Premult multiplies by the alpha it reads from its own picked channel; on this fixture that
// channel is the image's only channel, always 1, so premultiplying (or unpremultiplying) by it
// is a no-op, provided getClipPreferences keeps the stream alpha-only rather than requesting RGBA.
TEST_F(ColorViewsRenderTest, PremultOnAlphaOnlyLeavesAlphaUnchanged)
{
    NodePtr reader = createReader("flat-alpha-only.exr");
    ASSERT_TRUE(bool(reader));
    NodePtr premult = createNode(QString::fromUtf8(kPremultPluginID));
    ASSERT_TRUE(bool(premult));
    connectNodes(reader, premult, 0, true);

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(premult, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    ASSERT_EQ(1u, image.channels.size());
    EXPECT_EQ(std::string("A"), image.channels.front());
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "A"), 1e-5f);
}

// WritePNG declares Alpha support, so an alpha-only input is written as a one-channel PNG instead
// of being widened. PNG has no alpha-only type: the file is a grey image, which a Read gives a black
// colour plane like any file without R, G, B or A, so the check is on the file itself.
TEST_F(ColorViewsRenderTest, AlphaOnlyInputWritesAOneChannelPng)
{
    NodePtr reader = createReader("flat-alpha-only.exr");
    ASSERT_TRUE(bool(reader));

    NodePtr writePng = createNode(QString::fromUtf8(kWritePNGPluginID));
    ASSERT_TRUE(bool(writePng));
    connectNodes(reader, writePng, 0, true);

    KnobChannelSet* writePngChannels = dynamic_cast<KnobChannelSet*>(writePng->getKnobByName(kNodeParamChannelSet).get());
    ASSERT_TRUE(writePngChannels != NULL);
    writePngChannels->setAll();
    KnobChoice* pngBitDepth = dynamic_cast<KnobChoice*>(writePng->getKnobByName("bitDepth").get());
    ASSERT_TRUE(pngBitDepth != NULL);
    pngBitDepth->setValueFromID("8u", 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string pngPath = (tmp.path() + QLatin1String("/alpha.png")).toStdString();
    writePng->setOutputFilesForWriter(pngPath);

    OutputEffectInstance* writePngEffect = dynamic_cast<OutputEffectInstance*>(writePng->getEffectInstance().get());
    ASSERT_TRUE(writePngEffect != NULL);
    std::list<AppInstance::RenderWork> writeWorks;
    writeWorks.push_back(AppInstance::RenderWork(writePngEffect, 1, 1, 1, false));
    getApp()->startWritersRendering(false, writeWorks);
    ASSERT_TRUE(QFile::exists(QString::fromStdString(pngPath))) << pngPath;

    OIIO::ImageInput::unique_ptr png = OIIO::ImageInput::open(pngPath);
    ASSERT_TRUE(bool(png)) << OIIO::geterror();
    const OIIO::ImageSpec& spec = png->spec();
    ASSERT_EQ(1, spec.nchannels);
    std::vector<float> pixels((std::size_t)spec.width * (std::size_t)spec.height);
    ASSERT_TRUE(png->read_image(0, 0, 0, 1, OIIO::TypeDesc::FLOAT, pixels.data())) << png->geterror();
    ASSERT_GT(spec.height, kCheckY);
    ASSERT_GT(spec.width, kCheckX);
    // PNG rows run top-down.
    EXPECT_NEAR(1.f, pixels[(std::size_t)(spec.height - 1 - kCheckY) * (std::size_t)spec.width + (std::size_t)kCheckX], 1e-4f);
}

// A single-A file is decoded straight into a 1-component buffer instead of being widened to RGBA
// and narrowed back.
TEST_F(ColorViewsRenderTest, ReadDecodesAlphaOnlyFileIntoOneChannel)
{
    NodePtr reader = createReader("flat-alpha-only.exr");
    ASSERT_TRUE(bool(reader));

    ImageLayerDesc layer;
    ImageLayerDesc paired;
    reader->getEffectInstance()->getMetadataComponents(-1, &layer, &paired);
    EXPECT_EQ(1u, layer.getChannels().size());

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(reader, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    ASSERT_EQ(1u, image.channels.size());
    EXPECT_EQ(std::string("A"), image.channels.front());
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "A"), 1e-5f);
}

TEST_F(ColorViewsRenderTest, ReadDecodesRgbaFileIntoFourChannels)
{
    NodePtr reader = createReader("flat-rgba-only.exr");
    ASSERT_TRUE(bool(reader));

    ImageLayerDesc layer;
    ImageLayerDesc paired;
    reader->getEffectInstance()->getMetadataComponents(-1, &layer, &paired);
    EXPECT_EQ(4u, layer.getChannels().size());

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(reader, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    ASSERT_EQ(4u, image.channels.size());
    EXPECT_GE(image.channelIndex("R"), 0);
    EXPECT_GE(image.channelIndex("G"), 0);
    EXPECT_GE(image.channelIndex("B"), 0);
    EXPECT_GE(image.channelIndex("A"), 0);
    EXPECT_NEAR(1.f, image.at(kCheckX, kCheckY, "A"), 1e-5f);
}

// Both fixtures put a 6x4 data window at file x = -1 inside a display window whose origin is
// (-2, -1); OpenEXR addresses scanline samples by absolute file x, so a reader ignoring either
// origin shifts the samples or writes past the row. In the RGB-and-A fixture R and G hold each sample's file x and y,
// so A must cover exactly the pixels whose R and G follow one consistent offset; B is absent
// there and reads 0, as does every pixel outside the data window.
TEST_F(ColorViewsRenderTest, ReadPlacesAnOffsetDataWindowAndZeroFillsTheRest)
{
    const char* const fixtures[] = { "flat-rgb-a-offset-window.exr", "flat-alpha-offset-window.exr" };
    for (std::size_t f = 0; f < sizeof(fixtures) / sizeof(fixtures[0]); ++f) {
        SCOPED_TRACE(fixtures[f]);

        NodePtr exrReader = createReader(fixtures[f]);
        ASSERT_TRUE(bool(exrReader));

        FlatExrImage exr;
        std::string error;
        ASSERT_TRUE(render(exrReader, &ColorViewsRenderTest::writeAll, &exr, &error)) << error;
        ASSERT_GE(exr.channelIndex("A"), 0);
        const bool encodesPosition = exr.channelIndex("R") >= 0;

        int covered = 0;
        bool haveOffset = false;
        int32_t offsetX = 0;
        int32_t offsetY = 0;
        std::vector<bool> seenX(6, false);
        std::vector<bool> seenY(4, false);
        for (int32_t y = exr.y1; y < exr.y1 + exr.height; ++y) {
            for (int32_t x = exr.x1; x < exr.x1 + exr.width; ++x) {
                const bool isCovered = exr.at(x, y, "A") > 0.f;
                if (isCovered) {
                    ++covered;
                }
                for (std::size_t c = 0; c < exr.channels.size(); ++c) {
                    const std::string& channel = exr.channels[c];
                    const float value = exr.at(x, y, channel);
                    if (!isCovered || (channel == "B")) {
                        EXPECT_EQ(0.f, value) << channel << " at (" << x << ", " << y << ")";
                    }
                }
                if (!encodesPosition || !isCovered) {
                    continue;
                }
                const int32_t fileX = static_cast<int32_t>(exr.at(x, y, "R"));
                const int32_t fileY = static_cast<int32_t>(exr.at(x, y, "G"));
                if (!haveOffset) {
                    offsetX = x - fileX;
                    offsetY = y - fileY;
                    haveOffset = true;
                }
                EXPECT_EQ(offsetX, x - fileX) << "at (" << x << ", " << y << ")";
                EXPECT_EQ(offsetY, y - fileY) << "at (" << x << ", " << y << ")";
                if ((fileX >= -1) && (fileX < 5) && (fileY >= 2) && (fileY < 6)) {
                    seenX[fileX + 1] = true;
                    seenY[fileY - 2] = true;
                }
            }
        }
        EXPECT_EQ(6 * 4, covered);
        if (encodesPosition) {
            EXPECT_EQ(std::vector<bool>(6, true), seenX);
            EXPECT_EQ(std::vector<bool>(4, true), seenY);
        }
    }
}

// The RGB-only counterpart of the offset fixtures: with no A channel in the file the Read has no
// A to place, and R, G and B land at one consistent offset. R and G hold each sample's file x and
// y, and B is 0.5 over the data window.
TEST_F(ColorViewsRenderTest, ReadPlacesAnOffsetRgbOnlyFile)
{
    NodePtr reader = createReader("flat-rgb-offset-window.exr");
    ASSERT_TRUE(bool(reader));

    FlatExrImage exr;
    std::string error;
    ASSERT_TRUE(render(reader, &ColorViewsRenderTest::writeAll, &exr, &error)) << error;
    ASSERT_EQ(3u, exr.channels.size());
    ASSERT_GE(exr.channelIndex("R"), 0);
    ASSERT_GE(exr.channelIndex("G"), 0);
    ASSERT_GE(exr.channelIndex("B"), 0);
    EXPECT_EQ(-1, exr.channelIndex("A"));

    int covered = 0;
    bool haveOffset = false;
    int32_t offsetX = 0;
    int32_t offsetY = 0;
    std::vector<bool> seenX(6, false);
    std::vector<bool> seenY(4, false);
    for (int32_t y = exr.y1; y < exr.y1 + exr.height; ++y) {
        for (int32_t x = exr.x1; x < exr.x1 + exr.width; ++x) {
            const bool isCovered = exr.at(x, y, "B") > 0.f;
            if (!isCovered) {
                EXPECT_EQ(0.f, exr.at(x, y, "R")) << "R at (" << x << ", " << y << ")";
                EXPECT_EQ(0.f, exr.at(x, y, "G")) << "G at (" << x << ", " << y << ")";
                continue;
            }
            ++covered;
            EXPECT_NEAR(0.5f, exr.at(x, y, "B"), 1e-5f) << "B at (" << x << ", " << y << ")";
            const int32_t fileX = static_cast<int32_t>(exr.at(x, y, "R"));
            const int32_t fileY = static_cast<int32_t>(exr.at(x, y, "G"));
            if (!haveOffset) {
                offsetX = x - fileX;
                offsetY = y - fileY;
                haveOffset = true;
            }
            EXPECT_EQ(offsetX, x - fileX) << "at (" << x << ", " << y << ")";
            EXPECT_EQ(offsetY, y - fileY) << "at (" << x << ", " << y << ")";
            if ((fileX >= -1) && (fileX < 5) && (fileY >= 2) && (fileY < 6)) {
                seenX[fileX + 1] = true;
                seenY[fileY - 2] = true;
            }
        }
    }
    EXPECT_EQ(6 * 4, covered);
    EXPECT_EQ(std::vector<bool>(6, true), seenX);
    EXPECT_EQ(std::vector<bool>(4, true), seenY);
}

// openfx-misc compiles TimeBuffer only into DEBUG builds, so release plug-in sets lack it.
static bool
timeBufferPluginsLoaded()
{
    bool loaded = false;
    try {
        const bool readLoaded = appPTR->getPluginBinary(QString::fromUtf8(kTimeBufferReadPluginID), -1, -1, false) != NULL;
        const bool writeLoaded = appPTR->getPluginBinary(QString::fromUtf8(kTimeBufferWritePluginID), -1, -1, false) != NULL;
        loaded = readLoaded && writeLoaded;
    } catch (const std::exception&) {
    }
    if (!loaded) {
        std::cout << "TimeBuffer plug-ins not loaded: openfx-misc builds them only with DEBUG defined, "
                     "so point OFX_PLUGIN_PATH at such a build to run this check"
                  << std::endl;
    }

    return loaded;
}

static int
inputIndexByLabel(const NodePtr& node,
                  const std::string& label)
{
    for (int i = 0; i < node->getNInputs(); ++i) {
        if (node->getInputLabel(i) == label) {
            return i;
        }
    }

    return -1;
}

void
ColorViewsRenderTest::createTimeBufferPair(const NodePtr& readSource,
                                           const NodePtr& writeSource,
                                           NodePtr* bufferRead,
                                           NodePtr* bufferWrite)
{
    *bufferRead = createNode(QString::fromUtf8(kTimeBufferReadPluginID));
    *bufferWrite = createNode(QString::fromUtf8(kTimeBufferWritePluginID));
    ASSERT_TRUE(bool(*bufferRead));
    ASSERT_TRUE(bool(*bufferWrite));

    const char* const bufferName = "m66AlphaBuffer";
    KnobString* readName = dynamic_cast<KnobString*>((*bufferRead)->getKnobByName("bufferName").get());
    KnobString* writeName = dynamic_cast<KnobString*>((*bufferWrite)->getKnobByName("bufferName").get());
    KnobInt* startFrame = dynamic_cast<KnobInt*>((*bufferRead)->getKnobByName("startFrame").get());
    ASSERT_TRUE(readName != NULL);
    ASSERT_TRUE(writeName != NULL);
    ASSERT_TRUE(startFrame != NULL);
    readName->setValue(std::string(bufferName));
    writeName->setValue(std::string(bufferName));
    startFrame->setValue(1);
    // TimeBufferRead also has a Generator context, so Natron gives it a target layer, and the
    // default rgba target would widen the alpha-only stream it hands back.
    KnobLayerSelectPtr readLayer = std::dynamic_pointer_cast<KnobLayerSelect>((*bufferRead)->getLayerKnob());
    ASSERT_TRUE(bool(readLayer));
    readLayer->setLayer(kNatronColorViewAlpha);

    const int readSourceInput = inputIndexByLabel(*bufferRead, "Source");
    const int writeSourceInput = inputIndexByLabel(*bufferWrite, "Source");
    const int writeSyncInput = inputIndexByLabel(*bufferWrite, "Sync");
    ASSERT_GE(readSourceInput, 0);
    ASSERT_GE(writeSourceInput, 0);
    ASSERT_GE(writeSyncInput, 0);
    connectNodes(readSource, *bufferRead, readSourceInput, true);
    connectNodes(*bufferRead, *bufferWrite, writeSyncInput, true);
    connectNodes(writeSource, *bufferWrite, writeSourceInput, true);
}

// TimeBufferWrite stores the layout of the stream it is fed and TimeBufferRead hands it back at
// the next frame, so an alpha-only stream must stay alpha-only through both nodes.
TEST_F(ColorViewsRenderTest, TimeBufferCarriesAnAlphaOnlyStreamToTheNextFrame)
{
    if (!timeBufferPluginsLoaded()) {
        return;
    }
    NodePtr reader = createReader("flat-alpha-pattern.exr");
    ASSERT_TRUE(bool(reader));
    NodePtr bufferRead;
    NodePtr bufferWrite;
    createTimeBufferPair(reader, reader, &bufferRead, &bufferWrite);
    ASSERT_FALSE(HasFatalFailure());

    EXPECT_TRUE(bufferRead->isSupportedComponent(0, ImageLayerDesc::getAlphaComponents()));
    EXPECT_TRUE(bufferRead->isSupportedComponent(-1, ImageLayerDesc::getAlphaComponents()));
    EXPECT_TRUE(bufferWrite->isSupportedComponent(0, ImageLayerDesc::getAlphaComponents()));
    EXPECT_TRUE(bufferWrite->isSupportedComponent(-1, ImageLayerDesc::getAlphaComponents()));
    EXPECT_EQ(1, bufferRead->getEffectInstance()->getMetadataNComps(0));
    EXPECT_EQ(1, bufferRead->getEffectInstance()->getMetadataNComps(-1));
    EXPECT_EQ(1, bufferWrite->getEffectInstance()->getMetadataNComps(0));
    EXPECT_EQ(1, bufferWrite->getEffectInstance()->getMetadataNComps(-1));

    FlatExrImage frame1;
    std::string error;
    ASSERT_TRUE(render(bufferWrite, &ColorViewsRenderTest::writeAll, &frame1, &error, 1)) << error;
    ASSERT_EQ(1u, frame1.channels.size());
    EXPECT_EQ(std::string("A"), frame1.channels.front());

    FlatExrImage frame2;
    ASSERT_TRUE(render(bufferRead, &ColorViewsRenderTest::writeAll, &frame2, &error, 2)) << error;
    ASSERT_EQ(1u, frame2.channels.size());
    EXPECT_EQ(std::string("A"), frame2.channels.front());
    ASSERT_EQ(frame1.width, frame2.width);
    ASSERT_EQ(frame1.height, frame2.height);
    for (int32_t y = 0; y < frame1.height; ++y) {
        for (int32_t x = 0; x < frame1.width; ++x) {
            EXPECT_NEAR(frame1.at(frame1.x1 + x, frame1.y1 + y, "A"),
                        frame2.at(frame2.x1 + x, frame2.y1 + y, "A"), 1e-5f)
                << "(" << x << ", " << y << ")";
        }
    }
}

// TimeBufferRead's output takes the layout of its Source input, so a buffer written from an RGBA
// stream cannot be handed back through an alpha-only Source; the read fails with an error
// rather than reinterpreting four channels as one.
TEST_F(ColorViewsRenderTest, TimeBufferReadFailsWhenTheBufferLayoutDiffersFromItsSource)
{
    if (!timeBufferPluginsLoaded()) {
        return;
    }
    NodePtr alphaReader = createReader("flat-alpha-pattern.exr");
    NodePtr rgbaReader = createReader("flat-rgba-pattern.exr");
    ASSERT_TRUE(bool(alphaReader));
    ASSERT_TRUE(bool(rgbaReader));
    NodePtr bufferRead;
    NodePtr bufferWrite;
    createTimeBufferPair(alphaReader, rgbaReader, &bufferRead, &bufferWrite);
    ASSERT_FALSE(HasFatalFailure());

    EXPECT_EQ(1, bufferRead->getEffectInstance()->getMetadataNComps(-1));
    EXPECT_EQ(4, bufferWrite->getEffectInstance()->getMetadataNComps(-1));

    FlatExrImage frame1;
    std::string error;
    ASSERT_TRUE(render(bufferWrite, &ColorViewsRenderTest::writeAll, &frame1, &error, 1)) << error;
    EXPECT_EQ(4u, frame1.channels.size());

    FlatExrImage frame2;
    EXPECT_FALSE(render(bufferRead, &ColorViewsRenderTest::writeAll, &frame2, &error, 2));
    ASSERT_TRUE(bufferRead->hasPersistentMessage());
    QString message;
    int type = 0;
    bufferRead->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("different pixel layout"))) << message.toStdString();
}

// A channel picker naming a colour channel resolves to an index into the layout the stream is
// fetched in: alpha.A is the only channel of an alpha-only stream, and rgba.R, which that stream
// lacks, reads as zero rather than as the stored alpha.
class ColorViewsChannelPickerRenderTest
    : public ColorViewsRenderTest {
protected:
    // flat-alpha-only.exr with its alpha graded down to 0.5, still alpha-only, into a Grade that
    // offsets R, G and B by 0.3 (widening the stream to RGBA with R = G = B = 0 read in) and is
    // (un)premultiplied by unPremultBy.
    NodePtr createOffsetOverHalvedAlpha(const std::string& unPremultBy)
    {
        KnobChannelSetPtr halveChannels;
        NodePtr halve = createEffectOnReader(kGradePluginID, "flat-alpha-only.exr", &halveChannels);
        if (!halve || !halveChannels) {
            return NodePtr();
        }
        halveChannels->setLayer(0, kNatronColorViewAlpha, NULL);
        KnobColor* white = dynamic_cast<KnobColor*>(halve->getKnobByName("white").get());
        if (!white) {
            return NodePtr();
        }
        white->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

        NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
        if (!grade) {
            return NodePtr();
        }
        connectNodes(halve, grade, 0, true);
        KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(grade->getKnobByName(kNodeParamChannelSet));
        KnobColor* offset = dynamic_cast<KnobColor*>(grade->getKnobByName("offset").get());
        KnobChannelSelectPtr divisor = grade->getUnPremultBySelector();
        if (!channels || !offset || !divisor) {
            return NodePtr();
        }
        channels->setLayer(0, kNatronColorViewRGB, NULL);
        offset->setValues(0.3, 0.3, 0.3, 0., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        divisor->set(unPremultBy);

        return grade;
    }

    // flat-three-layers.exr, Color (1, 0, 0, 1), into a Grade offsetting R, G and B by 0.3 and
    // masked by maskChannel of flat-alpha-only.exr.
    NodePtr createOffsetMaskedByAlphaOnly(const std::string& maskChannel)
    {
        KnobChannelSetPtr channels;
        NodePtr grade = createEffectOnReader(kGradePluginID, "flat-three-layers.exr", &channels);
        NodePtr mask = createReader("flat-alpha-only.exr");
        if (!grade || !channels || !mask) {
            return NodePtr();
        }
        channels->setLayer(0, kNatronColorViewRGB, NULL);
        KnobColor* offset = dynamic_cast<KnobColor*>(grade->getKnobByName("offset").get());
        if (!offset) {
            return NodePtr();
        }
        offset->setValues(0.3, 0.3, 0.3, 0., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

        int maskInput = -1;
        for (int i = 0; i < grade->getNInputs(); ++i) {
            if (grade->getInputLabel(i) == "Mask") {
                maskInput = i;
            }
        }
        if (maskInput < 0) {
            return NodePtr();
        }
        connectNodes(mask, grade, maskInput, true);

        KnobBool* maskEnabled = dynamic_cast<KnobBool*>(grade->getKnobByName("enableMask_Mask").get());
        KnobChannelSelect* maskSelect = dynamic_cast<KnobChannelSelect*>(grade->getKnobByName("maskChannel_Mask").get());
        if (!maskEnabled || !maskSelect) {
            return NodePtr();
        }
        maskEnabled->setValue(true);
        maskSelect->set(maskChannel);

        return grade;
    }
};

// R = 0.5 * (0 / 0.5 + 0.3) = 0.15; dividing by nothing would leave the plain offset, 0.3.
TEST_F(ColorViewsChannelPickerRenderTest, UnPremultByAlphaAOfAlphaOnlyStreamDividesByTheStoredAlpha)
{
    NodePtr grade = createOffsetOverHalvedAlpha(std::string(kNatronColorViewAlpha) + ".A");
    ASSERT_TRUE(bool(grade));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    expectColor(image, 0.15f, 0.15f, 0.15f, 0.5f);
}

// A zero divisor leaves the source undivided and multiplies the offset G and B back by zero;
// dividing by the stored alpha instead would give 0.15. R is the divisor channel itself, which
// is never divided or multiplied by itself (as A is not when unpremultiplying by A), so it keeps
// the plain offset.
TEST_F(ColorViewsChannelPickerRenderTest, UnPremultByMissingRgbaRReadsZeroNotTheStoredAlpha)
{
    NodePtr grade = createOffsetOverHalvedAlpha(std::string(kNatronColorViewRGBA) + ".R");
    ASSERT_TRUE(bool(grade));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    expectColor(image, 0.3f, 0.f, 0.f, 0.5f);
}

TEST_F(ColorViewsChannelPickerRenderTest, MaskByAlphaAOfAlphaOnlyStreamReadsTheStoredAlpha)
{
    NodePtr grade = createOffsetMaskedByAlphaOnly(std::string(kNatronColorViewAlpha) + ".A");
    ASSERT_TRUE(bool(grade));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    expectColor(image, 1.3f, 0.3f, 0.3f, 1.f);
}

TEST_F(ColorViewsChannelPickerRenderTest, MaskByMissingRgbaRReadsZeroNotTheStoredAlpha)
{
    NodePtr grade = createOffsetMaskedByAlphaOnly(std::string(kNatronColorViewRGBA) + ".R");
    ASSERT_TRUE(bool(grade));

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(render(grade, &ColorViewsRenderTest::writeAll, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
}

// flat-alpha-pattern.exr's alpha varies across the frame with no symmetry, so a geometric effect
// that misplaces coverage, or a stride or row-order error on the one-channel path, shows up
// against the same effect on flat-rgba-pattern.exr, which carries that alpha under RGB. The gray
// effects read the matte as intensity instead, so their reference is flat-rgba-gray-pattern.exr,
// the same values as R = G = B under an opaque A, and the R channel of their RGBA result.
TEST_F(ColorViewsRenderTest, MagickEffectsAcceptAlphaOnlyStreamsNatively)
{
    // mismatchBudget counts pixels allowed to differ from the RGBA path; random marks an effect whose output is randomly seeded on every run, so the test compares it pixel by pixel only through the changed/non-zero checks that follow.
    struct Row {
        const char* pluginID;
        bool matte;
        int mismatchBudget;
        bool random;
    };
    // On the 16x16 pattern ImageMagick's Charcoal, on a gray image, differs from the same image
    // read with an opaque alpha channel at exactly eight pixels: its edge and normalise steps see
    // that channel. The same call with the alpha channel switched off reproduces the alpha-only
    // output, so the difference is ImageMagick's, not a read or write error in the plug-in.
    const Row rows[] = {
        { "net.fxarena.openfx.Arc", true, 0, false },
        { "net.fxarena.openfx.Implode", true, 0, false },
        { "net.fxarena.openfx.Polar", true, 0, false },
        { "net.fxarena.openfx.Reflection", true, 0, false },
        { "net.fxarena.openfx.Tile", true, 0, false },
        { "net.fxarena.openfx.Charcoal", false, 8, false },
        { "net.fxarena.openfx.Sketch", false, 0, true },
        { "net.fxarena.openfx.Oilpaint", false, 0, false },
        { "net.fxarena.openfx.Edges", false, 0, false },
    };

    NodePtr sourceReader = createReader("flat-alpha-pattern.exr");
    ASSERT_TRUE(bool(sourceReader));
    FlatExrImage source;
    std::string error;
    ASSERT_TRUE(render(sourceReader, &ColorViewsRenderTest::writeAll, &source, &error)) << error;
    ASSERT_EQ(1u, source.channels.size());

    for (std::size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); ++r) {
        const char* pluginID = rows[r].pluginID;
        SCOPED_TRACE(pluginID);

        NodePtr alphaReader = createReader("flat-alpha-pattern.exr");
        ASSERT_TRUE(bool(alphaReader));
        NodePtr effect = createNode(QString::fromUtf8(pluginID));
        ASSERT_TRUE(bool(effect));
        connectNodes(alphaReader, effect, 0, true);

        EXPECT_TRUE(effect->isSupportedComponent(0, ImageLayerDesc::getAlphaComponents()));
        EXPECT_TRUE(effect->isSupportedComponent(-1, ImageLayerDesc::getAlphaComponents()));
        EXPECT_EQ(1, effect->getEffectInstance()->getMetadataNComps(0));
        EXPECT_EQ(1, effect->getEffectInstance()->getMetadataNComps(-1));

        FlatExrImage alphaImage;
        ASSERT_TRUE(render(effect, &ColorViewsRenderTest::writeAll, &alphaImage, &error)) << error;

        ASSERT_EQ(1u, alphaImage.channels.size());
        EXPECT_EQ(std::string("A"), alphaImage.channels.front());
        ASSERT_FALSE(alphaImage.pixels.empty());
        for (std::size_t i = 0; i < alphaImage.pixels.size(); ++i) {
            ASSERT_TRUE(std::isfinite(alphaImage.pixels[i])) << "pixel " << i;
        }

        NodePtr rgbaReader = createReader(rows[r].matte ? "flat-rgba-pattern.exr" : "flat-rgba-gray-pattern.exr");
        ASSERT_TRUE(bool(rgbaReader));
        NodePtr rgbaEffect = createNode(QString::fromUtf8(pluginID));
        ASSERT_TRUE(bool(rgbaEffect));
        connectNodes(rgbaReader, rgbaEffect, 0, true);

        FlatExrImage rgbaImage;
        ASSERT_TRUE(render(rgbaEffect, &ColorViewsRenderTest::writeAll, &rgbaImage, &error)) << error;

        const char* const referenceChannel = rows[r].matte ? "A" : "R";
        ASSERT_GE(rgbaImage.channelIndex(referenceChannel), 0);
        ASSERT_EQ(rgbaImage.x1, alphaImage.x1);
        ASSERT_EQ(rgbaImage.y1, alphaImage.y1);
        ASSERT_EQ(rgbaImage.width, alphaImage.width);
        ASSERT_EQ(rgbaImage.height, alphaImage.height);
        // The alpha-only path writes the intensity as it is, and Edges' brightness takes it above
        // 1, while the RGBA path composites it over opaque black, which clamps it to 1.
        int mismatches = 0;
        for (int32_t y = alphaImage.y1; y < alphaImage.y1 + alphaImage.height; ++y) {
            for (int32_t x = alphaImage.x1; x < alphaImage.x1 + alphaImage.width; ++x) {
                const float expected = rgbaImage.at(x, y, referenceChannel);
                float actual = alphaImage.at(x, y, "A");
                if (!rows[r].matte) {
                    actual = std::min(std::max(actual, 0.f), 1.f);
                }
                if (rows[r].random) {
                    continue;
                } else if (!(std::fabs(expected - actual) <= 1e-4f) && (++mismatches > rows[r].mismatchBudget)) {
                    ADD_FAILURE() << "pixel (" << x << ", " << y << "): RGBA path's " << referenceChannel << " " << expected << ", alpha-only path " << actual;
                }
            }
        }
        EXPECT_LE(mismatches, rows[r].mismatchBudget);

        if (!rows[r].matte) {
            bool changed = false;
            bool nonZero = false;
            for (int32_t y = alphaImage.y1; y < alphaImage.y1 + alphaImage.height; ++y) {
                for (int32_t x = alphaImage.x1; x < alphaImage.x1 + alphaImage.width; ++x) {
                    const float value = alphaImage.at(x, y, "A");
                    changed = changed || !(std::fabs(value - source.at(x, y, "A")) <= 1e-4f);
                    nonZero = nonZero || value != 0.f;
                }
            }
            EXPECT_TRUE(changed) << "the effect passed its input through unchanged";
            EXPECT_TRUE(nonZero) << "the effect produced an all-zero image";
        }
    }
}
