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
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kWidth = 64;
const int kHeight = 48;
const double kTime = 1.;

struct FileImage {
    OIIO::ImageSpec spec;
    std::vector<float> pixels;
};

bool
readBack(const std::string& path,
         FileImage* out,
         std::string* error)
{
    // Without this the reader multiplies the colour by the alpha of a format that stores it unassociated.
    OIIO::ImageSpec config;

    config.attribute("oiio:UnassociatedAlpha", 1);
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path, &config);
    if (!input) {
        *error = OIIO::geterror();

        return false;
    }
    out->spec = input->spec();
    out->pixels.assign((std::size_t)out->spec.width * (std::size_t)out->spec.height * (std::size_t)out->spec.nchannels, 0.f);
    const bool ok = input->read_image(0, 0, 0, out->spec.nchannels, OIIO::TypeDesc::FLOAT, out->pixels.data());
    if (!ok) {
        *error = input->geterror();
    }
    input->close();

    return ok;
}

struct Difference {
    double maxError = 0.;
    double meanError = 0.;
    int compared = 0;
};

// Compares the file's pixels to the rendered plane over the file's data window; the file's rows
// run downwards from the top of that window, the plane's upwards.
Difference
compare(const FileImage& file,
        const RenderedPlane& reference)
{
    Difference d;
    const RectI window = OiioReadSupport::dataWindowOf(file.spec);
    const int refWidth = reference.window.width();
    const int nChannels = std::min(file.spec.nchannels, 4);
    double sum = 0.;

    for (int row = 0; row < file.spec.height; ++row) {
        const int y = window.y2 - 1 - row;
        for (int col = 0; col < file.spec.width; ++col) {
            const int x = window.x1 + col;
            if (x < reference.window.x1 || x >= reference.window.x2 || y < reference.window.y1 || y >= reference.window.y2) {
                continue;
            }
            for (int c = 0; c < nChannels; ++c) {
                const float expected = reference.pixels[(std::size_t)((y - reference.window.y1) * refWidth + (x - reference.window.x1)) * 4 + (std::size_t)c];
                const float actual = file.pixels[((std::size_t)row * (std::size_t)file.spec.width + (std::size_t)col) * (std::size_t)file.spec.nchannels + (std::size_t)c];
                const double error = std::fabs((double)expected - (double)actual);
                d.maxError = std::max(d.maxError, error);
                sum += error;
                ++d.compared;
            }
        }
    }
    d.meanError = d.compared > 0 ? sum / d.compared : 0.;

    return d;
}

// JPEG is lossy and subsamples chroma, so the saturated 10x7 boxes of the fixture bleed into each other;
// the writer is checked against the pixels libjpeg itself produces for the same plane and quality.
bool
jpegBaseline(const RenderedPlane& reference,
             const std::string& path,
             FileImage* out,
             std::string* error)
{
    const int width = reference.window.width();
    const int height = reference.window.height();
    std::vector<float> rows((std::size_t)width * (std::size_t)height * 3, 0.f);

    for (int row = 0; row < height; ++row) {
        const int y = reference.window.y2 - 1 - row;
        for (int col = 0; col < width; ++col) {
            for (int c = 0; c < 3; ++c) {
                rows[((std::size_t)row * (std::size_t)width + (std::size_t)col) * 3 + (std::size_t)c] = reference.pixels[(std::size_t)((y - reference.window.y1) * width + col) * 4 + (std::size_t)c];
            }
        }
    }
    OIIO::ImageSpec spec(width, height, 3, OIIO::TypeDesc::UINT8);
    spec.attribute("compression", "jpeg:100");
    std::unique_ptr<OIIO::ImageOutput> output = OIIO::ImageOutput::create(path);
    if (!output || !output->open(path, spec) || !output->write_image(OIIO::TypeDesc::FLOAT, rows.data()) || !output->close()) {
        *error = output ? output->geterror() : OIIO::geterror();

        return false;
    }

    return readBack(path, out, error);
}

std::string
slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);

    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool
exists(const std::string& path)
{
    return std::ifstream(path, std::ios::binary).good();
}

struct EncodeCase {
    const char* name;
    const char* extension;
    const char* bitDepth;
    const char* compression;
    double maxError;
    double meanError;
    int channels;
    OIIO::TypeDesc type;
};

} // namespace

class NativeWriteEncodeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeWriteEncode", 1.));

        _checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        ASSERT_TRUE(bool(_checker));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamBoxSize, { 10., 7. }));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamColor0, { 0.1, 0.2, 0.3, 1.0 }));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamColor1, { 0.9, 0.5, 0.25, 1.0 }));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamColor2, { 0.0, 1.0, 0.5, 0.5 }));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamColor3, { 0.75, 0.25, 1.0, 0.75 }));

        _writer = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
        ASSERT_TRUE(bool(_writer));
        // The values are compared with the constants that went in, so the file is written without conversion.
        KnobStringBase* outputSpace = dynamic_cast<KnobStringBase*>(_writer->getKnobByName("ocioOutputSpace").get());
        KnobBool* outputSpaceSet = dynamic_cast<KnobBool*>(_writer->getKnobByName("ocioOutputSpaceSet").get());
        ASSERT_TRUE(outputSpace != NULL);
        ASSERT_TRUE(outputSpaceSet != NULL);
        outputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
        outputSpaceSet->setValue(true);
        connectNodes(_checker, _writer, 0, true);

        ASSERT_TRUE(_dir.isValid());
    }

    virtual void TearDown() OVERRIDE
    {
        _writer.reset();
        _checker.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    std::string pathOf(const std::string& name) const
    {
        return (_dir.path() + QLatin1Char('/') + QString::fromStdString(name)).toStdString();
    }

    KnobIPtr knob(const NodePtr& node,
                  const char* name)
    {
        KnobIPtr k = node->getKnobByName(name);

        EXPECT_TRUE(bool(k)) << name;

        return k;
    }

    void setFile(const std::string& path)
    {
        KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(knob(_writer, kOfxImageEffectFileParamName).get());

        ASSERT_TRUE(file != NULL);
        file->setValue(path);
    }

    bool setChoice(const NodePtr& node,
                   const char* name,
                   const std::string& id)
    {
        KnobChoice* choice = dynamic_cast<KnobChoice*>(knob(node, name).get());

        if (!choice) {
            return false;
        }
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

                return true;
            }
        }
        ADD_FAILURE() << name << " has no option " << id;

        return false;
    }

    bool isShown(const char* name)
    {
        KnobIPtr k = knob(_writer, name);

        return k && !k->getIsSecret();
    }

    void renderFrame()
    {
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());

        ASSERT_TRUE(effect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);
    }

    RenderedPlane referencePlane()
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        std::list<ImageLayerDesc> layers;

        layers.push_back(ImageLayerDesc::getRGBAComponents());
        EXPECT_TRUE(renderNodePlanesDirect(_checker, kTime, ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), layers, &planes, &error)) << error;

        return planes.empty() ? RenderedPlane() : planes.front();
    }

    NodePtr _checker;
    NodePtr _writer;
    QTemporaryDir _dir;
};

TEST_F(NativeWriteEncodeTest, KnobsKeepTheWriteOIIOScriptNames)
{
    static const char* const kNames[] = {
        kOfxImageEffectFileParamName, kNatronWriteParamOverwrite, kNatronWriteParamClipToRoD, kNatronWriteParamBitDepth, kNatronWriteParamCompression,
        kNatronWriteParamQuality, kNatronWriteParamDWACompressionLevel,
        kNatronWriteParamZIPCompressionLevel, kNatronWriteParamTileSize, kNatronWriteParamFrameStep
    };

    for (std::size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        EXPECT_TRUE(bool(_writer->getKnobByName(kNames[i]))) << kNames[i];
    }
    KnobBool* overwrite = dynamic_cast<KnobBool*>(knob(_writer, kNatronWriteParamOverwrite).get());
    ASSERT_TRUE(overwrite != NULL);
    EXPECT_TRUE(overwrite->getValue());

    static const char* const kGone[] = {
        "formatType", kNatronParamFormatChoice, kNatronParamFormatSize, kNatronParamFormatPar
    };
    for (std::size_t i = 0; i < sizeof(kGone) / sizeof(kGone[0]); ++i) {
        EXPECT_FALSE(bool(_writer->getKnobByName(kGone[i]))) << kGone[i] << ": the Write always writes its input's format";
    }

    KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(knob(_writer, kNatronWriteParamBitDepth).get());
    ASSERT_TRUE(bitDepth != NULL);
    EXPECT_EQ(std::string(kNatronWriteBitDepthAuto), bitDepth->getEntries_mt_safe().front().id);
}

