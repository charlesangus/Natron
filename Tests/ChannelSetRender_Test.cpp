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

// Audit of every openfx-misc plugin declaring the standard NatronOfxParamProcessR/G/B/A quad,
// checking whether it reads _processR/G/B/A only to mask (write the computed value or pass the
// source channel through) or also to change its algorithm. Host masking (this fork forces the
// quad true and applies copyUnProcessedChannels per plane) is only equivalent to the plugin's
// own masking for the "mask only" plugins below.
// Add.cpp — mask only.
// Clamp.cpp — mask only.
// CImgFilter.h (shared base of the CImg.* plugins, incl. CImgBlur) — mask only for filters whose
//   per-channel math is independent (verified for CImgBlur's default Blur mode); it does reduce
//   the internal channel count passed to CImg when supportsComponentRemapping is set, so
//   CImg plugins with cross-channel algorithms (ChromaBlur, Sharpen, Bloom, EdgeDetect) are not
//   covered by this audit.
// ClipTest.cpp — beyond masking: the zebra-stripe decision ORs the out-of-range test across only
//   the channels marked processed, then paints every matched channel with the same stripe value;
//   forcing all four true changes which channels can trigger the stripe for channels that stay
//   selected. Real finding.
// ColorCorrect.cpp — mask only (luminance and the S/M/H recombination always read the pixel's raw
//   r/g/b; the process gate only decides whether a channel's result is written back).
// ColorMatrix.cpp — mask only (apply() always reads all four raw source components).
// CopyRectangle.cpp — mask only.
// DenoiseSharpen.cpp — beyond masking: R/G/B are collapsed into one combined "process color" flag
//   before filtering, so the plugin itself never processes R independently of G/B. Real finding.
// Grade.cpp — mask only.
// Gamma.cpp — mask only.
// FrameBlend.cpp — mask only.
// Distortion.cpp — mask only.
// Log2Lin.cpp — mask only.
// KeyMix.cpp — beyond masking: the quad IS the plugin's per-channel mix-source selector (clip A
//   vs. clip B), not a masking convenience. Real finding.
// Multiply.cpp — mask only.
// Premult.cpp — mask only (the alpha used to premultiply comes from a separate premult-channel
//   selection, not from _processA).
// PLogLin.cpp — mask only.
// Invert.cpp — mask only.
// Quantize.cpp — mask only.
// Radial.cpp — mask only (generator; the gate selects write vs. source passthrough).
// Roto.cpp — mask only (generator).
// Rectangle.cpp — mask only (generator).
// Saturation.cpp — mask only (luminance always reads the raw source r/g/b).
// Ramp.cpp — mask only (generator).
// Threshold.cpp — mask only.
// Templates/{MixableFilter,SimpleFilter,MaskableFilter}.cpp — example sources, not registered in
//   any CMakeLists of the bundle; not shipped plugins, excluded from this audit.

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

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
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>
#include <ofxNatron.h>

NATRON_NAMESPACE_USING

namespace {

const int32_t kCheckX = 1;
const int32_t kCheckY = 1;

void
expectPlane(const FlatExrImage& image,
            const std::string& prefix,
            float r,
            float g,
            float b)
{
    EXPECT_NEAR(r, image.at(kCheckX, kCheckY, prefix + "R"), 1e-5f) << prefix << "R";
    EXPECT_NEAR(g, image.at(kCheckX, kCheckY, prefix + "G"), 1e-5f) << prefix << "G";
    EXPECT_NEAR(b, image.at(kCheckX, kCheckY, prefix + "B"), 1e-5f) << prefix << "B";
}

void
expectColor(const FlatExrImage& image,
            float r,
            float g,
            float b,
            float a)
{
    expectPlane(image, "", r, g, b);
    EXPECT_NEAR(a, image.at(kCheckX, kCheckY, "A"), 1e-5f) << "A";
}

} // namespace

