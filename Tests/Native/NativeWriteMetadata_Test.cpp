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
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/IO/OiioWriteSupport.h"
#include "Engine/Nodes/Merge/Switch.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kSize = 8;

bool
writeImage(const std::string& path,
           const OIIO::ImageSpec& spec)
{
    std::vector<float> pixels((std::size_t)spec.width * spec.height * spec.nchannels, 0.25f);
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

    return out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();
}

bool
writeSourceExr(const std::string& path)
{
    OIIO::ImageSpec spec(kSize, kSize, 4, OIIO::TypeDesc::FLOAT);
    const float vector[3] = { 1.f, 2.f, 3.f };
    const int rate[2] = { 24000, 1001 };
    const unsigned int timecode[2] = { 0x01020304u, 0u };

    spec.attribute("compression", "none");
    spec.attribute("myString", "hello");
    spec.attribute("myInt", 42);
    spec.attribute("myFloat", 1.5f);
    spec.attribute("myVec", OIIO::TypeVector, vector);
    spec.attribute("Exif:Make", "NatronCam");
    spec.attribute("FramesPerSecond", OIIO::TypeRational, rate);
    spec.attribute("smpte:TimeCode", OIIO::TypeTimeCode, timecode);

    return writeImage(path, spec);
}

// A plain 4-channel EXR of the given size and pixel aspect ratio, naming its source in a custom
// "mySource" attribute.
bool
writeSizedExr(const std::string& path,
              int width,
              int height,
              float par,
              const std::string& source)
{
    OIIO::ImageSpec spec(width, height, 4, OIIO::TypeDesc::FLOAT);

    spec.attribute("compression", "none");
    spec.attribute("PixelAspectRatio", par);
    spec.attribute("mySource", source);

    return writeImage(path, spec);
}

bool
writeSourceJpeg(const std::string& path)
{
    OIIO::ImageSpec spec(kSize, kSize, 3, OIIO::TypeDesc::UINT8);

    spec.attribute("Make", "NatronCam");
    spec.attribute("Model", "Model One");
    // 2 s because the JPEG writer's rational conversion does not round-trip exposure times below one second.
    spec.attribute("ExposureTime", 2.f);
    spec.attribute("FNumber", 4.f);
    spec.attribute("Exif:PhotographicSensitivity", 400);

    return writeImage(path, spec);
}

std::optional<OIIO::ImageSpec>
specOf(const std::string& path)
{
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);

    if (!input) {
        return std::nullopt;
    }
    const OIIO::ImageSpec spec = input->spec();
    input->close();

    return spec;
}

NativeEffectBase*
nativeEffectOf(const NodePtr& node)
{
    return dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}
} // namespace

class NativeWriteMetadataTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
        ASSERT_TRUE(_dir.isValid());
    }

    virtual void TearDown() OVERRIDE
    {
        _write.reset();
        _dot.reset();
        _read.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    std::string pathOf(const std::string& name) const
    {
        return (_dir.path() + QLatin1Char('/') + QString::fromStdString(name)).toStdString();
    }

    // Read -> Dot -> Write, the Write pointed at `output`.
    void buildChain(const std::string& source,
                    const std::string& output)
    {
        _read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        _dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
        _write = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
        ASSERT_TRUE(_read && _dot && _write);

        KnobFile* sourceKnob = dynamic_cast<KnobFile*>(_read->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(sourceKnob != NULL);
        sourceKnob->setValue(source);
        _read->getEffectInstance()->dropCachedMetadata();

        connectNodes(_read, _dot, 0, true);
        connectNodes(_dot, _write, 0, true);
        configureWrite(output);
    }

    // The Write writes `output` in its input's format, with no colour conversion.
    void configureWrite(const std::string& output)
    {
        // The file is compared with what went in, so no colour conversion may sit between them.
        KnobStringBase* outputSpace = dynamic_cast<KnobStringBase*>(_write->getKnobByName("ocioOutputSpace").get());
        KnobBool* outputSpaceSet = dynamic_cast<KnobBool*>(_write->getKnobByName("ocioOutputSpaceSet").get());
        if (outputSpace && outputSpaceSet) {
            outputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
            outputSpaceSet->setValue(true);
        }

        KnobChoice* formatType = dynamic_cast<KnobChoice*>(_write->getKnobByName(kNatronWriteParamFormatType).get());
        ASSERT_TRUE(formatType != NULL);
        const std::vector<ChoiceOption> entries = formatType->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == kNatronWriteFormatTypeInput) {
                formatType->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
            }
        }

        KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(_write->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(file != NULL);
        file->setValue(output);
    }

    void setWriteMetadata(bool on)
    {
        KnobBool* knob = dynamic_cast<KnobBool*>(_write->getKnobByName(kNatronWriteParamWriteMetadata).get());

        ASSERT_TRUE(knob != NULL);
        knob->setValue(on);
    }

    void renderFrame()
    {
        renderFrames(1, 1);
    }

    void renderFrames(int first,
                      int last)
    {
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(_write->getEffectInstance().get());

        ASSERT_TRUE(effect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, first, last, 1, false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);
    }

    NodePtr createRead(const std::string& source)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        EXPECT_TRUE(bool(read));
        if (!read) {
            return read;
        }
        KnobFile* knob = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());
        EXPECT_TRUE(knob != NULL);
        if (knob) {
            knob->setValue(source);
            read->getEffectInstance()->dropCachedMetadata();
        }

        return read;
    }

    // The metadata a native Read gives the file at `path`.
    ImageMetadata reread(const std::string& path)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        EXPECT_TRUE(bool(read));
        if (!read) {
            return ImageMetadata();
        }
        KnobFile* knob = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());
        EXPECT_TRUE(knob != NULL);
        if (!knob) {
            return ImageMetadata();
        }
        knob->setValue(path);
        read->getEffectInstance()->dropCachedMetadata();
        NativeEffectBase* effect = nativeEffectOf(read);
        EXPECT_TRUE(effect != NULL);

        return effect ? effect->getOutputMetadata(1., ViewIdx(0)) : ImageMetadata();
    }

    QTemporaryDir _dir;
    NodePtr _read;
    NodePtr _dot;
    NodePtr _write;
};

