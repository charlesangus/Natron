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

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/OfxMetadataBridge.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>
#include <ofxMetadata.h>
#include <ofxhClip.h>
#include <ofxhPropertySuite.h>

NATRON_NAMESPACE_USING

namespace {
const char kMetadataViewID[] = "org.openfx.examples.metadataView";
const int kSize = 4;

std::string
framePath(const QTemporaryDir& dir,
          const char* stem,
          int frame)
{
    char number[16];

    snprintf(number, sizeof(number), "%04d", frame);

    return (dir.path() + QString::fromUtf8("/%1.%2.exr").arg(QString::fromUtf8(stem), QString::fromUtf8(number))).toStdString();
}

std::string
patternPath(const QTemporaryDir& dir,
            const char* stem)
{
    return (dir.path() + QString::fromUtf8("/%1.####.exr").arg(QString::fromUtf8(stem))).toStdString();
}

bool
writeImage(const std::string& path,
           const OIIO::ImageSpec& spec)
{
    std::vector<float> pixels((std::size_t)spec.width * spec.height * spec.nchannels, 0.25f);
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

    return out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();
}

// An EXR frame whose attributes name it, so the file a key came from can be told from its value.
bool
writeExrFrame(const std::string& path,
              int frame)
{
    OIIO::ImageSpec spec(kSize, kSize, 4, OIIO::TypeDesc::FLOAT);
    const float vector[3] = { 1.f, 2.f, 3.f };
    const int rate[2] = { 24000, 1001 };
    const unsigned int timecode[2] = { 0x01020304u, 0u };

    spec.attribute("myString", "hello");
    spec.attribute("myInt", 42);
    spec.attribute("myFloat", 1.5f);
    spec.attribute("myVec", OIIO::TypeVector, vector);
    spec.attribute("frameTag", frame);
    spec.attribute("PixelAspectRatio", 2.0f);
    spec.attribute("FramesPerSecond", OIIO::TypeRational, rate);
    spec.attribute("smpte:TimeCode", OIIO::TypeTimeCode, timecode);

    return writeImage(path, spec);
}

bool
writeJpeg(const std::string& path)
{
    OIIO::ImageSpec spec(kSize, kSize, 3, OIIO::TypeDesc::UINT8);

    spec.attribute("Make", "NatronCam");
    spec.attribute("Model", "Model One");
    // OIIO files the main-IFD tags unprefixed, the Exif-IFD tags under Exif:, and writes ISO as PhotographicSensitivity.
    // 2 s because the JPEG writer's rational conversion does not round-trip exposure times below one second.
    spec.attribute("ExposureTime", 2.f);
    spec.attribute("FNumber", 4.f);
    spec.attribute("Exif:PhotographicSensitivity", 400);

    return writeImage(path, spec);
}

class MetadataRef {
public:
    MetadataRef(OFX::Host::ImageEffect::ClipInstance* clip,
                OfxTime time)
        : _set(clip ? clip->getMetadata(time) : NULL)
    {
    }

    ~MetadataRef()
    {
        if (_set) {
            _set->releaseReference();
        }
    }

    OFX::Host::ImageEffect::MetadataSet* get() const
    {
        return _set;
    }

private:
    MetadataRef(const MetadataRef&);
    MetadataRef& operator=(const MetadataRef&);

    OFX::Host::ImageEffect::MetadataSet* _set;
};

// The keys the OpenFX input clip holds, read back the way a plug-in sees them.
ImageMetadata
receivedBy(const NodePtr& view,
           double time)
{
    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>(view->getEffectInstance().get());
    OFX::Host::ImageEffect::ClipInstance* clip = (ofxEffect && ofxEffect->effectInstance()) ? ofxEffect->effectInstance()->getClip(kOfxImageEffectSimpleSourceClipName) : NULL;
    MetadataRef received(clip, time);

    EXPECT_TRUE(received.get() != NULL);

    return received.get() ? OfxMetadataBridge::fromOfxPropertySet(*received.get()) : ImageMetadata();
}

NativeEffectBase*
nativeEffectOf(const NodePtr& node)
{
    return dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}
} // namespace

class NativeReadMetadataTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    void writeSequence(const QTemporaryDir& dir,
                       const char* stem,
                       const std::vector<int>& frames)
    {
        ASSERT_TRUE(dir.isValid());
        for (std::size_t i = 0; i < frames.size(); ++i) {
            ASSERT_TRUE(writeExrFrame(framePath(dir, stem, frames[i]), frames[i]));
        }
    }

    NodePtr createRead(const std::string& file)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        EXPECT_TRUE(bool(node));
        if (node && !file.empty()) {
            setFile(node, file);
        }

        return node;
    }

    void setFile(const NodePtr& node,
                 const std::string& file)
    {
        KnobFile* knob = dynamic_cast<KnobFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(knob != NULL);
        knob->setValue(file);
        node->getEffectInstance()->refreshMetadata_public(false);
    }

    // What the GUI does when the user types into an int knob.
    void userSetInt(const NodePtr& node,
                    const char* name,
                    int value)
    {
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName(name).get());
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    }

    ImageMetadata metadataOf(const NodePtr& node,
                             double time)
    {
        NativeEffectBase* effect = nativeEffectOf(node);

        EXPECT_TRUE(effect != NULL);

        return effect ? effect->getOutputMetadata(time, ViewIdx(0)) : ImageMetadata();
    }
};