class ChannelSetRenderTest
    : public BaseTest {
protected:
    NodePtr createReader(const std::string& fixture = "flat-three-layers.exr")
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());

        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);

        return getApp()->createNode(readerArgs);
    }

    NodePtr createEffectOnReader(const QString& pluginID,
                                 KnobChannelSetPtr* channels,
                                 const std::string& fixture = "flat-three-layers.exr")
    {
        NodePtr reader = createReader(fixture);
        if (!reader) {
            return NodePtr();
        }
        NodePtr effect = createNode(pluginID);
        if (!effect) {
            return NodePtr();
        }
        connectNodes(reader, effect, 0, true);
        *channels = std::dynamic_pointer_cast<KnobChannelSet>(effect->getKnobByName(kNodeParamChannelSet));

        return effect;
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
        KnobBool* processAllLayers = dynamic_cast<KnobBool*>(writer->getKnobByName("processAllLayers").get());
        if (!partSplitting || !bitDepth || !compression || !processAllLayers) {
            *error = "writer knobs missing";

            return false;
        }
        partSplitting->setValueFromID("single", 0);
        bitDepth->setValueFromID("32f", 0);
        compression->setValueFromID("none", 0);
        processAllLayers->setValue(true);

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

// --- Grade (net.sf.openfx.GradePlugin) ---------------------------------------------------

TEST_F(ChannelSetRenderTest, GradeDefaultRowMatchesPluginMathAndMasksAlpha)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(QString::fromUtf8("net.sf.openfx.GradePlugin"), &channels);
    ASSERT_TRUE(bool(grade));

    KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);
    multiply->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(grade, tmp, &image, &error)) << error;

    expectColor(image, 0.5f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderTest, GradeSetAllProcessesEveryPlane)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(QString::fromUtf8("net.sf.openfx.GradePlugin"), &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));

    KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);
    multiply->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setAll();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(grade, tmp, &image, &error)) << error;

    expectColor(image, 0.5f, 0.f, 0.f, 0.5f);
    expectPlane(image, "diffuse.", 0.f, 0.5f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 0.5f);
}

TEST_F(ChannelSetRenderTest, GradeColorRSingleChannelTouchesOnlyR)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(QString::fromUtf8("net.sf.openfx.GradePlugin"), &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));

    KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);
    multiply->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    std::vector<std::string> r;
    r.push_back("R");
    channels->setLayer(0, kNatronColorLayerID, &r);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(grade, tmp, &image, &error)) << error;

    expectColor(image, 0.5f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderTest, GradeSpecularRowLeavesColorAndDiffuseUntouched)
{
    KnobChannelSetPtr channels;
    NodePtr grade = createEffectOnReader(QString::fromUtf8("net.sf.openfx.GradePlugin"), &channels);
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(channels));

    KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);
    multiply->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setLayer(0, "specular", NULL);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(grade, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 0.5f);
}

// --- Invert (net.sf.openfx.Invert) -------------------------------------------------------

// Unlike Grade/ColorCorrect/Multiply/Saturation, Invert's own quad defaults processA to true,
// so its default row inverts alpha too instead of masking it.
TEST_F(ChannelSetRenderTest, InvertDefaultRowMatchesPluginMath)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createEffectOnReader(QString::fromUtf8("net.sf.openfx.Invert"), &channels);
    ASSERT_TRUE(bool(invert));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(invert, tmp, &image, &error)) << error;

    expectColor(image, 0.f, 1.f, 1.f, 0.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderTest, InvertSetAllProcessesEveryPlane)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createEffectOnReader(QString::fromUtf8("net.sf.openfx.Invert"), &channels);
    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    channels->setAll();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(invert, tmp, &image, &error)) << error;

    expectColor(image, 0.f, 1.f, 1.f, 0.f);
    expectPlane(image, "diffuse.", 1.f, 0.f, 1.f);
    expectPlane(image, "specular.", 1.f, 1.f, 0.f);
}