TEST_F(NativeWriteMetadataTest, ExrAttributesSurviveReadDotWrite)
{
    ASSERT_TRUE(writeSourceExr(pathOf("src.0001.exr")));
    buildChain(pathOf("src.####.exr"), pathOf("out.exr"));
    ASSERT_TRUE(bool(_write));

    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.exr"));
    ASSERT_TRUE(spec.has_value());

    EXPECT_EQ("hello", spec->get_string_attribute("myString"));
    EXPECT_EQ(42, spec->get_int_attribute("myInt", -1));
    EXPECT_FLOAT_EQ(1.5f, spec->get_float_attribute("myFloat", -1.f));
    EXPECT_EQ("NatronCam", spec->get_string_attribute("Exif:Make"));

    const OIIO::ParamValue* vector = spec->find_attribute("myVec");
    ASSERT_TRUE(vector != NULL);
    ASSERT_EQ(3, vector->type().basevalues());
    const float* components = static_cast<const float*>(vector->data());
    EXPECT_FLOAT_EQ(1.f, components[0]);
    EXPECT_FLOAT_EQ(2.f, components[1]);
    EXPECT_FLOAT_EQ(3.f, components[2]);

    const OIIO::ParamValue* timecode = spec->find_attribute("smpte:TimeCode");
    ASSERT_TRUE(timecode != NULL);
    EXPECT_EQ(0x01020304u, static_cast<const unsigned int*>(timecode->data())[0]);

    const OIIO::ParamValue* rate = spec->find_attribute("FramesPerSecond");
    ASSERT_TRUE(rate != NULL);
    ASSERT_EQ(2, rate->type().basevalues());
    EXPECT_EQ(24000, static_cast<const int*>(rate->data())[0]);
    EXPECT_EQ(1001, static_cast<const int*>(rate->data())[1]);

    const ImageMetadata metadata = reread(pathOf("out.exr"));
    EXPECT_EQ(std::optional<std::string>("hello"), metadata.getString("exr/myString"));
    EXPECT_EQ(std::optional<int>(42), metadata.getInt("exr/myInt"));
    EXPECT_EQ(std::optional<double>(1.5), metadata.getDouble("exr/myFloat"));
    EXPECT_EQ(std::optional<std::vector<double>>(std::vector<double>({ 1., 2., 3. })), metadata.getDoubleVector("exr/myVec"));
    EXPECT_EQ(std::optional<std::string>("NatronCam"), metadata.getString("exif/Make"));
    EXPECT_EQ(std::optional<std::string>("01:02:03:04"), metadata.getString("ofx/timecode"));
    EXPECT_EQ(std::optional<double>(24000. / 1001.), metadata.getDouble("exr/FramesPerSecond"));
}

TEST_F(NativeWriteMetadataTest, SourceLayoutIsNotWritten)
{
    ASSERT_TRUE(writeSourceExr(pathOf("src.0001.exr")));
    buildChain(pathOf("src.####.exr"), pathOf("out.exr"));
    ASSERT_TRUE(bool(_write));

    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.exr"));
    ASSERT_TRUE(spec.has_value());

    // The source says "none"; the compression of the file is the Write's own choice.
    EXPECT_NE("none", spec->get_string_attribute("compression"));
}