TEST_F(NativeReadMetadataTest, ExrAttributesAndTheStandardKeys)
{
    QTemporaryDir dir;
    std::vector<int> frames;
    frames.push_back(1);
    frames.push_back(2);
    frames.push_back(3);
    writeSequence(dir, "meta", frames);
    NodePtr read = createRead(patternPath(dir, "meta"));
    ASSERT_TRUE(bool(read));

    const ImageMetadata metadata = metadataOf(read, 2.);

    EXPECT_EQ(std::optional<std::string>(framePath(dir, "meta", 2)), metadata.getString("ofx/filepath"));
    EXPECT_EQ(std::optional<int>(2), metadata.getInt("ofx/frame"));
    EXPECT_FALSE(metadata.contains("ofx/framerate"));
    EXPECT_EQ(std::optional<double>(2.), metadata.getDouble("ofx/pixelaspect"));
    EXPECT_EQ(std::optional<std::string>("01:02:03:04"), metadata.getString("ofx/timecode"));

    const std::optional<double> size = metadata.getDouble("ofx/filesize");
    ASSERT_TRUE(size.has_value());
    EXPECT_GT(*size, 0.);
    const std::optional<double> mtime = metadata.getDouble("ofx/mtime");
    ASSERT_TRUE(mtime.has_value());
    EXPECT_GT(*mtime, 0.);

    EXPECT_EQ(std::optional<std::string>("hello"), metadata.getString("exr/myString"));
    EXPECT_EQ(std::optional<int>(42), metadata.getInt("exr/myInt"));
    EXPECT_EQ(std::optional<double>(1.5), metadata.getDouble("exr/myFloat"));
    EXPECT_EQ(std::optional<std::vector<double>>(std::vector<double>({ 1., 2., 3. })), metadata.getDoubleVector("exr/myVec"));
    EXPECT_EQ(std::optional<int>(2), metadata.getInt("exr/frameTag"));
    EXPECT_EQ(std::optional<double>(24000. / 1001.), metadata.getDouble("exr/FramesPerSecond"));
    EXPECT_EQ(std::optional<double>(2.), metadata.getDouble("exr/PixelAspectRatio"));

    for (ImageMetadata::const_iterator it = metadata.begin(); it != metadata.end(); ++it) {
        EXPECT_NE(std::string::npos, it->first.find('/')) << it->first;
        EXPECT_NE(0, it->first.compare(0, 5, "oiio:")) << it->first;
    }
}

TEST_F(NativeReadMetadataTest, JpegExifFieldsAppearUnderExif)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = (dir.path() + QString::fromUtf8("/photo.jpg")).toStdString();
    ASSERT_TRUE(writeJpeg(path));
    NodePtr read = createRead(path);
    ASSERT_TRUE(bool(read));

    const ImageMetadata metadata = metadataOf(read, 1.);

    EXPECT_EQ(std::optional<std::string>(path), metadata.getString("ofx/filepath"));
    EXPECT_EQ(std::optional<std::string>("NatronCam"), metadata.getString("exif/Make"));
    EXPECT_EQ(std::optional<std::string>("Model One"), metadata.getString("exif/Model"));
    EXPECT_EQ(std::optional<double>(2.), metadata.getDouble("exif/ExposureTime"));
    EXPECT_EQ(std::optional<double>(4.), metadata.getDouble("exif/FNumber"));
    EXPECT_EQ(std::optional<int>(400), metadata.getInt("exif/PhotographicSensitivity"));
    EXPECT_FALSE(metadata.contains("exr/Make"));
}

TEST_F(NativeReadMetadataTest, FrameKeysFollowTheTimeOffsetAndTheFilename)
{
    QTemporaryDir dir;
    std::vector<int> frames;
    frames.push_back(1);
    frames.push_back(2);
    frames.push_back(3);
    writeSequence(dir, "a", frames);
    writeSequence(dir, "b", frames);
    NodePtr read = createRead(patternPath(dir, "a"));
    ASSERT_TRUE(bool(read));

    // Past the end of the range the last frame is held.
    EXPECT_EQ(std::optional<int>(3), metadataOf(read, 12.).getInt("ofx/frame"));

    userSetInt(read, "timeOffset", 10);

    ImageMetadata metadata = metadataOf(read, 12.);
    EXPECT_EQ(std::optional<int>(2), metadata.getInt("ofx/frame"));
    EXPECT_EQ(std::optional<std::string>(framePath(dir, "a", 2)), metadata.getString("ofx/filepath"));
    EXPECT_EQ(std::optional<int>(2), metadata.getInt("exr/frameTag"));

    setFile(read, patternPath(dir, "b"));

    metadata = metadataOf(read, 12.);
    EXPECT_EQ(std::optional<std::string>(framePath(dir, "b", 2)), metadata.getString("ofx/filepath"));
}