TEST_F(ChannelSetRenderTest, InvertColorRSingleChannelTouchesOnlyR)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createEffectOnReader(QString::fromUtf8("net.sf.openfx.Invert"), &channels);
    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    std::vector<std::string> r;
    r.push_back("R");
    channels->setLayer(0, kNatronColorLayerID, &r);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(invert, tmp, &image, &error)) << error;

    expectColor(image, 0.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderTest, InvertSpecularRowLeavesColorAndDiffuseUntouched)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createEffectOnReader(QString::fromUtf8("net.sf.openfx.Invert"), &channels);
    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, "specular", NULL);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(invert, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 1.f, 1.f, 0.f);
}

// --- ColorCorrect (net.sf.openfx.ColorCorrectPlugin) -------------------------------------

TEST_F(ChannelSetRenderTest, ColorCorrectDefaultRowMatchesPluginMathAndMasksAlpha)
{
    KnobChannelSetPtr channels;
    NodePtr colorCorrect = createEffectOnReader(QString::fromUtf8("net.sf.openfx.ColorCorrectPlugin"), &channels);
    ASSERT_TRUE(bool(colorCorrect));

    KnobColor* masterGain = dynamic_cast<KnobColor*>(colorCorrect->getKnobByName("MasterGain").get());
    ASSERT_TRUE(masterGain != NULL);
    masterGain->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(colorCorrect, tmp, &image, &error)) << error;

    expectColor(image, 0.5f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderTest, ColorCorrectSetAllProcessesEveryPlane)
{
    KnobChannelSetPtr channels;
    NodePtr colorCorrect = createEffectOnReader(QString::fromUtf8("net.sf.openfx.ColorCorrectPlugin"), &channels);
    ASSERT_TRUE(bool(colorCorrect));
    ASSERT_TRUE(bool(channels));

    KnobColor* masterGain = dynamic_cast<KnobColor*>(colorCorrect->getKnobByName("MasterGain").get());
    ASSERT_TRUE(masterGain != NULL);
    masterGain->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setAll();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(colorCorrect, tmp, &image, &error)) << error;

    expectColor(image, 0.5f, 0.f, 0.f, 0.5f);
    expectPlane(image, "diffuse.", 0.f, 0.5f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 0.5f);
}

TEST_F(ChannelSetRenderTest, ColorCorrectColorRSingleChannelTouchesOnlyR)
{
    KnobChannelSetPtr channels;
    NodePtr colorCorrect = createEffectOnReader(QString::fromUtf8("net.sf.openfx.ColorCorrectPlugin"), &channels);
    ASSERT_TRUE(bool(colorCorrect));
    ASSERT_TRUE(bool(channels));

    KnobColor* masterGain = dynamic_cast<KnobColor*>(colorCorrect->getKnobByName("MasterGain").get());
    ASSERT_TRUE(masterGain != NULL);
    masterGain->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    std::vector<std::string> r;
    r.push_back("R");
    channels->setLayer(0, kNatronColorLayerID, &r);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(colorCorrect, tmp, &image, &error)) << error;

    expectColor(image, 0.5f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderTest, ColorCorrectSpecularRowLeavesColorAndDiffuseUntouched)
{
    KnobChannelSetPtr channels;
    NodePtr colorCorrect = createEffectOnReader(QString::fromUtf8("net.sf.openfx.ColorCorrectPlugin"), &channels);
    ASSERT_TRUE(bool(colorCorrect));
    ASSERT_TRUE(bool(channels));

    KnobColor* masterGain = dynamic_cast<KnobColor*>(colorCorrect->getKnobByName("MasterGain").get());
    ASSERT_TRUE(masterGain != NULL);
    masterGain->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setLayer(0, "specular", NULL);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(colorCorrect, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 0.5f);
}

// --- Multiply (net.sf.openfx.MultiplyPlugin) ---------------------------------------------

