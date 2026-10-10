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

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <SequenceParsing.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/OfxMetadataBridge.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"
#include "Engine/WriteNode.h"

#include <ofxImageEffect.h>
#include <ofxMetadata.h>
#include <ofxhClip.h>
#include <ofxhPropertySuite.h>

NATRON_NAMESPACE_USING

namespace {
const char kMetadataContributeID[] = "org.openfx.examples.metadataContribute";
const char kMetadataViewID[] = "org.openfx.examples.metadataView";
const char kContributeNoteParam[] = "note";
const char kContributeNoteKey[] = "org.openfx.examples.metadataContribute.note";
const double kContributedFrameRate = 30.;

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

    OFX::Host::ImageEffect::MetadataSet* operator->() const
    {
        return _set;
    }

private:
    MetadataRef(const MetadataRef&);
    MetadataRef& operator=(const MetadataRef&);

    OFX::Host::ImageEffect::MetadataSet* _set;
};

OFX::Host::ImageEffect::ClipInstance*
clipOf(const NodePtr& node,
       const char* clipName)
{
    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());

    if (!ofxEffect || !ofxEffect->effectInstance()) {
        return NULL;
    }

    return ofxEffect->effectInstance()->getClip(clipName);
}

NativeEffectBase*
nativeEffectOf(const NodePtr& node)
{
    return dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

void
setContributedNote(const NodePtr& node,
                   const std::string& note)
{
    KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(kContributeNoteParam).get());

    ASSERT_TRUE(knob != NULL) << "metadataContribute has no " << kContributeNoteParam << " param";
    knob->setValue(note);
}

// Asserts that props holds key with exactly the type, dimension and values of value.
void
expectPropertyHolds(const OFX::Host::Property::Set& props,
                    const std::string& key,
                    const ImageMetadata::Value& value)
{
    OFX::Host::Property::Property* property = props.fetchProperty(key);

    ASSERT_TRUE(property != NULL) << key << " is missing from the OpenFX set";

    if (const int* i = std::get_if<int>(&value)) {
        ASSERT_EQ(OFX::Host::Property::eInt, property->getType()) << key;
        ASSERT_EQ(1, property->getDimension()) << key;
        EXPECT_EQ(*i, props.getIntProperty(key)) << key;
    } else if (const double* d = std::get_if<double>(&value)) {
        ASSERT_EQ(OFX::Host::Property::eDouble, property->getType()) << key;
        ASSERT_EQ(1, property->getDimension()) << key;
        EXPECT_EQ(*d, props.getDoubleProperty(key)) << key;
    } else if (const std::string* s = std::get_if<std::string>(&value)) {
        ASSERT_EQ(OFX::Host::Property::eString, property->getType()) << key;
        ASSERT_EQ(1, property->getDimension()) << key;
        EXPECT_EQ(*s, props.getStringProperty(key)) << key;
    } else if (const std::vector<int>* iv = std::get_if<std::vector<int>>(&value)) {
        ASSERT_EQ(OFX::Host::Property::eInt, property->getType()) << key;
        ASSERT_EQ((int)iv->size(), property->getDimension()) << key;
        for (std::size_t k = 0; k < iv->size(); ++k) {
            EXPECT_EQ((*iv)[k], props.getIntProperty(key, (int)k)) << key << "[" << k << "]";
        }
    } else if (const std::vector<double>* dv = std::get_if<std::vector<double>>(&value)) {
        ASSERT_EQ(OFX::Host::Property::eDouble, property->getType()) << key;
        ASSERT_EQ((int)dv->size(), property->getDimension()) << key;
        for (std::size_t k = 0; k < dv->size(); ++k) {
            EXPECT_EQ((*dv)[k], props.getDoubleProperty(key, (int)k)) << key << "[" << k << "]";
        }
    }
}
} // namespace