TEST_F(NativeWriteMetadataTest, SwitchedOffWritesNoneOfTheCustomAttributes)
{
    ASSERT_TRUE(writeSourceExr(pathOf("src.0001.exr")));
    buildChain(pathOf("src.####.exr"), pathOf("out.exr"));
    ASSERT_TRUE(bool(_write));
    setWriteMetadata(false);

    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.exr"));
    ASSERT_TRUE(spec.has_value());
    for (const char* name : { "myString", "myInt", "myFloat", "myVec", "Exif:Make", "FramesPerSecond", "smpte:TimeCode" }) {
        EXPECT_TRUE(spec->find_attribute(name) == NULL) << name;
    }
}

TEST_F(NativeWriteMetadataTest, WriteMetadataDefaultsOn)
{
    NodePtr write = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
    ASSERT_TRUE(bool(write));
    KnobBool* knob = dynamic_cast<KnobBool*>(write->getKnobByName(kNatronWriteParamWriteMetadata).get());
    ASSERT_TRUE(knob != NULL);
    EXPECT_TRUE(knob->getValue());
}

TEST_F(NativeWriteMetadataTest, JpegKeepsTheExifFieldsItCanHold)
{
    ASSERT_TRUE(writeSourceJpeg(pathOf("src.0001.jpg")));
    buildChain(pathOf("src.####.jpg"), pathOf("out.jpg"));
    ASSERT_TRUE(bool(_write));

    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.jpg"));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ("NatronCam", spec->get_string_attribute("Make"));
    EXPECT_EQ("Model One", spec->get_string_attribute("Model"));
    EXPECT_FLOAT_EQ(2.f, spec->get_float_attribute("ExposureTime", -1.f));
    EXPECT_FLOAT_EQ(4.f, spec->get_float_attribute("FNumber", -1.f));
    EXPECT_EQ(400, spec->get_int_attribute("Exif:PhotographicSensitivity", -1));

    const ImageMetadata metadata = reread(pathOf("out.jpg"));
    EXPECT_EQ(std::optional<std::string>("NatronCam"), metadata.getString("exif/Make"));
    EXPECT_EQ(std::optional<double>(2.), metadata.getDouble("exif/ExposureTime"));
    EXPECT_EQ(std::optional<int>(400), metadata.getInt("exif/PhotographicSensitivity"));
}

TEST(OiioWriteMetadata, MapsEachPrefixToItsAttributeAndFormat)
{
    ImageMetadata metadata;

    metadata.setString("exr/custom", "a");
    metadata.setString("exif/Make", "cam");
    metadata.setDouble("exif/FocalLength", 35.);
    metadata.setString("tiff/Foo", "t");
    metadata.setString("dpx/Creator", "d");
    metadata.setInt("oiio/Custom", 3);
    metadata.setString("ofx/filepath", "/x");
    metadata.setInt("ofx/frame", 4);
    metadata.setDouble("ofx/framerate", 24.);
    metadata.setString("oiio/ColorSpace", "x");
    metadata.setString("exr/compression", "none");
    metadata.setString("tiff/Compression", "8");
    metadata.setString("dpx/Packing", "p");
    metadata.setString("ofx/edgecode", "1:2:3:4:5:6:7");

    OIIO::ImageSpec exr;
    OiioWriteSupport::applyMetadata(exr, metadata, "openexr");
    EXPECT_EQ("a", exr.get_string_attribute("custom"));
    EXPECT_EQ("cam", exr.get_string_attribute("Exif:Make"));
    EXPECT_FLOAT_EQ(35.f, exr.get_float_attribute("Exif:FocalLength", -1.f));
    EXPECT_EQ(3, exr.get_int_attribute("oiio:Custom", -1));
    EXPECT_TRUE(exr.find_attribute("tiff:Foo") == NULL);
    EXPECT_TRUE(exr.find_attribute("dpx:Creator") == NULL);
    EXPECT_TRUE(exr.find_attribute("compression") == NULL);
    EXPECT_TRUE(exr.find_attribute("oiio:ColorSpace") == NULL);
    EXPECT_TRUE(exr.find_attribute("smpte:KeyCode") != NULL);
    for (const OIIO::ParamValue& attribute : exr.extra_attribs) {
        const std::string name = attribute.name().string();
        EXPECT_EQ(std::string::npos, name.find("filepath")) << name;
        EXPECT_EQ(std::string::npos, name.find("framerate")) << name;
        EXPECT_EQ(std::string::npos, name.find("frame")) << name;
    }

    OIIO::ImageSpec tiff;
    OiioWriteSupport::applyMetadata(tiff, metadata, "tiff");
    EXPECT_EQ("t", tiff.get_string_attribute("tiff:Foo"));
    EXPECT_EQ("cam", tiff.get_string_attribute("Make"));
    EXPECT_TRUE(tiff.find_attribute("custom") == NULL);
    EXPECT_TRUE(tiff.find_attribute("tiff:Compression") == NULL);
    EXPECT_TRUE(tiff.find_attribute("Exif:Make") == NULL);
    EXPECT_FLOAT_EQ(35.f, tiff.get_float_attribute("Exif:FocalLength", -1.f));

    OIIO::ImageSpec dpx;
    OiioWriteSupport::applyMetadata(dpx, metadata, "dpx");
    EXPECT_EQ("d", dpx.get_string_attribute("dpx:Creator"));
    EXPECT_TRUE(dpx.find_attribute("dpx:Packing") == NULL);
    EXPECT_TRUE(dpx.find_attribute("tiff:Foo") == NULL);
}