TEST_F(ChannelSetRenderTest, MultiplyDefaultRowMatchesPluginMathAndMasksAlpha)
{
    KnobChannelSetPtr channels;
    NodePtr multiplyNode = createEffectOnReader(QString::fromUtf8("net.sf.openfx.MultiplyPlugin"), &channels);
    ASSERT_TRUE(bool(multiplyNode));

    KnobColor* value = dynamic_cast<KnobColor*>(multiplyNode->getKnobByName("value").get());
    ASSERT_TRUE(value != NULL);
    value->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(multiplyNode, tmp, &image, &error)) << error;

    expectColor(image, 0.5f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderTest, MultiplySetAllProcessesEveryPlane)
{
    KnobChannelSetPtr channels;
    NodePtr multiplyNode = createEffectOnReader(QString::fromUtf8("net.sf.openfx.MultiplyPlugin"), &channels);
    ASSERT_TRUE(bool(multiplyNode));
    ASSERT_TRUE(bool(channels));

    KnobColor* value = dynamic_cast<KnobColor*>(multiplyNode->getKnobByName("value").get());
    ASSERT_TRUE(value != NULL);
    value->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setAll();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(multiplyNode, tmp, &image, &error)) << error;

    expectColor(image, 0.5f, 0.f, 0.f, 0.5f);
    expectPlane(image, "diffuse.", 0.f, 0.5f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 0.5f);
}

TEST_F(ChannelSetRenderTest, MultiplyColorRSingleChannelTouchesOnlyR)
{
    KnobChannelSetPtr channels;
    NodePtr multiplyNode = createEffectOnReader(QString::fromUtf8("net.sf.openfx.MultiplyPlugin"), &channels);
    ASSERT_TRUE(bool(multiplyNode));
    ASSERT_TRUE(bool(channels));

    KnobColor* value = dynamic_cast<KnobColor*>(multiplyNode->getKnobByName("value").get());
    ASSERT_TRUE(value != NULL);
    value->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    std::vector<std::string> r;
    r.push_back("R");
    channels->setLayer(0, kNatronColorLayerID, &r);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(multiplyNode, tmp, &image, &error)) << error;

    expectColor(image, 0.5f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderTest, MultiplySpecularRowLeavesColorAndDiffuseUntouched)
{
    KnobChannelSetPtr channels;
    NodePtr multiplyNode = createEffectOnReader(QString::fromUtf8("net.sf.openfx.MultiplyPlugin"), &channels);
    ASSERT_TRUE(bool(multiplyNode));
    ASSERT_TRUE(bool(channels));

    KnobColor* value = dynamic_cast<KnobColor*>(multiplyNode->getKnobByName("value").get());
    ASSERT_TRUE(value != NULL);
    value->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    channels->setLayer(0, "specular", NULL);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(multiplyNode, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 0.5f);
}

// --- Saturation (net.sf.openfx.SaturationPlugin) -----------------------------------------

TEST_F(ChannelSetRenderTest, SaturationDefaultRowMatchesPluginMathAndMasksAlpha)
{
    KnobChannelSetPtr channels;
    NodePtr saturationNode = createEffectOnReader(QString::fromUtf8("net.sf.openfx.SaturationPlugin"), &channels);
    ASSERT_TRUE(bool(saturationNode));

    KnobDouble* saturation = dynamic_cast<KnobDouble*>(saturationNode->getKnobByName("saturation").get());
    ASSERT_TRUE(saturation != NULL);
    saturation->setValue(0.);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(saturationNode, tmp, &image, &error)) << error;

    const float lumaRed = 0.2126729f;
    expectColor(image, lumaRed, lumaRed, lumaRed, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderTest, SaturationSetAllProcessesEveryPlane)
{
    KnobChannelSetPtr channels;
    NodePtr saturationNode = createEffectOnReader(QString::fromUtf8("net.sf.openfx.SaturationPlugin"), &channels);
    ASSERT_TRUE(bool(saturationNode));
    ASSERT_TRUE(bool(channels));

    KnobDouble* saturation = dynamic_cast<KnobDouble*>(saturationNode->getKnobByName("saturation").get());
    ASSERT_TRUE(saturation != NULL);
    saturation->setValue(0.);
    channels->setAll();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(saturationNode, tmp, &image, &error)) << error;

    const float lumaRed = 0.2126729f;
    const float lumaGreen = 0.7151522f;
    const float lumaBlue = 0.0721750f;
    expectColor(image, lumaRed, lumaRed, lumaRed, 1.f);
    expectPlane(image, "diffuse.", lumaGreen, lumaGreen, lumaGreen);
    expectPlane(image, "specular.", lumaBlue, lumaBlue, lumaBlue);
}