TEST(OfxMetadataBridge, ConvertsEverySupportedTypeBothWays)
{
    OFX::Host::Property::Set ofx;

    const OFX::Host::Property::PropSpec specs[] = {
        { "exr/scalarInt", OFX::Host::Property::eInt, 1, false, "0" },
        { "exr/scalarDouble", OFX::Host::Property::eDouble, 1, false, "0" },
        { "ofx/filepath", OFX::Host::Property::eString, 1, false, "" },
        { "exr/intTriple", OFX::Host::Property::eInt, 3, false, "0" },
        { "exr/doublePair", OFX::Host::Property::eDouble, 2, false, "0" },
        { "exr/emptyInts", OFX::Host::Property::eInt, 0, false, "0" },
        { "ofx/viewnames", OFX::Host::Property::eString, 2, false, "" },
        { "exr/pointer", OFX::Host::Property::ePointer, 1, false, 0 },
        OFX::Host::Property::propSpecEnd
    };
    ofx.addProperties(specs);

    const int intTriple[] = { 4, -5, 6 };
    const double doublePair[] = { 0.3127, -0.329 };

    ofx.setIntProperty("exr/scalarInt", -7);
    ofx.setDoubleProperty("exr/scalarDouble", 23.976);
    ofx.setStringProperty("ofx/filepath", "/shots/a/plate.0003.exr");
    ofx.setIntPropertyN("exr/intTriple", intTriple, 3);
    ofx.setDoublePropertyN("exr/doublePair", doublePair, 2);
    ofx.setStringProperty("ofx/viewnames", "left", 0);
    ofx.setStringProperty("ofx/viewnames", "right", 1);

    const ImageMetadata native = OfxMetadataBridge::fromOfxPropertySet(ofx);

    EXPECT_EQ(std::optional<int>(-7), native.getInt("exr/scalarInt"));
    EXPECT_EQ(std::optional<double>(23.976), native.getDouble("exr/scalarDouble"));
    EXPECT_EQ(std::optional<std::string>("/shots/a/plate.0003.exr"), native.getString("ofx/filepath"));
    EXPECT_EQ(std::optional<std::vector<int>>(std::vector<int>(intTriple, intTriple + 3)), native.getIntVector("exr/intTriple"));
    EXPECT_EQ(std::optional<std::vector<double>>(std::vector<double>(doublePair, doublePair + 2)), native.getDoubleVector("exr/doublePair"));
    EXPECT_EQ(std::optional<std::vector<int>>(std::vector<int>()), native.getIntVector("exr/emptyInts"));

    // ImageMetadata has no string vector and no pointer type.
    EXPECT_FALSE(native.contains("ofx/viewnames"));
    EXPECT_FALSE(native.contains("exr/pointer"));
    EXPECT_EQ(6u, native.size());

    OFX::Host::Property::Set back;
    OfxMetadataBridge::toOfxPropertySet(native, &back);

    // OpenFX metadata has no key of dimension 0, so the empty vector does not come back.
    EXPECT_TRUE(back.fetchProperty("exr/emptyInts") == NULL);
    EXPECT_EQ(native.size() - 1, back.getProperties().size());

    ImageMetadata nonEmpty = native;
    nonEmpty.remove("exr/emptyInts");
    for (ImageMetadata::const_iterator it = nonEmpty.begin(); it != nonEmpty.end(); ++it) {
        expectPropertyHolds(back, it->first, it->second);
    }

    EXPECT_EQ(nonEmpty, OfxMetadataBridge::fromOfxPropertySet(back));
}

TEST(OfxMetadataBridge, WritingReplacesAPropertyOfAnotherType)
{
    const char* const kReplacedKey = "test/replaced";
    OFX::Host::Property::Set ofx;
    const OFX::Host::Property::PropSpec spec = { kReplacedKey, OFX::Host::Property::eInt, 1, false, "0" };

    ofx.createProperty(spec);
    ofx.setIntProperty(kReplacedKey, 12);

    ImageMetadata native;
    native.setDouble(kReplacedKey, 12.5);
    OfxMetadataBridge::toOfxPropertySet(native, &ofx);

    expectPropertyHolds(ofx, kReplacedKey, ImageMetadata::Value(12.5));
}