TEST(OiioWriteMetadata, TimecodeAndEdgecodeAreRebuiltFromTheirStrings)
{
    ImageMetadata metadata;

    metadata.setString("ofx/timecode", "10:20:30;15");
    metadata.setString("ofx/edgecode", "12:34:056789:1234:05:4:64");

    OIIO::ImageSpec spec;
    OiioWriteSupport::applyMetadata(spec, metadata, "openexr");

    const OIIO::ParamValue* timecode = spec.find_attribute("smpte:TimeCode");
    ASSERT_TRUE(timecode != NULL);
    EXPECT_EQ(0x10203055u, static_cast<const unsigned int*>(timecode->data())[0]);

    const OIIO::ParamValue* keycode = spec.find_attribute("smpte:KeyCode");
    ASSERT_TRUE(keycode != NULL);
    const int* parts = static_cast<const int*>(keycode->data());
    EXPECT_EQ(12, parts[0]);
    EXPECT_EQ(34, parts[1]);
    EXPECT_EQ(56789, parts[2]);
    EXPECT_EQ(1234, parts[3]);
    EXPECT_EQ(5, parts[4]);
    EXPECT_EQ(4, parts[5]);
    EXPECT_EQ(64, parts[6]);
}

TEST_F(NativeWriteMetadataTest, EachFileOfASequenceTakesItsOwnFramesFormat)
{
    for (int frame = 1; frame <= 2; ++frame) {
        ASSERT_TRUE(writeSizedExr(pathOf("narrow.000" + std::to_string(frame) + ".exr"), 8, 6, 1.f, "narrow"));
        ASSERT_TRUE(writeSizedExr(pathOf("wide.000" + std::to_string(frame) + ".exr"), 16, 12, 2.f, "wide"));
    }
    NodePtr narrow = createRead(pathOf("narrow.####.exr"));
    NodePtr wide = createRead(pathOf("wide.####.exr"));
    NodePtr switchNode = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));
    _write = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
    ASSERT_TRUE(narrow && wide && switchNode && _write);
    connectNodes(narrow, switchNode, 0, true);
    connectNodes(wide, switchNode, 1, true);
    connectNodes(switchNode, _write, 0, true);
    KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName(kSwitchParamWhich));
    ASSERT_TRUE(bool(which));
    which->setValueAtTime(1, 0, ViewSpec::all(), 0);
    which->setValueAtTime(2, 1, ViewSpec::all(), 0);
    configureWrite(pathOf("out.####.exr"));

    // The key/value metadata follows the input the Switch routes to at each frame.
    NativeEffectBase* switchEffect = nativeEffectOf(switchNode);
    ASSERT_TRUE(switchEffect != NULL);
    EXPECT_EQ(std::optional<std::string>("narrow"), switchEffect->getOutputMetadata(1., ViewIdx(0)).getString("exr/mySource"));
    EXPECT_EQ(std::optional<std::string>("wide"), switchEffect->getOutputMetadata(2., ViewIdx(0)).getString("exr/mySource"));

    renderFrames(1, 2);

    const std::optional<OIIO::ImageSpec> first = specOf(pathOf("out.0001.exr"));
    const std::optional<OIIO::ImageSpec> second = specOf(pathOf("out.0002.exr"));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(8, first->full_width);
    EXPECT_EQ(6, first->full_height);
    EXPECT_FLOAT_EQ(1.f, first->get_float_attribute("PixelAspectRatio", 1.f));
    EXPECT_EQ(16, second->full_width);
    EXPECT_EQ(12, second->full_height);
    EXPECT_FLOAT_EQ(2.f, second->get_float_attribute("PixelAspectRatio", 1.f));
    EXPECT_EQ("narrow", first->get_string_attribute("mySource"));
    EXPECT_EQ("wide", second->get_string_attribute("mySource"));
}