TEST_F(ChannelSetRenderTest, SaturationColorRSingleChannelTouchesOnlyR)
{
    KnobChannelSetPtr channels;
    NodePtr saturationNode = createEffectOnReader(QString::fromUtf8("net.sf.openfx.SaturationPlugin"), &channels);
    ASSERT_TRUE(bool(saturationNode));
    ASSERT_TRUE(bool(channels));

    KnobDouble* saturation = dynamic_cast<KnobDouble*>(saturationNode->getKnobByName("saturation").get());
    ASSERT_TRUE(saturation != NULL);
    saturation->setValue(0.);
    std::vector<std::string> r;
    r.push_back("R");
    channels->setLayer(0, kNatronColorLayerID, &r);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(saturationNode, tmp, &image, &error)) << error;

    const float lumaRed = 0.2126729f;
    expectColor(image, lumaRed, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderTest, SaturationSpecularRowLeavesColorAndDiffuseUntouched)
{
    KnobChannelSetPtr channels;
    NodePtr saturationNode = createEffectOnReader(QString::fromUtf8("net.sf.openfx.SaturationPlugin"), &channels);
    ASSERT_TRUE(bool(saturationNode));
    ASSERT_TRUE(bool(channels));

    KnobDouble* saturation = dynamic_cast<KnobDouble*>(saturationNode->getKnobByName("saturation").get());
    ASSERT_TRUE(saturation != NULL);
    saturation->setValue(0.);
    channels->setLayer(0, "specular", NULL);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(saturationNode, tmp, &image, &error)) << error;

    const float lumaBlue = 0.0721750f;
    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", lumaBlue, lumaBlue, lumaBlue);
}

// --- CImgBlur (net.sf.cimg.CImgBlur) ------------------------------------------------------

