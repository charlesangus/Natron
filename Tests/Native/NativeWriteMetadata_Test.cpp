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
#include "Engine/Nodes/Metadata/ModifyMetadata.h"
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
        _modify.reset();
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
        fileInWorkingSpace(_write);

        KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(_write->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(file != NULL);
        file->setValue(output);
    }

    void setMetadataMode(int index)
    {
        KnobChoice* knob = dynamic_cast<KnobChoice*>(_write->getKnobByName(kNatronWriteParamMetadata).get());

        ASSERT_TRUE(knob != NULL);
        knob->setValue(index);
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

    void setSlot(int slot,
                 const std::string& key,
                 const std::string& value)
    {
        const std::string suffix = std::to_string(slot);
        KnobChoice* operation = dynamic_cast<KnobChoice*>(_modify->getKnobByName("operation" + suffix).get());
        KnobString* keyKnob = dynamic_cast<KnobString*>(_modify->getKnobByName("key" + suffix).get());
        KnobChoice* type = dynamic_cast<KnobChoice*>(_modify->getKnobByName("type" + suffix).get());
        KnobString* stringValue = dynamic_cast<KnobString*>(_modify->getKnobByName("stringValue" + suffix).get());

        ASSERT_TRUE(operation && keyKnob && type && stringValue);
        operation->setValue((int)ModifyMetadata::eOperationSet, ViewSpec::all(), 0);
        keyKnob->setValue(key, ViewSpec::all(), 0);
        type->setValue((int)ModifyMetadata::eValueTypeString, ViewSpec::all(), 0);
        stringValue->setValue(value, ViewSpec::all(), 0);
    }

    void setDoubleSlot(int slot,
                       const std::string& key,
                       double value)
    {
        const std::string suffix = std::to_string(slot);
        KnobChoice* operation = dynamic_cast<KnobChoice*>(_modify->getKnobByName("operation" + suffix).get());
        KnobString* keyKnob = dynamic_cast<KnobString*>(_modify->getKnobByName("key" + suffix).get());
        KnobChoice* type = dynamic_cast<KnobChoice*>(_modify->getKnobByName("type" + suffix).get());
        KnobDouble* doubleValue = dynamic_cast<KnobDouble*>(_modify->getKnobByName("doubleValue" + suffix).get());

        ASSERT_TRUE(operation && keyKnob && type && doubleValue);
        operation->setValue((int)ModifyMetadata::eOperationSet, ViewSpec::all(), 0);
        keyKnob->setValue(key, ViewSpec::all(), 0);
        type->setValue((int)ModifyMetadata::eValueTypeDouble, ViewSpec::all(), 0);
        doubleValue->setValue(value, ViewSpec::all(), 0);
    }

    void buildEditedChain(const std::string& output,
                          int mode)
    {
        ASSERT_TRUE(writeSizedExr(pathOf("src.0001.exr"), kSize, kSize, 1.f, "src"));
        _read = createRead(pathOf("src.####.exr"));
        _modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
        _dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
        _write = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
        ASSERT_TRUE(_read && _modify && _dot && _write);

        connectNodes(_read, _modify, 0, true);
        connectNodes(_modify, _dot, 0, true);
        connectNodes(_dot, _write, 0, true);
        ASSERT_NO_FATAL_FAILURE(setSlot(1, "shot", "sh010"));
        ASSERT_NO_FATAL_FAILURE(setSlot(2, "ofx/comment", "a note"));
        ASSERT_NO_FATAL_FAILURE(setSlot(3, "ofx/creator", "test tool"));
        ASSERT_NO_FATAL_FAILURE(setDoubleSlot(4, "exr/gain", 0.1));
        ASSERT_NO_FATAL_FAILURE(setSlot(5, "compression", "none"));
        ASSERT_NO_FATAL_FAILURE(setSlot(6, "name", "structural"));
        configureWrite(output);
        setMetadataMode(mode);
    }

    QTemporaryDir _dir;
    NodePtr _read;
    NodePtr _modify;
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

TEST_F(NativeWriteMetadataTest, NoMetadataWritesNoneOfTheCustomAttributes)
{
    ASSERT_TRUE(writeSourceExr(pathOf("src.0001.exr")));
    buildChain(pathOf("src.####.exr"), pathOf("out.exr"));
    ASSERT_TRUE(bool(_write));
    setMetadataMode(0);

    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.exr"));
    ASSERT_TRUE(spec.has_value());
    for (const char* name : { "myString", "myInt", "myFloat", "myVec", "Exif:Make", "FramesPerSecond", "smpte:TimeCode" }) {
        EXPECT_TRUE(spec->find_attribute(name) == NULL) << name;
    }
}

TEST_F(NativeWriteMetadataTest, MetadataDefaultsToAllExceptInput)
{
    NodePtr write = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
    ASSERT_TRUE(bool(write));
    KnobChoice* knob = dynamic_cast<KnobChoice*>(write->getKnobByName(kNatronWriteParamMetadata).get());
    ASSERT_TRUE(knob != NULL);
    EXPECT_EQ(2, knob->getValue());
    EXPECT_EQ(std::string(kNatronWriteMetadataAllExceptInput), knob->getActiveEntry().id);
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
    OiioWriteSupport::applyMetadata(exr, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::Default);
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
    OiioWriteSupport::applyMetadata(tiff, metadata, "tiff", OiioWriteSupport::WriteMetadataMode::Default);
    EXPECT_EQ("t", tiff.get_string_attribute("tiff:Foo"));
    EXPECT_EQ("cam", tiff.get_string_attribute("Make"));
    EXPECT_TRUE(tiff.find_attribute("custom") == NULL);
    EXPECT_TRUE(tiff.find_attribute("tiff:Compression") == NULL);
    EXPECT_TRUE(tiff.find_attribute("Exif:Make") == NULL);
    EXPECT_FLOAT_EQ(35.f, tiff.get_float_attribute("Exif:FocalLength", -1.f));

    OIIO::ImageSpec dpx;
    OiioWriteSupport::applyMetadata(dpx, metadata, "dpx", OiioWriteSupport::WriteMetadataMode::Default);
    EXPECT_EQ("d", dpx.get_string_attribute("dpx:Creator"));
    EXPECT_TRUE(dpx.find_attribute("dpx:Packing") == NULL);
    EXPECT_TRUE(dpx.find_attribute("tiff:Foo") == NULL);
}

TEST(OiioWriteMetadata, StandardOfxKeysMapToTheirTagsAndNativeKeysWin)
{
    ImageMetadata metadata;

    metadata.setString("ofx/creator", "tool");
    metadata.setString("ofx/copyright", "me");
    metadata.setString("ofx/comment", "note");
    metadata.setDouble("ofx/framerate", 24.);
    metadata.setDouble("ofx/focallength", 50.);
    metadata.setDouble("ofx/fnumber", 2.8);
    metadata.setDouble("ofx/exposuretime", 0.5);

    OIIO::ImageSpec tiff;
    OiioWriteSupport::applyMetadata(tiff, metadata, "tiff", OiioWriteSupport::WriteMetadataMode::Default);
    EXPECT_EQ("tool", tiff.get_string_attribute("Software"));
    EXPECT_EQ("me", tiff.get_string_attribute("Copyright"));
    EXPECT_EQ("note", tiff.get_string_attribute("ImageDescription"));
    EXPECT_TRUE(tiff.find_attribute("FramesPerSecond") != NULL);
    EXPECT_FLOAT_EQ(50.f, tiff.get_float_attribute("Exif:FocalLength", -1.f));
    EXPECT_FLOAT_EQ(2.8f, tiff.get_float_attribute("FNumber", -1.f));
    EXPECT_FLOAT_EQ(0.5f, tiff.get_float_attribute("ExposureTime", -1.f));

    OIIO::ImageSpec exr;
    OiioWriteSupport::applyMetadata(exr, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::Default);
    EXPECT_EQ("tool", exr.get_string_attribute("Software"));
    EXPECT_EQ("me", exr.get_string_attribute("Copyright"));
    EXPECT_EQ("note", exr.get_string_attribute("ImageDescription"));
    EXPECT_FLOAT_EQ(2.8f, exr.get_float_attribute("Exif:FNumber", -1.f));
    EXPECT_TRUE(exr.find_attribute("Exif:Software") == NULL);
    EXPECT_TRUE(exr.find_attribute("Exif:Copyright") == NULL);
    EXPECT_TRUE(exr.find_attribute("Exif:ImageDescription") == NULL);

    metadata.setString("exif/Software", "native");
    OIIO::ImageSpec both;
    OiioWriteSupport::applyMetadata(both, metadata, "tiff", OiioWriteSupport::WriteMetadataMode::Default);
    EXPECT_EQ("native", both.get_string_attribute("Software"));
}

TEST(OiioWriteMetadata, ExrKeysBeatOfxKeysUnderEverySpelling)
{
    ImageMetadata metadata;

    metadata.setString("ofx/creator", "ofx");
    metadata.setString("ofx/copyright", "ofx");
    metadata.setString("ofx/comment", "ofx");
    metadata.setDouble("ofx/fnumber", 8.);
    metadata.setDouble("ofx/exposuretime", 8.);
    metadata.setString("exif/Software", "native");
    metadata.setString("exr/owner", "native");
    metadata.setString("exr/ImageDescription", "native");
    metadata.setDouble("exr/FNumber", 2.);
    metadata.setDouble("exif/ExposureTime", 2.);

    OIIO::ImageSpec exr;
    OiioWriteSupport::applyMetadata(exr, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::Default);
    EXPECT_EQ("native", exr.get_string_attribute("Exif:Software"));
    EXPECT_TRUE(exr.find_attribute("Software") == NULL);
    EXPECT_EQ("native", exr.get_string_attribute("owner"));
    EXPECT_TRUE(exr.find_attribute("Copyright") == NULL);
    EXPECT_EQ("native", exr.get_string_attribute("ImageDescription"));
    EXPECT_TRUE(exr.find_attribute("Exif:ImageDescription") == NULL);
    EXPECT_FLOAT_EQ(2.f, exr.get_float_attribute("FNumber", -1.f));
    EXPECT_TRUE(exr.find_attribute("Exif:FNumber") == NULL);
    EXPECT_FLOAT_EQ(2.f, exr.get_float_attribute("Exif:ExposureTime", -1.f));
    EXPECT_TRUE(exr.find_attribute("ExposureTime") == NULL);
}

TEST(OiioWriteMetadata, ExrNumbersAreFloatWhenAFloatHoldsThemOrTheNameRequiresIt)
{
    ImageMetadata metadata;

    metadata.setDouble("exr/widened", (double)0.1f);
    metadata.setDouble("exr/precise", 0.1);
    metadata.setDouble("precise", 0.1);
    metadata.setDouble("widened", (double)0.1f);
    metadata.setDouble("huge", 1e300);
    metadata.setDouble("exr/ExposureTime", 0.1);
    metadata.setDouble("exr/FNumber", 0.1);
    metadata.setDouble("exr/XResolution", 0.1);
    metadata.setDouble("exr/screenWindowWidth", 0.1);

    OIIO::ImageSpec exr;
    OiioWriteSupport::applyMetadata(exr, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::AllExceptInput);
    for (const char* name : { "widened", "ExposureTime", "FNumber", "XResolution", "screenWindowWidth" }) {
        const OIIO::ParamValue* attribute = exr.find_attribute(name);
        ASSERT_TRUE(attribute != NULL) << name;
        EXPECT_EQ(OIIO::TypeFloat, attribute->type()) << name;
    }
    for (const char* name : { "precise", "huge" }) {
        const OIIO::ParamValue* attribute = exr.find_attribute(name);
        ASSERT_TRUE(attribute != NULL) << name;
        EXPECT_EQ(OIIO::TypeDesc::DOUBLE, attribute->type().basetype) << name;
    }
}

TEST(OiioWriteMetadata, ExrDoubleVectorsStayDoubleWhereTheFormatHasTheType)
{
    ImageMetadata metadata;
    const std::vector<double> matrix = { 0.1, 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1. };

    metadata.setDoubleVector("exr/precise2", { 0.1, 0.2 });
    metadata.setDoubleVector("exr/precise3", { 0.1, 0.2, 0.3 });
    metadata.setDoubleVector("exr/precise9", { 0.1, 0., 0., 0., 1., 0., 0., 0., 1. });
    metadata.setDoubleVector("exr/precise16", matrix);
    metadata.setDoubleVector("exr/widened3", { 0.5, 1.5, 2.5 });
    metadata.setDoubleVector("exr/precise4", { 0.1, 0.2, 0.3, 0.4 });
    metadata.setDoubleVector("exr/screenWindowCenter", { 0.1, 0.2 });
    metadata.setDoubleVector("exr/worldToCamera", matrix);

    OIIO::ImageSpec exr;
    OiioWriteSupport::applyMetadata(exr, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::Default);
    for (const char* name : { "precise2", "precise3", "precise9", "precise16" }) {
        const OIIO::ParamValue* attribute = exr.find_attribute(name);
        ASSERT_TRUE(attribute != NULL) << name;
        EXPECT_EQ(OIIO::TypeDesc::DOUBLE, attribute->type().basetype) << name;
    }
    EXPECT_DOUBLE_EQ(0.3, static_cast<const double*>(exr.find_attribute("precise3")->data())[2]);
    for (const char* name : { "widened3", "precise4", "screenWindowCenter", "worldToCamera" }) {
        const OIIO::ParamValue* attribute = exr.find_attribute(name);
        ASSERT_TRUE(attribute != NULL) << name;
        EXPECT_EQ(OIIO::TypeDesc::FLOAT, attribute->type().basetype) << name;
    }
}

TEST(OiioWriteMetadata, StructuralNamesAndWriterDirectivesAreExcludedWhateverTheirCase)
{
    ImageMetadata metadata;

    metadata.setString("Compression", "none");
    metadata.setString("COMPRESSION", "none");
    metadata.setString("exr/Compression", "none");
    metadata.setString("openexr:lineOrder", "decreasingY");
    metadata.setString("OpenEXR:LineOrder", "decreasingY");
    metadata.setString("exr/openexr:lineOrder", "decreasingY");
    metadata.setString("oiio:subimagename", "part");
    metadata.setString("exr/oiio:subimagename", "part");
    metadata.setString("Name", "part");
    metadata.setString("exr/textureformat", "Plain Texture");
    metadata.setInt("acesImageContainerFlag", 1);
    metadata.setString("oiio/colorspace", "x");

    for (const char* format : { "openexr", "png" }) {
        OIIO::ImageSpec spec;
        OiioWriteSupport::applyMetadata(spec, metadata, format, OiioWriteSupport::WriteMetadataMode::All);
        for (const OIIO::ParamValue& attribute : spec.extra_attribs) {
            ADD_FAILURE() << format << " took " << attribute.name().string();
        }
    }
}

TEST(OiioWriteMetadata, TimecodesPngCannotHoldAndUnparseableOnesAreWrittenAsText)
{
    ImageMetadata metadata;

    metadata.setString("ofx/timecode", "01:02:03:04");
    metadata.setString("ofx/edgecode", "1:2:3:4:5:6:7");

    OIIO::ImageSpec png;
    OiioWriteSupport::applyMetadata(png, metadata, "png", OiioWriteSupport::WriteMetadataMode::AllExceptInput);
    EXPECT_EQ("01:02:03:04", png.get_string_attribute("ofx/timecode"));
    EXPECT_EQ("1:2:3:4:5:6:7", png.get_string_attribute("ofx/edgecode"));
    EXPECT_TRUE(png.find_attribute("smpte:TimeCode") == NULL);
    EXPECT_TRUE(png.find_attribute("smpte:KeyCode") == NULL);

    OIIO::ImageSpec pngDefault;
    OiioWriteSupport::applyMetadata(pngDefault, metadata, "png", OiioWriteSupport::WriteMetadataMode::Default);
    EXPECT_TRUE(pngDefault.extra_attribs.empty());

    OIIO::ImageSpec parsed;
    OiioWriteSupport::applyMetadata(parsed, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::AllExceptInput);
    EXPECT_TRUE(parsed.find_attribute("smpte:TimeCode") != NULL);
    EXPECT_TRUE(parsed.find_attribute("ofx/timecode") == NULL);

    metadata.setString("ofx/timecode", "not a timecode");
    metadata.setString("ofx/edgecode", "nor an edgecode");
    OIIO::ImageSpec unparseable;
    OiioWriteSupport::applyMetadata(unparseable, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::AllExceptInput);
    EXPECT_EQ("not a timecode", unparseable.get_string_attribute("ofx/timecode"));
    EXPECT_EQ("nor an edgecode", unparseable.get_string_attribute("ofx/edgecode"));
    EXPECT_TRUE(unparseable.find_attribute("smpte:TimeCode") == NULL);
    EXPECT_TRUE(unparseable.find_attribute("smpte:KeyCode") == NULL);

    OIIO::ImageSpec unparseableDefault;
    OiioWriteSupport::applyMetadata(unparseableDefault, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::Default);
    EXPECT_TRUE(unparseableDefault.extra_attribs.empty());
}

TEST(OiioWriteMetadata, CustomOiioKeysGoUnderTheirFullKeyInTheAllMetadataModes)
{
    ImageMetadata metadata;

    metadata.setString("oiio/Custom", "kept");
    metadata.setString("oiio/ColorSpace", "x");

    for (const char* format : { "openexr", "png" }) {
        OIIO::ImageSpec spec;
        OiioWriteSupport::applyMetadata(spec, metadata, format, OiioWriteSupport::WriteMetadataMode::AllExceptInput);
        EXPECT_EQ("kept", spec.get_string_attribute("oiio/Custom")) << format;
        EXPECT_TRUE(spec.find_attribute("oiio:Custom") == NULL) << format;
        EXPECT_TRUE(spec.find_attribute("oiio/ColorSpace") == NULL) << format;
    }

    OIIO::ImageSpec defaults;
    OiioWriteSupport::applyMetadata(defaults, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::Default);
    EXPECT_TRUE(defaults.find_attribute("oiio/Custom") == NULL);

    OIIO::ImageSpec tiff;
    OiioWriteSupport::applyMetadata(tiff, metadata, "tiff", OiioWriteSupport::WriteMetadataMode::AllExceptInput);
    EXPECT_EQ("kept", tiff.get_string_attribute("oiio:Custom"));
    EXPECT_TRUE(tiff.find_attribute("oiio/Custom") == NULL);
}

TEST(OiioWriteMetadata, SourceLayoutOfxKeysAreWrittenOnlyInAllMetadata)
{
    ImageMetadata metadata;
    const char* const layoutKeys[] = { "ofx/pixelaspect", "ofx/width", "ofx/height", "ofx/bitdepth", "ofx/sampletype", "ofx/viewnames" };

    metadata.setDouble("ofx/pixelaspect", 2.);
    metadata.setInt("ofx/width", 1920);
    metadata.setInt("ofx/height", 1080);
    metadata.setInt("ofx/bitdepth", 16);
    metadata.setString("ofx/sampletype", "float");
    metadata.setString("ofx/viewnames", "main");

    for (const char* format : { "openexr", "png" }) {
        OIIO::ImageSpec except;
        OiioWriteSupport::applyMetadata(except, metadata, format, OiioWriteSupport::WriteMetadataMode::AllExceptInput);
        EXPECT_TRUE(except.extra_attribs.empty()) << format;

        OIIO::ImageSpec defaults;
        OiioWriteSupport::applyMetadata(defaults, metadata, format, OiioWriteSupport::WriteMetadataMode::Default);
        EXPECT_TRUE(defaults.extra_attribs.empty()) << format;

        OIIO::ImageSpec all;
        OiioWriteSupport::applyMetadata(all, metadata, format, OiioWriteSupport::WriteMetadataMode::All);
        for (const char* key : layoutKeys) {
            EXPECT_TRUE(all.find_attribute(key) != NULL) << format << " " << key;
        }
    }
}

TEST(OiioWriteMetadata, AllExceptInputWritesRemainingKeysVerbatimToExrAndPng)
{
    ImageMetadata metadata;

    metadata.setString("shot", "sh010");
    metadata.setString("ofx/creator", "tool");
    metadata.setDouble("ofx/custom", 0.1);
    metadata.setInt("ofx/count", 7);
    metadata.setString("exr/Lens", "50mm");
    metadata.setString("quicktime/Reel", "r1");
    metadata.setString("ofx/filepath", "/x");
    metadata.setInt("ofx/frame", 4);
    metadata.setDouble("ofx/filesize", 10.);
    metadata.setDouble("ofx/mtime", 11.);
    metadata.setString("exr/compression", "none");
    metadata.setString("compression", "zip");
    metadata.setString("name", "n");

    OIIO::ImageSpec exr;
    OiioWriteSupport::applyMetadata(exr, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::AllExceptInput);
    EXPECT_EQ("sh010", exr.get_string_attribute("shot"));
    EXPECT_EQ("tool", exr.get_string_attribute("ofx/creator"));
    EXPECT_EQ("tool", exr.get_string_attribute("Software"));
    EXPECT_EQ("50mm", exr.get_string_attribute("Lens"));
    EXPECT_EQ("r1", exr.get_string_attribute("quicktime/Reel"));
    const OIIO::ParamValue* custom = exr.find_attribute("ofx/custom");
    ASSERT_TRUE(custom != NULL);
    EXPECT_EQ(OIIO::TypeDesc::DOUBLE, custom->type().basetype);
    EXPECT_DOUBLE_EQ(0.1, *(const double*)custom->data());
    EXPECT_EQ(7, exr.get_int_attribute("ofx/count", -1));
    EXPECT_TRUE(exr.find_attribute("ofx/filepath") == NULL);
    EXPECT_TRUE(exr.find_attribute("ofx/frame") == NULL);
    EXPECT_TRUE(exr.find_attribute("ofx/filesize") == NULL);
    EXPECT_TRUE(exr.find_attribute("ofx/mtime") == NULL);
    EXPECT_TRUE(exr.find_attribute("compression") == NULL);
    EXPECT_TRUE(exr.find_attribute("name") == NULL);

    OIIO::ImageSpec png;
    OiioWriteSupport::applyMetadata(png, metadata, "png", OiioWriteSupport::WriteMetadataMode::AllExceptInput);
    EXPECT_EQ("sh010", png.get_string_attribute("shot"));
    EXPECT_EQ("tool", png.get_string_attribute("ofx/creator"));
    EXPECT_EQ("50mm", png.get_string_attribute("exr/Lens"));
    EXPECT_EQ("7", png.get_string_attribute("ofx/count"));
    const OIIO::ParamValue* text = png.find_attribute("ofx/custom");
    ASSERT_TRUE(text != NULL);
    EXPECT_EQ(OIIO::TypeDesc::STRING, text->type().basetype);
    EXPECT_DOUBLE_EQ(0.1, std::stod(png.get_string_attribute("ofx/custom")));
    EXPECT_TRUE(png.find_attribute("exr/compression") == NULL);
    EXPECT_TRUE(png.find_attribute("compression") == NULL);
    EXPECT_TRUE(png.find_attribute("ofx/filepath") == NULL);

    OIIO::ImageSpec tiff;
    OiioWriteSupport::applyMetadata(tiff, metadata, "tiff", OiioWriteSupport::WriteMetadataMode::AllExceptInput);
    EXPECT_TRUE(tiff.find_attribute("shot") == NULL);
    EXPECT_TRUE(tiff.find_attribute("ofx/creator") == NULL);
    EXPECT_TRUE(tiff.find_attribute("exr/Lens") == NULL);
    EXPECT_EQ("tool", tiff.get_string_attribute("Software"));
}

TEST(OiioWriteMetadata, AllAddsTheInputIdentityKeysAndDefaultAddsNoCustomKeys)
{
    ImageMetadata metadata;

    metadata.setString("shot", "sh010");
    metadata.setString("ofx/filepath", "/x");
    metadata.setInt("ofx/frame", 4);
    metadata.setDouble("ofx/filesize", 10.);
    metadata.setDouble("ofx/mtime", 11.);
    metadata.setString("oiio/ColorSpace", "x");
    metadata.setString("oiio/colorInteropID", "lin_rec709_scene");
    metadata.setString("exr/colorInteropID", "lin_rec709_scene");
    metadata.setString("tiff/Compression", "8");

    OIIO::ImageSpec exr;
    OiioWriteSupport::applyMetadata(exr, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::All);
    EXPECT_EQ("sh010", exr.get_string_attribute("shot"));
    EXPECT_EQ("/x", exr.get_string_attribute("ofx/filepath"));
    EXPECT_EQ(4, exr.get_int_attribute("ofx/frame", -1));
    EXPECT_TRUE(exr.find_attribute("ofx/filesize") != NULL);
    EXPECT_TRUE(exr.find_attribute("ofx/mtime") != NULL);
    EXPECT_TRUE(exr.find_attribute("oiio/ColorSpace") == NULL);
    EXPECT_TRUE(exr.find_attribute("oiio:ColorSpace") == NULL);
    EXPECT_TRUE(exr.find_attribute("oiio/colorInteropID") == NULL);
    EXPECT_TRUE(exr.find_attribute("oiio:colorInteropID") == NULL);
    EXPECT_TRUE(exr.find_attribute("exr/colorInteropID") == NULL);
    EXPECT_TRUE(exr.find_attribute("colorInteropID") == NULL);
    EXPECT_TRUE(exr.find_attribute("tiff/Compression") == NULL);

    OIIO::ImageSpec png;
    OiioWriteSupport::applyMetadata(png, metadata, "png", OiioWriteSupport::WriteMetadataMode::All);
    EXPECT_EQ("/x", png.get_string_attribute("ofx/filepath"));
    EXPECT_EQ("4", png.get_string_attribute("ofx/frame"));

    OIIO::ImageSpec tiff;
    OiioWriteSupport::applyMetadata(tiff, metadata, "tiff", OiioWriteSupport::WriteMetadataMode::All);
    EXPECT_TRUE(tiff.find_attribute("ofx/filepath") == NULL);

    OIIO::ImageSpec defaults;
    OiioWriteSupport::applyMetadata(defaults, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::Default);
    EXPECT_TRUE(defaults.find_attribute("shot") == NULL);
    EXPECT_TRUE(defaults.find_attribute("ofx/filepath") == NULL);
}

TEST(OiioWriteMetadata, TimecodeAndEdgecodeAreRebuiltFromTheirStrings)
{
    ImageMetadata metadata;

    metadata.setString("ofx/timecode", "10:20:30;15");
    metadata.setString("ofx/edgecode", "12:34:056789:1234:05:4:64");

    OIIO::ImageSpec spec;
    OiioWriteSupport::applyMetadata(spec, metadata, "openexr", OiioWriteSupport::WriteMetadataMode::Default);

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

namespace {
const int kModeNone = 0;
const int kModeDefault = 1;
const int kModeAllExceptInput = 2;
const int kModeAll = 3;

const char* const kIdentityKeys[] = { "ofx/filepath", "ofx/frame", "ofx/filesize", "ofx/mtime" };
} // namespace

TEST_F(NativeWriteMetadataTest, ExrDefaultModeWritesStandardTagsOnly)
{
    ASSERT_NO_FATAL_FAILURE(buildEditedChain(pathOf("out.exr"), kModeDefault));
    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.exr"));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ("a note", spec->get_string_attribute("ImageDescription"));
    EXPECT_EQ("test tool", spec->get_string_attribute("Software"));
    EXPECT_TRUE(spec->find_attribute("Exif:ImageDescription") == NULL);
    EXPECT_TRUE(spec->find_attribute("Exif:Software") == NULL);
    EXPECT_TRUE(spec->find_attribute("shot") == NULL);
    EXPECT_TRUE(spec->find_attribute("ofx/comment") == NULL);
    EXPECT_TRUE(spec->find_attribute("ofx/creator") == NULL);

    const ImageMetadata metadata = reread(pathOf("out.exr"));
    EXPECT_EQ(std::optional<std::string>("a note"), metadata.getString("exr/ImageDescription"));
    EXPECT_EQ(std::optional<std::string>("test tool"), metadata.getString("exr/Software"));
    EXPECT_FALSE(metadata.getString("exr/shot").has_value());
}

TEST_F(NativeWriteMetadataTest, ExrFloatAttributesSurviveAReadWriteRoundTripAsFloats)
{
    OIIO::ImageSpec source(kSize, kSize, 4, OIIO::TypeDesc::FLOAT);
    source.attribute("compression", "none");
    source.attribute("ExposureTime", 0.01f);
    source.attribute("FNumber", 2.8f);
    source.attribute("screenWindowWidth", 2.5f);
    source.attribute("myCustomFloat", 0.1f);
    ASSERT_TRUE(writeImage(pathOf("src.0001.exr"), source));

    for (int mode : { kModeDefault, kModeAllExceptInput, kModeAll }) {
        const std::string output = "out" + std::to_string(mode) + ".exr";
        buildChain(pathOf("src.####.exr"), pathOf(output));
        ASSERT_TRUE(bool(_write));
        setMetadataMode(mode);

        renderFrame();

        const std::optional<OIIO::ImageSpec> spec = specOf(pathOf(output));
        ASSERT_TRUE(spec.has_value());
        const struct {
            const char* name;
            float value;
        } expected[] = { { "ExposureTime", 0.01f }, { "FNumber", 2.8f }, { "screenWindowWidth", 2.5f }, { "myCustomFloat", 0.1f } };
        for (const auto& attribute : expected) {
            const OIIO::ParamValue* written = spec->find_attribute(attribute.name);
            ASSERT_TRUE(written != NULL) << attribute.name << " in mode " << mode;
            EXPECT_EQ(OIIO::TypeFloat, written->type()) << attribute.name << " in mode " << mode;
            EXPECT_EQ(attribute.value, *static_cast<const float*>(written->data())) << attribute.name << " in mode " << mode;
        }

        const ImageMetadata metadata = reread(pathOf(output));
        EXPECT_EQ(std::optional<double>((double)0.01f), metadata.getDouble("exr/ExposureTime"));
        EXPECT_EQ(std::optional<double>((double)2.8f), metadata.getDouble("exr/FNumber"));
        EXPECT_EQ(std::optional<double>(2.5), metadata.getDouble("exr/screenWindowWidth"));
        EXPECT_EQ(std::optional<double>((double)0.1f), metadata.getDouble("exr/myCustomFloat"));
    }
}

TEST_F(NativeWriteMetadataTest, ExrKeyBeatsTheOfxKeyInTheFile)
{
    ASSERT_NO_FATAL_FAILURE(buildEditedChain(pathOf("out.exr"), kModeDefault));
    ASSERT_NO_FATAL_FAILURE(setSlot(7, "exr/ImageDescription", "native note"));
    ASSERT_NO_FATAL_FAILURE(setSlot(8, "exr/Software", "native tool"));
    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.exr"));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ("native note", spec->get_string_attribute("ImageDescription"));
    EXPECT_EQ("native tool", spec->get_string_attribute("Software"));
    EXPECT_TRUE(spec->find_attribute("Exif:ImageDescription") == NULL);
    EXPECT_TRUE(spec->find_attribute("Exif:Software") == NULL);

    const ImageMetadata metadata = reread(pathOf("out.exr"));
    EXPECT_EQ(std::optional<std::string>("native note"), metadata.getString("exr/ImageDescription"));
    EXPECT_EQ(std::optional<std::string>("native tool"), metadata.getString("exr/Software"));
    EXPECT_FALSE(metadata.getString("exif/ImageDescription").has_value());
    EXPECT_FALSE(metadata.getString("exif/Software").has_value());
}

TEST_F(NativeWriteMetadataTest, OutputHashIsNeverOverwrittenByAnInputKey)
{
    ASSERT_NO_FATAL_FAILURE(buildEditedChain(pathOf("out.exr"), kModeAllExceptInput));
    ASSERT_NO_FATAL_FAILURE(setSlot(7, "natron:outputHash", "stale"));
    ASSERT_NO_FATAL_FAILURE(setSlot(8, std::string("exr/") + kNatronWriteOutputHashAttribute, "stale"));
    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.exr"));
    ASSERT_TRUE(spec.has_value());
    const std::string hash = spec->get_string_attribute(kNatronWriteOutputHashAttribute);
    EXPECT_FALSE(hash.empty());
    EXPECT_NE("stale", hash);
}

TEST_F(NativeWriteMetadataTest, ExrAllExceptInputRoundTripsEveryKeyWithoutTheInputIdentity)
{
    ASSERT_NO_FATAL_FAILURE(buildEditedChain(pathOf("out.exr"), kModeAllExceptInput));
    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.exr"));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ("sh010", spec->get_string_attribute("shot"));
    EXPECT_EQ("a note", spec->get_string_attribute("ofx/comment"));
    EXPECT_EQ("test tool", spec->get_string_attribute("ofx/creator"));
    const OIIO::ParamValue* gain = spec->find_attribute("gain");
    ASSERT_TRUE(gain != NULL);
    EXPECT_EQ(OIIO::TypeDesc::DOUBLE, gain->type().basetype);
    EXPECT_DOUBLE_EQ(0.1, *(const double*)gain->data());
    for (const char* key : kIdentityKeys) {
        EXPECT_TRUE(spec->find_attribute(key) == NULL) << key;
    }
    EXPECT_NE("none", spec->get_string_attribute("compression"));
    EXPECT_TRUE(spec->find_attribute("name") == NULL);

    const ImageMetadata metadata = reread(pathOf("out.exr"));
    EXPECT_EQ(std::optional<std::string>("sh010"), metadata.getString("exr/shot"));
    EXPECT_EQ(std::optional<std::string>("a note"), metadata.getString("ofx/comment"));
    EXPECT_EQ(std::optional<std::string>("test tool"), metadata.getString("ofx/creator"));
    EXPECT_EQ(std::optional<double>(0.1), metadata.getDouble("exr/gain"));
    EXPECT_FALSE(metadata.getString("exr/name").has_value());
    EXPECT_TRUE(spec->find_attribute("ofx/pixelaspect") == NULL);
}

TEST_F(NativeWriteMetadataTest, ExrAllWritesTheInputIdentityToo)
{
    ASSERT_NO_FATAL_FAILURE(buildEditedChain(pathOf("out.exr"), kModeAll));
    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.exr"));
    ASSERT_TRUE(spec.has_value());
    EXPECT_NE(std::string::npos, spec->get_string_attribute("ofx/filepath").find("src.0001.exr"));
    EXPECT_TRUE(spec->find_attribute("ofx/frame") != NULL);
    EXPECT_TRUE(spec->find_attribute("ofx/filesize") != NULL);
    EXPECT_TRUE(spec->find_attribute("ofx/mtime") != NULL);
    EXPECT_TRUE(spec->find_attribute("ofx/pixelaspect") != NULL);
    EXPECT_EQ("sh010", spec->get_string_attribute("shot"));
    EXPECT_TRUE(spec->find_attribute("name") == NULL);
}

TEST_F(NativeWriteMetadataTest, ExrNoMetadataWritesNoneOfTheEditedKeys)
{
    ASSERT_NO_FATAL_FAILURE(buildEditedChain(pathOf("out.exr"), kModeNone));
    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.exr"));
    ASSERT_TRUE(spec.has_value());
    for (const char* name : { "shot", "ofx/comment", "ofx/creator", "gain", "ImageDescription", "Software", "Exif:ImageDescription", "Exif:Software", "ofx/filepath", "name" }) {
        EXPECT_TRUE(spec->find_attribute(name) == NULL) << name;
    }
}

TEST_F(NativeWriteMetadataTest, PngDefaultModeWritesStandardTagsOnly)
{
    ASSERT_NO_FATAL_FAILURE(buildEditedChain(pathOf("out.png"), kModeDefault));
    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.png"));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ("a note", spec->get_string_attribute("ImageDescription"));
    EXPECT_EQ("test tool", spec->get_string_attribute("Software"));
    EXPECT_TRUE(spec->find_attribute("shot") == NULL);
    EXPECT_TRUE(spec->find_attribute("ofx/comment") == NULL);

    const ImageMetadata metadata = reread(pathOf("out.png"));
    EXPECT_EQ(std::optional<std::string>("a note"), metadata.getString("exif/ImageDescription"));
    EXPECT_EQ(std::optional<std::string>("test tool"), metadata.getString("exif/Software"));
    EXPECT_FALSE(metadata.getString("exif/shot").has_value());
    EXPECT_FALSE(metadata.getString("ofx/comment").has_value());
    EXPECT_FALSE(metadata.getString("ofx/creator").has_value());
}

TEST_F(NativeWriteMetadataTest, PngAllExceptInputKeepsEveryKeyAsTextWithoutTheInputIdentity)
{
    ASSERT_NO_FATAL_FAILURE(buildEditedChain(pathOf("out.png"), kModeAllExceptInput));
    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.png"));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ("sh010", spec->get_string_attribute("shot"));
    EXPECT_EQ("a note", spec->get_string_attribute("ofx/comment"));
    EXPECT_EQ("test tool", spec->get_string_attribute("ofx/creator"));
    EXPECT_DOUBLE_EQ(0.1, std::stod(spec->get_string_attribute("exr/gain")));
    for (const char* key : kIdentityKeys) {
        EXPECT_TRUE(spec->find_attribute(key) == NULL) << key;
    }
    EXPECT_TRUE(spec->find_attribute("ofx/pixelaspect") == NULL);
    EXPECT_TRUE(spec->find_attribute("DocumentName") == NULL);

    const ImageMetadata metadata = reread(pathOf("out.png"));
    EXPECT_EQ(std::optional<std::string>("sh010"), metadata.getString("exif/shot"));
    EXPECT_EQ(std::optional<std::string>("a note"), metadata.getString("ofx/comment"));
    EXPECT_EQ(std::optional<std::string>("test tool"), metadata.getString("ofx/creator"));
    EXPECT_EQ(std::optional<std::string>("a note"), metadata.getString("exif/ImageDescription"));
    EXPECT_EQ(std::optional<std::string>("test tool"), metadata.getString("exif/Software"));
    const std::optional<std::string> gain = metadata.getString("exr/gain");
    ASSERT_TRUE(gain.has_value());
    EXPECT_DOUBLE_EQ(0.1, std::stod(*gain));
    EXPECT_FALSE(metadata.getString("exif/DocumentName").has_value());
}

TEST_F(NativeWriteMetadataTest, PngAllWritesTheInputIdentityToo)
{
    ASSERT_NO_FATAL_FAILURE(buildEditedChain(pathOf("out.png"), kModeAll));
    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.png"));
    ASSERT_TRUE(spec.has_value());
    EXPECT_NE(std::string::npos, spec->get_string_attribute("ofx/filepath").find("src.0001.exr"));
    EXPECT_TRUE(spec->find_attribute("ofx/pixelaspect") != NULL);
    EXPECT_EQ("sh010", spec->get_string_attribute("shot"));

    const ImageMetadata metadata = reread(pathOf("out.png"));
    EXPECT_EQ(std::optional<std::string>("sh010"), metadata.getString("exif/shot"));
    EXPECT_EQ(std::optional<std::string>("a note"), metadata.getString("ofx/comment"));
    EXPECT_EQ(std::optional<std::string>("test tool"), metadata.getString("ofx/creator"));
}

TEST_F(NativeWriteMetadataTest, PngNoMetadataWritesNoneOfTheEditedKeys)
{
    ASSERT_NO_FATAL_FAILURE(buildEditedChain(pathOf("out.png"), kModeNone));
    renderFrame();

    const std::optional<OIIO::ImageSpec> spec = specOf(pathOf("out.png"));
    ASSERT_TRUE(spec.has_value());
    for (const char* name : { "shot", "ofx/comment", "ofx/creator", "exr/gain", "ImageDescription", "Software", "ofx/filepath", "DocumentName" }) {
        EXPECT_TRUE(spec->find_attribute(name) == NULL) << name;
    }

    const ImageMetadata metadata = reread(pathOf("out.png"));
    EXPECT_FALSE(metadata.getString("exif/shot").has_value());
    EXPECT_FALSE(metadata.getString("ofx/comment").has_value());
    EXPECT_FALSE(metadata.getString("exr/gain").has_value());
    EXPECT_FALSE(metadata.getString("exif/ImageDescription").has_value());
    EXPECT_FALSE(metadata.getString("exif/Software").has_value());
}
