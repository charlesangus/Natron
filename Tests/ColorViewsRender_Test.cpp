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
#include <functional>
#include <list>
#include <memory>
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
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
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
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());

        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);

        return getApp()->createNode(readerArgs);
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
    // its channel set chosen by `setChannels`, renders frame 1 and parses the result.
    bool render(const NodePtr& node,
                const std::function<void(KnobChannelSet*)>& setChannels,
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
        works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
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
// {3}, so checkMetadata widens the output to RGBA per the design doc's widen-on-write rule: RGB
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

// None of this suite's EXR fixtures carry a base layer with exactly two channels that aren't an
// I+A (luminance+alpha) pair, so ReadOIIO's guessParamsFromFilename never reports
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