TEST_F(NativeWriteEncodeTest, OptionsFollowTheFormatOfTheFileName)
{
    setFile(pathOf("options.exr"));
    EXPECT_TRUE(isShown(kNatronWriteParamCompression));
    EXPECT_TRUE(isShown(kNatronWriteParamTileSize));
    EXPECT_TRUE(isShown(kNatronWriteParamClipToRoD));
    EXPECT_TRUE(isShown(kNatronWriteParamBitDepth));
    EXPECT_FALSE(isShown(kNatronWriteParamQuality));
    EXPECT_FALSE(isShown(kNatronWriteParamDWACompressionLevel));
    EXPECT_FALSE(isShown(kNatronWriteParamZIPCompressionLevel));

    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamCompression, "zip"));
    EXPECT_TRUE(isShown(kNatronWriteParamZIPCompressionLevel));
    EXPECT_FALSE(isShown(kNatronWriteParamDWACompressionLevel));

    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamCompression, "dwaa"));
    EXPECT_TRUE(isShown(kNatronWriteParamDWACompressionLevel));
    EXPECT_FALSE(isShown(kNatronWriteParamZIPCompressionLevel));
    EXPECT_FALSE(isShown(kNatronWriteParamQuality));

    setFile(pathOf("options.jpg"));
    EXPECT_TRUE(isShown(kNatronWriteParamQuality));
    EXPECT_FALSE(isShown(kNatronWriteParamDWACompressionLevel));
    EXPECT_FALSE(isShown(kNatronWriteParamZIPCompressionLevel));
    EXPECT_FALSE(isShown(kNatronWriteParamTileSize));
    EXPECT_FALSE(isShown(kNatronWriteParamClipToRoD));

    KnobChoice* compression = dynamic_cast<KnobChoice*>(knob(_writer, kNatronWriteParamCompression).get());
    ASSERT_TRUE(compression != NULL);
    EXPECT_EQ(std::string(kNatronWriteCompressionDefault), compression->getActiveEntry().id)
        << "the dwaa choice does not survive a format that has no such compression";

    setFile(pathOf("options.exr"));
    EXPECT_TRUE(isShown(kNatronWriteParamClipToRoD));
    EXPECT_FALSE(isShown(kNatronWriteParamQuality));
}

TEST_F(NativeWriteEncodeTest, EncodesTheColourPlane)
{
    const OIIO::TypeDesc half = OIIO::TypeDesc::HALF;
    const OIIO::TypeDesc f32 = OIIO::TypeDesc::FLOAT;
    const OIIO::TypeDesc u8 = OIIO::TypeDesc::UINT8;
    const OIIO::TypeDesc u16 = OIIO::TypeDesc::UINT16;
    const EncodeCase cases[] = {
        { "exr-half-none", "exr", "16f", "none", 1e-3, 1e-3, 4, half },
        { "exr-half-zip", "exr", "16f", "zip", 1e-3, 1e-3, 4, half },
        { "exr-half-dwaa", "exr", "16f", "dwaa", 1., 0.03, 4, half },
        { "exr-float-none", "exr", "32f", "none", 1e-6, 1e-6, 4, f32 },
        { "exr-float-zip", "exr", "32f", "zip", 1e-6, 1e-6, 4, f32 },
        { "png-8", "png", "8i", "default", 0.003, 0.003, 4, u8 },
        { "png-16", "png", "16i", "default", 1e-4, 1e-4, 4, u16 },
        { "jpeg", "jpg", "8i", "default", 1., 1., 3, u8 },
        { "tiff-16", "tif", "16i", "default", 1e-4, 1e-4, 4, u16 },
        { "dpx-16", "dpx", "16i", "default", 1e-4, 1e-4, 4, u16 },
    };
    const RenderedPlane reference = referencePlane();

    ASSERT_FALSE(reference.pixels.empty());
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const EncodeCase& c = cases[i];
        SCOPED_TRACE(c.name);
        const std::string path = pathOf(std::string(c.name) + "." + c.extension);

        setFile(path);
        ASSERT_TRUE(setChoice(_writer, kNatronWriteParamBitDepth, c.bitDepth));
        if (std::string(c.compression) != kNatronWriteCompressionDefault) {
            ASSERT_TRUE(setChoice(_writer, kNatronWriteParamCompression, c.compression));
        }
        renderFrame();

        FileImage file;
        std::string error;
        ASSERT_TRUE(readBack(path, &file, &error)) << error;
        EXPECT_EQ(kWidth, file.spec.width);
        EXPECT_EQ(kHeight, file.spec.height);
        EXPECT_EQ(c.channels, file.spec.nchannels);
        EXPECT_EQ(c.type, file.spec.format);
        const Difference d = compare(file, reference);
        EXPECT_EQ(kWidth * kHeight * std::min(c.channels, 4), d.compared);
        EXPECT_LE(d.maxError, c.maxError);
        EXPECT_LE(d.meanError, c.meanError);
        if (std::string(c.extension) == "jpg") {
            FileImage baseline;
            ASSERT_TRUE(jpegBaseline(reference, pathOf("baseline.jpg"), &baseline, &error)) << error;
            ASSERT_EQ(baseline.pixels.size(), file.pixels.size());
            double worst = 0.;
            for (std::size_t p = 0; p < file.pixels.size(); ++p) {
                worst = std::max(worst, (double)std::fabs(file.pixels[p] - baseline.pixels[p]));
            }
            EXPECT_LE(worst, 1. / 255.) << "the written JPEG differs from libjpeg's own encode";
            EXPECT_LE(d.meanError, 0.15);
        }
        if (std::string(c.extension) == "exr") {
            const std::string stored = file.spec.get_string_attribute("compression");
            EXPECT_EQ(0u, stored.find(c.compression)) << stored;
        }
    }
}