TEST_F(BaseTest, OfxMetadataBridgeNativeKeysReachTheOfxInputClip)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));

    ASSERT_TRUE(bool(source) && bool(shuffle));
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

    MetadataSourceTestEffect* sourceEffect = dynamic_cast<MetadataSourceTestEffect*>(source->getEffectInstance().get());
    NativeEffectBase* shuffleEffect = nativeEffectOf(shuffle);
    ASSERT_TRUE(sourceEffect != NULL);
    ASSERT_TRUE(shuffleEffect != NULL);

    ImageMetadata extra;
    extra.setString(kOfxMetadataKeyFilePath, "/shots/a/plate.0003.exr");
    extra.setIntVector("exr/dataWindow", std::vector<int> { 0, 0, 1919, 1079 });
    extra.setDoubleVector("exr/chromaticities", std::vector<double> { 0.64, 0.33, 0.3, 0.6 });
    sourceEffect->setExtraMetadata(extra);

    connectNodes(source, shuffle, 0, true);
    connectNodes(shuffle, view, 0, true);

    const double time = 3.;
    const ImageMetadata expected = shuffleEffect->getOutputMetadata(time, ViewIdx(0));

    ASSERT_EQ(std::optional<int>(static_cast<int>(time)), expected.getInt(kOfxMetadataKeySourceFrame));
    ASSERT_TRUE(expected.contains(kOfxMetadataKeyFilePath));
    ASSERT_TRUE(expected.contains("exr/tag"));

    OFX::Host::ImageEffect::ClipInstance* clip = clipOf(view, kOfxImageEffectSimpleSourceClipName);
    ASSERT_TRUE(clip != NULL) << "metadataView has no " << kOfxImageEffectSimpleSourceClipName << " clip";

    MetadataRef received(clip, time);
    ASSERT_TRUE(received.get() != NULL);

    for (ImageMetadata::const_iterator it = expected.begin(); it != expected.end(); ++it) {
        expectPropertyHolds(*received.get(), it->first, it->second);
    }

    EXPECT_TRUE(received->fetchProperty(kOfxMetadataKeyWidth) == NULL);
    EXPECT_TRUE(received->fetchProperty(kOfxMetadataKeyFrameRate) == NULL);
}

TEST_F(BaseTest, OfxMetadataBridgeNativeGeneratorKeysReachTheOfxInputClipWithoutSourceKeys)
{
    NodePtr generator = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));

    ASSERT_TRUE(bool(generator));
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

    NativeEffectBase* generatorEffect = nativeEffectOf(generator);
    ASSERT_TRUE(generatorEffect != NULL);

    connectNodes(generator, view, 0, true);

    OFX::Host::ImageEffect::ClipInstance* clip = clipOf(view, kOfxImageEffectSimpleSourceClipName);
    ASSERT_TRUE(clip != NULL) << "metadataView has no " << kOfxImageEffectSimpleSourceClipName << " clip";

    const double time = 7.;
    MetadataRef received(clip, time);
    ASSERT_TRUE(received.get() != NULL);

    EXPECT_TRUE(received->fetchProperty(kOfxMetadataKeySourceFrame) == NULL);
    EXPECT_TRUE(received->fetchProperty(kOfxMetadataKeyFilePath) == NULL);
    EXPECT_TRUE(received->fetchProperty(kOfxMetadataKeyFileSize) == NULL);
    EXPECT_TRUE(received->fetchProperty(kOfxMetadataKeyMTime) == NULL);
    EXPECT_TRUE(received->fetchProperty(kOfxMetadataKeyWidth) == NULL);
    EXPECT_TRUE(received->fetchProperty(kOfxMetadataKeyBitDepth) == NULL);

    ASSERT_TRUE(received->fetchProperty(kOfxMetadataKeyFrameRate) != NULL);
    EXPECT_EQ(getApp()->getProjectFrameRate(), received->getDoubleProperty(kOfxMetadataKeyFrameRate));
    ASSERT_TRUE(received->fetchProperty(kOfxMetadataKeyPixelAspect) != NULL);
    EXPECT_EQ(generatorEffect->getAspectRatio(-1), received->getDoubleProperty(kOfxMetadataKeyPixelAspect));

    EXPECT_EQ(OfxMetadataBridge::fromOfxPropertySet(*received.get()), generatorEffect->getOutputMetadata(time, ViewIdx(0)));
}

TEST_F(BaseTest, OfxMetadataBridgeOfxKeysReachTheNativeNode)
{
    NodePtr contribute = createNode(QString::fromUtf8(kMetadataContributeID));
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));

    ASSERT_TRUE(bool(contribute)) << "node creation failed for " << kMetadataContributeID;
    ASSERT_TRUE(bool(shuffle));

    NativeEffectBase* shuffleEffect = nativeEffectOf(shuffle);
    ASSERT_TRUE(shuffleEffect != NULL);

    setContributedNote(contribute, "bridged");
    connectNodes(contribute, shuffle, 0, true);

    const double time = 2.;
    const ImageMetadata native = shuffleEffect->getOutputMetadata(time, ViewIdx(0));

    EXPECT_EQ(std::optional<std::string>("bridged"), native.getString(kContributeNoteKey));
    EXPECT_EQ(std::optional<double>(kContributedFrameRate), native.getDouble(kOfxMetadataKeyFrameRate));

    OFX::Host::ImageEffect::ClipInstance* output = clipOf(contribute, kOfxImageEffectOutputClipName);
    ASSERT_TRUE(output != NULL);
    {
        MetadataRef ofx(output, time);
        ASSERT_TRUE(ofx.get() != NULL);
        EXPECT_EQ(OfxMetadataBridge::fromOfxPropertySet(*ofx.get()), native);
    }

    setContributedNote(contribute, "changed");

    EXPECT_EQ(std::optional<std::string>("changed"), shuffleEffect->getOutputMetadata(time, ViewIdx(0)).getString(kContributeNoteKey));
}