class ChannelSetRenderBlurTest
    : public ChannelSetRenderTest {
protected:
    NodePtr createBlurOnReader(KnobChannelSetPtr* channels)
    {
        NodePtr blur = createEffectOnReader(QString::fromUtf8("net.sf.cimg.CImgBlur"), channels);
        if (!blur) {
            return NodePtr();
        }
        KnobDouble* size = dynamic_cast<KnobDouble*>(blur->getKnobByName("size").get());
        if (!size) {
            return NodePtr();
        }
        size->setValues(3., 3., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        KnobChoice* boundary = dynamic_cast<KnobChoice*>(blur->getKnobByName("boundary").get());
        if (!boundary) {
            return NodePtr();
        }
        boundary->setValueFromID("nearest", 0);
        KnobBool* expandRoD = dynamic_cast<KnobBool*>(blur->getKnobByName("expandRoD").get());
        if (!expandRoD) {
            return NodePtr();
        }
        expandRoD->setValue(false);

        return blur;
    }
};

TEST_F(ChannelSetRenderBlurTest, CImgBlurDefaultRowIsIdenticalOnTheConstantFixture)
{
    KnobChannelSetPtr channels;
    NodePtr blur = createBlurOnReader(&channels);
    ASSERT_TRUE(bool(blur));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(blur, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderBlurTest, CImgBlurSetAllProcessesEveryPlane)
{
    KnobChannelSetPtr channels;
    NodePtr blur = createBlurOnReader(&channels);
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(bool(channels));

    channels->setAll();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(blur, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderBlurTest, CImgBlurColorRSingleChannelTouchesOnlyR)
{
    KnobChannelSetPtr channels;
    NodePtr blur = createBlurOnReader(&channels);
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(bool(channels));

    std::vector<std::string> r;
    r.push_back("R");
    channels->setLayer(0, kNatronColorLayerID, &r);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(blur, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(ChannelSetRenderBlurTest, CImgBlurSpecularRowLeavesColorAndDiffuseUntouched)
{
    KnobChannelSetPtr channels;
    NodePtr blur = createBlurOnReader(&channels);
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, "specular", NULL);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(blur, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

// --- Quad-adoption exceptions: KeyMix, DenoiseSharpen, ClipTest --------------------------
//
// adoptChannelQuad() leaves these three plug-ins' own R/G/B/A quad alone instead of forcing
// it true and hiding it (see the audit at the top of this file for why each one's quad is not
// a per-channel mask); Node::pluginOwnsChannelMask() reports that so the host also stops
// masking their output with the layer knob's row-0 channel bits.

TEST_F(ChannelSetRenderTest, KeyMixQuadIsLeftToThePluginWhileGradeQuadIsAdopted)
{
    KnobChannelSetPtr keyMixChannels;
    NodePtr keyMix = createEffectOnReader(QString::fromUtf8("net.sf.openfx.KeyMix"), &keyMixChannels);
    ASSERT_TRUE(bool(keyMix));
    EXPECT_TRUE(keyMix->pluginOwnsChannelMask());

    KnobBool* keyMixProcessR = dynamic_cast<KnobBool*>(keyMix->getKnobByName(kNatronOfxParamProcessR).get());
    ASSERT_TRUE(keyMixProcessR != NULL);
    EXPECT_FALSE(keyMixProcessR->getIsSecret());
    EXPECT_TRUE(keyMixProcessR->getIsPersistent());
    EXPECT_TRUE(keyMixProcessR->getDefaultValue(0));
    EXPECT_TRUE(keyMixProcessR->getValue());

    KnobChannelSetPtr gradeChannels;
    NodePtr grade = createEffectOnReader(QString::fromUtf8("net.sf.openfx.GradePlugin"), &gradeChannels);
    ASSERT_TRUE(bool(grade));
    EXPECT_FALSE(grade->pluginOwnsChannelMask());

    KnobBool* gradeProcessR = dynamic_cast<KnobBool*>(grade->getKnobByName(kNatronOfxParamProcessR).get());
    ASSERT_TRUE(gradeProcessR != NULL);
    EXPECT_TRUE(gradeProcessR->getIsSecret());
    EXPECT_FALSE(gradeProcessR->getIsPersistent());
    EXPECT_TRUE(gradeProcessR->getValue());
}

TEST_F(ChannelSetRenderTest, DenoiseSharpenAndClipTestAlsoOwnTheirChannelMask)
{
    KnobChannelSetPtr denoiseChannels;
    NodePtr denoiseSharpen = createEffectOnReader(QString::fromUtf8("net.sf.openfx.DenoiseSharpen"), &denoiseChannels);
    ASSERT_TRUE(bool(denoiseSharpen));
    EXPECT_TRUE(denoiseSharpen->pluginOwnsChannelMask());

    KnobChannelSetPtr clipTestChannels;
    NodePtr clipTest = createEffectOnReader(QString::fromUtf8("net.sf.openfx.ClipTestPlugin"), &clipTestChannels);
    ASSERT_TRUE(bool(clipTest));
    EXPECT_TRUE(clipTest->pluginOwnsChannelMask());
}

// KeyMix's multiThreadProcessImages() reads its own NatronOfxParamProcessR/G/B/A quad
// (_aChannels) directly: for channel c, the source is A[c] when _aChannels[c] is set, else B[c]
// is copied through untouched by the "copy unprocessed channels from B" loop, independently of
// mask/mix. With the mask open (Constant alpha 1, default mix 1), the per-channel blend inside
// ofxsMaskMixPix resolves to exactly that A[c]-or-B[c] choice, so the expected output is
// R = A.R (quad picks A), G/B/A = B.G/B/A (quad picks B).
TEST_F(ChannelSetRenderTest, KeyMixRChannelFromAAndGBAlphaFromBIgnoringTheMeaninglessLayerKnobRow)
{
    NodePtr readerA = createReader("flat-three-layers.exr");
    ASSERT_TRUE(bool(readerA));
    NodePtr readerB = createReader("flat-rgba-only.exr");
    ASSERT_TRUE(bool(readerB));

    // flat-rgba-only.exr's Color (1,0,0,1) is identical to flat-three-layers.exr's, so scale it
    // down first: with A and B carrying the same values, a channel reading the wrong source
    // would go unnoticed.
    NodePtr multiplyB = createNode(QString::fromUtf8("net.sf.openfx.MultiplyPlugin"));
    ASSERT_TRUE(bool(multiplyB));
    KnobColor* multiplyValue = dynamic_cast<KnobColor*>(multiplyB->getKnobByName("value").get());
    ASSERT_TRUE(multiplyValue != NULL);
    multiplyValue->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    // Multiply's own default row leaves alpha unprocessed (see MultiplyDefaultRowMatchesPluginMathAndMasksAlpha
    // above), so without this the multiplied B would keep A's original alpha and the two
    // channel sources would be indistinguishable on alpha too.
    KnobChannelSetPtr multiplyChannels = std::dynamic_pointer_cast<KnobChannelSet>(multiplyB->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(multiplyChannels));
    multiplyChannels->setAll();
    connectNodes(readerB, multiplyB, 0, true);
    // B after the multiply: (0.5, 0, 0, 0.5).

    NodePtr maskConstant = createNode(QString::fromUtf8("net.sf.openfx.ConstantPlugin"));
    ASSERT_TRUE(bool(maskConstant));
    KnobColor* maskColor = dynamic_cast<KnobColor*>(maskConstant->getKnobByName("color").get());
    ASSERT_TRUE(maskColor != NULL);
    maskColor->setValues(0., 0., 0., 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    NodePtr keyMix = createNode(QString::fromUtf8("net.sf.openfx.KeyMix"));
    ASSERT_TRUE(bool(keyMix));
    ASSERT_EQ(std::string("B"), keyMix->getInputLabel(0));
    ASSERT_EQ(std::string("A"), keyMix->getInputLabel(1));
    ASSERT_EQ(std::string("Mask"), keyMix->getInputLabel(2));
    connectNodes(multiplyB, keyMix, 0, true);
    connectNodes(readerA, keyMix, 1, true);
    connectNodes(maskConstant, keyMix, 2, true);

    KnobBool* maskEnabled = dynamic_cast<KnobBool*>(keyMix->getKnobByName("enableMask_Mask").get());
    ASSERT_TRUE(maskEnabled != NULL);
    maskEnabled->setValue(true);
    KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(keyMix->getKnobByName("maskChannel_Mask").get());
    ASSERT_TRUE(maskChannel != NULL);
    maskChannel->set(ImageLayerDesc::getRGBAComponents().getChannelOption(3).id);

    KnobBool* processR = dynamic_cast<KnobBool*>(keyMix->getKnobByName(kNatronOfxParamProcessR).get());
    KnobBool* processG = dynamic_cast<KnobBool*>(keyMix->getKnobByName(kNatronOfxParamProcessG).get());
    KnobBool* processB = dynamic_cast<KnobBool*>(keyMix->getKnobByName(kNatronOfxParamProcessB).get());
    KnobBool* processA = dynamic_cast<KnobBool*>(keyMix->getKnobByName(kNatronOfxParamProcessA).get());
    ASSERT_TRUE(processR != NULL && processG != NULL && processB != NULL && processA != NULL);
    processR->setValue(true);
    processG->setValue(false);
    processB->setValue(false);
    processA->setValue(false);

    // The layer knob's row-0 channel buttons are meaningless for this plug-in: restrict them to
    // exclude R, the opposite of the plug-in's own quad, so a host that still masked by these
    // bits (the bug this test guards against) would overwrite R with the preferred input's (B's)
    // value instead of leaving the plug-in's own A-sourced R alone.
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(keyMix->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    std::vector<std::string> gba;
    gba.push_back("G");
    gba.push_back("B");
    gba.push_back("A");
    channels->setLayer(0, kNatronColorLayerID, &gba);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(keyMix, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 0.5f);
}