TEST_F(NativeWriteEncodeTest, AutoBitDepthIsHalfForExr)
{
    const std::string path = pathOf("auto.exr");

    setFile(path);
    renderFrame();
    FileImage file;
    std::string error;
    ASSERT_TRUE(readBack(path, &file, &error)) << error;
    EXPECT_EQ(OIIO::TypeDesc(OIIO::TypeDesc::HALF), file.spec.format);
}

TEST_F(NativeWriteEncodeTest, ClipToRoDWritesTheRoDAsTheDataWindow)
{
    ASSERT_TRUE(setChoice(_checker, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(_checker, kNativeGeneratorParamBottomLeft, { 8., 6. }));
    ASSERT_TRUE(setKnobValues(_checker, kNativeGeneratorParamSize, { 32., 24. }));
    const RenderedPlane reference = referencePlane();
    ASSERT_FALSE(reference.pixels.empty());

    const std::string clipped = pathOf("clipped.exr");
    setFile(clipped);
    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamBitDepth, "32f"));
    KnobBool* clipToRoD = dynamic_cast<KnobBool*>(knob(_writer, kNatronWriteParamClipToRoD).get());
    ASSERT_TRUE(clipToRoD != NULL);
    clipToRoD->setValue(true);
    renderFrame();

    FileImage file;
    std::string error;
    ASSERT_TRUE(readBack(clipped, &file, &error)) << error;
    EXPECT_EQ(32, file.spec.width);
    EXPECT_EQ(24, file.spec.height);
    EXPECT_EQ(8, file.spec.x);
    EXPECT_EQ(kHeight - (6 + 24), file.spec.y);
    EXPECT_EQ(0, file.spec.full_x);
    EXPECT_EQ(0, file.spec.full_y);
    EXPECT_EQ(kWidth, file.spec.full_width);
    EXPECT_EQ(kHeight, file.spec.full_height);
    EXPECT_EQ(RectI(8, 6, 40, 30), OiioReadSupport::dataWindowOf(file.spec));
    Difference d = compare(file, reference);
    EXPECT_EQ(32 * 24 * 4, d.compared);
    EXPECT_LE(d.maxError, 1e-6);

    const std::string padded = pathOf("padded.exr");
    clipToRoD->setValue(false);
    setFile(padded);
    renderFrame();
    ASSERT_TRUE(readBack(padded, &file, &error)) << error;
    EXPECT_EQ(kWidth, file.spec.width);
    EXPECT_EQ(kHeight, file.spec.height);
    EXPECT_EQ(RectI(0, 0, kWidth, kHeight), OiioReadSupport::dataWindowOf(file.spec));
    d = compare(file, reference);
    EXPECT_EQ(kWidth * kHeight * 4, d.compared);
    EXPECT_LE(d.maxError, 1e-6) << "outside the region of definition the file is black, as the render is";
}