TEST_F(BaseTest, OfxMetadataBridgeReadKeysReachTheWriteEncoderThroughNativeNodes)
{
    NodePtr generator = createNode(QString::fromUtf8(PLUGINID_OFX_CONSTANT));
    NodePtr fixtureWriter = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(generator) && bool(fixtureWriter));

    connectNodes(generator, fixtureWriter, 0, true);

    KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(fixtureWriter->getKnobByName("bitDepth").get());
    ASSERT_TRUE(bitDepth != NULL);
    bitDepth->setValueFromID("32f", 0);

    KnobChoice* compression = dynamic_cast<KnobChoice*>(fixtureWriter->getKnobByName("compression").get());
    ASSERT_TRUE(compression != NULL);
    compression->setValueFromID("none", 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string pattern = (tmp.path() + QLatin1String("/gateSource.####.exr")).toStdString();
    fixtureWriter->setOutputFilesForWriter(pattern);

    const int firstFrame = 1;
    const int lastFrame = 2;

    OutputEffectInstance* fixtureEffect = dynamic_cast<OutputEffectInstance*>(fixtureWriter->getEffectInstance().get());
    ASSERT_TRUE(fixtureEffect != NULL);

    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(fixtureEffect, firstFrame, lastFrame, 1, false));
    getApp()->startWritersRendering(false, works);

    const std::vector<std::string>& viewNames = getApp()->getProject()->getProjectViewNames();
    const std::string frameTwoPath = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, lastFrame, 0);
    ASSERT_TRUE(QFile::exists(QString::fromStdString(frameTwoPath))) << "fixture frame was not rendered: " << frameTwoPath;

    CreateNodeArgs readArgs(_readPluginID.toStdString(), getApp()->getProject());
    readArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, pattern);
    NodePtr reader = getApp()->createNode(readArgs);
    ASSERT_TRUE(bool(reader));
    ASSERT_TRUE(dynamic_cast<NativeRead*>(reader->getEffectInstance().get()) != NULL);

    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));
    NodePtr write = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(grade) && bool(merge) && bool(write));
    ASSERT_TRUE(nativeEffectOf(grade) != NULL);
    ASSERT_TRUE(nativeEffectOf(merge) != NULL);

    WriteNode* writeNode = dynamic_cast<WriteNode*>(write->getEffectInstance().get());
    ASSERT_TRUE(writeNode != NULL) << "the writer is not backed by a Write container";

    connectNodes(reader, grade, 0, true);
    connectNodes(grade, merge, 1, true);
    connectNodes(merge, write, 0, true);

    NodePtr encoder = writeNode->getEmbeddedWriter();
    ASSERT_TRUE(bool(encoder)) << "the Write container has no embedded encoder";

    OFX::Host::ImageEffect::ClipInstance* source = clipOf(encoder, kOfxImageEffectSimpleSourceClipName);
    ASSERT_TRUE(source != NULL) << "the embedded encoder has no " << kOfxImageEffectSimpleSourceClipName << " clip";

    MetadataRef received(source, lastFrame);
    ASSERT_TRUE(received.get() != NULL);

    ASSERT_TRUE(received->fetchProperty(kOfxMetadataKeyFilePath) != NULL)
        << "the reader's " << kOfxMetadataKeyFilePath << " never reached the encoder";
    EXPECT_EQ(frameTwoPath, received->getStringProperty(kOfxMetadataKeyFilePath));

    ASSERT_TRUE(received->fetchProperty(kOfxMetadataKeySourceFrame) != NULL);
    EXPECT_EQ(lastFrame, received->getIntProperty(kOfxMetadataKeySourceFrame));

    ASSERT_TRUE(received->fetchProperty(kOfxMetadataKeyFileSize) != NULL);
    EXPECT_GT(received->getDoubleProperty(kOfxMetadataKeyFileSize), 0.);
    ASSERT_TRUE(received->fetchProperty(kOfxMetadataKeyMTime) != NULL);
    EXPECT_GT(received->getDoubleProperty(kOfxMetadataKeyMTime), 0.);

    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        QFile::remove(QString::fromStdString(SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0)));
    }
}