TEST_F(NativeReadMetadataTest, AFrameThatLoadsBlackHasNoFileKeys)
{
    QTemporaryDir dir;
    std::vector<int> frames;
    frames.push_back(1);
    frames.push_back(2);
    frames.push_back(4);
    writeSequence(dir, "gap", frames);
    NodePtr read = createRead(patternPath(dir, "gap"));
    ASSERT_TRUE(bool(read));

    KnobChoice* onMissing = dynamic_cast<KnobChoice*>(read->getKnobByName("onMissingFrame").get());
    ASSERT_TRUE(onMissing != NULL);
    onMissing->setValue(4);

    const ImageMetadata metadata = metadataOf(read, 3.);

    EXPECT_FALSE(metadata.contains("ofx/filepath"));
    EXPECT_FALSE(metadata.contains("ofx/frame"));
    EXPECT_FALSE(metadata.contains("ofx/filesize"));
    EXPECT_FALSE(metadata.contains("ofx/mtime"));
    EXPECT_FALSE(metadata.contains("exr/myString"));
    EXPECT_FALSE(metadata.contains("ofx/framerate"));
    EXPECT_TRUE(metadata.contains("ofx/pixelaspect"));
}

TEST_F(NativeReadMetadataTest, ReadWithoutAFileHasOnlyTheAspect)
{
    NodePtr read = createRead(std::string());
    ASSERT_TRUE(bool(read));

    const ImageMetadata metadata = metadataOf(read, 1.);

    EXPECT_EQ(1u, metadata.size());
    EXPECT_FALSE(metadata.contains("ofx/framerate"));
    EXPECT_TRUE(metadata.contains("ofx/pixelaspect"));
}

TEST_F(NativeReadMetadataTest, AFileFrameRateIsPlainMetadataAndNotTheOutputRate)
{
    KnobDouble* projectFrameRate = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("frameRate").get());
    ASSERT_TRUE(projectFrameRate != NULL);
    projectFrameRate->setValue(30.);

    QTemporaryDir dir;
    std::vector<int> frames(1, 1);
    writeSequence(dir, "rate", frames);
    NodePtr read = createRead(patternPath(dir, "rate"));
    ASSERT_TRUE(bool(read));
    NodePtr empty = createRead(std::string());
    ASSERT_TRUE(bool(empty));

    const ImageMetadata metadata = metadataOf(read, 1.);

    EXPECT_FALSE(metadata.contains("ofx/framerate"));
    EXPECT_EQ(std::optional<double>(24000. / 1001.), metadata.getDouble("exr/FramesPerSecond"));
    EXPECT_DOUBLE_EQ(empty->getEffectInstance()->getFrameRate(), read->getEffectInstance()->getFrameRate());
    EXPECT_NE(24000. / 1001., read->getEffectInstance()->getFrameRate());
}

TEST_F(NativeReadMetadataTest, KeysReachANativeNodePastADot)
{
    QTemporaryDir dir;
    std::vector<int> frames(1, 1);
    writeSequence(dir, "dot", frames);
    NodePtr read = createRead(framePath(dir, "dot", 1));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(read && dot && grade);

    connectNodes(read, dot, 0, true);
    connectNodes(dot, grade, 0, true);

    const ImageMetadata expected = metadataOf(read, 1.);

    ASSERT_EQ(std::optional<std::string>(framePath(dir, "dot", 1)), expected.getString("ofx/filepath"));
    ASSERT_EQ(std::optional<std::string>("hello"), expected.getString("exr/myString"));
    EXPECT_EQ(expected, metadataOf(grade, 1.));
}

TEST_F(NativeReadMetadataTest, KeysReachAnOfxInputClipPastADot)
{
    QTemporaryDir dir;
    std::vector<int> frames(1, 1);
    writeSequence(dir, "ofx", frames);
    NodePtr read = createRead(framePath(dir, "ofx", 1));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));
    ASSERT_TRUE(read && dot);
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

    connectNodes(read, dot, 0, true);
    connectNodes(dot, view, 0, true);

    const ImageMetadata expected = metadataOf(read, 1.);

    ASSERT_EQ(std::optional<std::string>(framePath(dir, "ofx", 1)), expected.getString("ofx/filepath"));
    EXPECT_EQ(expected, receivedBy(view, 1.));
}

TEST_F(NativeReadMetadataTest, KeysReachAnOfxInputClipDirectly)
{
    QTemporaryDir dir;
    std::vector<int> frames(1, 1);
    writeSequence(dir, "direct", frames);
    NodePtr read = createRead(framePath(dir, "direct", 1));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

    connectNodes(read, view, 0, true);

    const ImageMetadata received = receivedBy(view, 1.);

    EXPECT_EQ(metadataOf(read, 1.), received);
    EXPECT_EQ(std::optional<std::string>(framePath(dir, "direct", 1)), received.getString("ofx/filepath"));
    EXPECT_EQ(std::optional<std::string>("hello"), received.getString("exr/myString"));
}