TEST_F(NativeWriteEncodeTest, WritesTheInputFormatNotTheProjectFormat)
{
    getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, 1920, 1080, "nativeWriteEncodeHD", 1.));
    ASSERT_TRUE(setChoice(_checker, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(_checker, kNativeGeneratorParamBottomLeft, { 8., 6. }));
    ASSERT_TRUE(setKnobValues(_checker, kNativeGeneratorParamSize, { 32., 24. }));

    NodePtr reformat = createNode(QString::fromUtf8(PLUGINID_NATRON_REFORMAT), PLUGIN_MAJOR_NATRON_REFORMAT);
    ASSERT_TRUE(bool(reformat));
    ASSERT_TRUE(setKnobValue(reformat, kReformatParamType, kReformatParamTypeOptionToBox));
    ASSERT_TRUE(setKnobValues(reformat, kReformatParamBoxSize, { 640., 360. }));
    ASSERT_TRUE(setKnobValues(reformat, kReformatParamBoxFixed, { 1. }));
    ASSERT_TRUE(setKnobValue(reformat, kReformatParamResize, kReformatParamResizeOptionNone));
    ASSERT_TRUE(setKnobValues(reformat, kReformatParamCenter, { 0. }));
    disconnectNodes(_checker, _writer, true);
    connectNodes(_checker, reformat, 0, true);
    connectNodes(reformat, _writer, 0, true);

    EffectInstancePtr reformatEffect = reformat->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    ASSERT_EQ(eStatusOK, reformatEffect->getRegionOfDefinition_public(reformatEffect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    const RectI dataWindow = rod.toPixelEnclosing(0u, 1.);
    ASSERT_GT(dataWindow.width(), 0);
    ASSERT_GT(dataWindow.height(), 0);

    const std::string clipped = pathOf("inputFormatClipped.exr");
    setFile(clipped);
    KnobBool* clipToRoD = dynamic_cast<KnobBool*>(knob(_writer, kNatronWriteParamClipToRoD).get());
    ASSERT_TRUE(clipToRoD != NULL);
    clipToRoD->setValue(true);
    renderFrame();

    FileImage file;
    std::string error;
    ASSERT_TRUE(readBack(clipped, &file, &error)) << error;
    EXPECT_EQ(0, file.spec.full_x);
    EXPECT_EQ(0, file.spec.full_y);
    EXPECT_EQ(640, file.spec.full_width);
    EXPECT_EQ(360, file.spec.full_height);
    EXPECT_EQ(dataWindow, OiioReadSupport::dataWindowOf(file.spec));

    const std::string full = pathOf("inputFormatFull.exr");
    clipToRoD->setValue(false);
    setFile(full);
    renderFrame();
    ASSERT_TRUE(readBack(full, &file, &error)) << error;
    EXPECT_EQ(640, file.spec.full_width);
    EXPECT_EQ(360, file.spec.full_height);
    EXPECT_EQ(RectI(0, 0, 640, 360), OiioReadSupport::dataWindowOf(file.spec));
}

TEST_F(NativeWriteEncodeTest, OverwriteOffLeavesAnExistingFileUntouched)
{
    const std::string path = pathOf("keep.png");
    const std::string sentinel = "not an image";

    {
        std::ofstream out(path, std::ios::binary);
        out << sentinel;
    }
    setFile(path);
    KnobBool* overwrite = dynamic_cast<KnobBool*>(knob(_writer, kNatronWriteParamOverwrite).get());
    ASSERT_TRUE(overwrite != NULL);
    overwrite->setValue(false);
    renderFrame();
    EXPECT_EQ(sentinel, slurp(path));

    const std::string fresh = pathOf("fresh.png");
    setFile(fresh);
    renderFrame();
    EXPECT_TRUE(exists(fresh)) << "overwrite off still writes a file that is not there yet";

    setFile(path);
    overwrite->setValue(true);
    renderFrame();
    FileImage file;
    std::string error;
    EXPECT_TRUE(readBack(path, &file, &error)) << error;
}

TEST_F(NativeWriteEncodeTest, UnsupportedExtensionWritesNothing)
{
    const std::string path = pathOf("nothing.notaformat");

    setFile(path);
    EXPECT_TRUE(_writer->hasPersistentMessage());
    renderFrame();
    EXPECT_FALSE(exists(path));
    EXPECT_TRUE(_writer->hasPersistentMessage());
}

TEST_F(NativeWriteEncodeTest, TaskGraphRenderHasNoUnplannedPulls)
{
    setFile(pathOf("planned.exr"));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    std::vector<int> unplannedPulls;
    const RenderMismatch m = renderBothWaysDirect(_writer, kTime, ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), poolSizes, std::function<void()>(), 0.f, &unplannedPulls);

    EXPECT_FALSE(m.any) << describe(m);
    for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
        EXPECT_EQ(0, unplannedPulls[p]) << "pool " << poolSizes[p];
    }
}
