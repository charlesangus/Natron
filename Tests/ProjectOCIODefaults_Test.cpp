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

#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <OpenColorIO/OpenColorIO.h>
#include <OpenImageIO/imageio.h>

#include <ofxImageEffect.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/IO/ReadColorSpace.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ViewIdx.h"
#include "Engine/WriteNode.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kSRGBSpace = "sRGB Encoded Rec.709 (sRGB)";
const char* const kGamma22Space = "Gamma 2.2 Encoded Rec.709";
const char* const kLinearRec709Space = "Linear Rec.709 (sRGB)";
const char* const kWorkingSpace = "ACEScg";

const char* const kInputSpaceKnob = "ocioInputSpace";
const char* const kInputSpaceChoiceKnob = "ocioInputSpaceIndex";
const char* const kOutputSpaceKnob = "ocioOutputSpace";
const char* const kWorkingSpaceKnob = "ocioWorkingSpace";
const char* const kInputSpaceSetKnob = "ocioInputSpaceSet";
const char* const kFileRuleSpace = "ACES2065-1";

ProjectPtr
project()
{
    return appPTR->getTopLevelInstance()->getProject();
}

std::string
fixture(const char* name)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR "/") + name;
}

NodePtr
createEmptyRead()
{
    CreateNodeArgs args(PLUGINID_NATRON_READ, project());

    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);

    return appPTR->getTopLevelInstance()->createNode(args);
}

void
changeReaderFile(const NodePtr& reader,
                 const std::string& file)
{
    KnobFilePtr knob = std::dynamic_pointer_cast<KnobFile>(reader->getKnobByName(kOfxImageEffectFileParamName));

    ASSERT_TRUE(bool(knob));
    knob->setValue(file);
    reader->getEffectInstance()->refreshMetadata_public(false);
}

NodePtr
createReader(const std::string& file)
{
    NodePtr reader = createEmptyRead();

    EXPECT_TRUE(bool(reader));
    if (reader) {
        changeReaderFile(reader, file);
    }

    return reader;
}

// A file whose pixel type picks its project default: nothing in it or in its name names a
// colourspace, which the fixtures' sRGB chunks do.
std::string
writeUntaggedImage(const QTemporaryDir& dir,
                   const char* file,
                   OIIO::TypeDesc type,
                   int bitsPerSample)
{
    const int width = 16;
    const int height = 8;
    const int channels = 3;
    const std::string path = (dir.path() + QString::fromUtf8("/") + QString::fromUtf8(file)).toStdString();
    OIIO::ImageSpec spec(width, height, channels, type);

    if (bitsPerSample > 0) {
        spec.attribute("oiio:BitsPerSample", bitsPerSample);
    }
    std::vector<float> pixels((std::size_t)width * height * channels, 0.5f);
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    EXPECT_TRUE(bool(out)) << file;
    if (!out) {
        return std::string();
    }
    EXPECT_TRUE(out->open(path, spec)) << out->geterror();
    EXPECT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
    EXPECT_TRUE(out->close()) << out->geterror();

    OIIO::ImageInput::unique_ptr in = OIIO::ImageInput::open(path);
    EXPECT_TRUE(bool(in)) << file;
    if (in) {
        EXPECT_TRUE(ReadColorSpace::embeddedColorSpace(project()->getColorManagement()->getConfig(), in->spec()).empty())
            << file << " carries a colourspace of the config";
    }

    return path;
}

std::string
writeEightBit(const QTemporaryDir& dir)
{
    return writeUntaggedImage(dir, "eight.tif", OIIO::TypeDesc::UINT8, 0);
}

std::string
writeSixteenBit(const QTemporaryDir& dir)
{
    return writeUntaggedImage(dir, "ten.dpx", OIIO::TypeDesc::UINT16, 10);
}

std::string
writeFloat(const QTemporaryDir& dir)
{
    return writeUntaggedImage(dir, "float.tif", OIIO::TypeDesc::FLOAT, 0);
}

NodePtr
createWriter(const std::string& encoderID,
             const std::string& file)
{
    CreateNodeArgs args(encoderID, project());

    args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, file);

    return appPTR->getTopLevelInstance()->createNode(args);
}

NodePtr
embeddedNode(const NodePtr& node)
{
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

    if (WriteNode* isWrite = dynamic_cast<WriteNode*>(effect.get())) {
        return isWrite->getEmbeddedWriter();
    }

    return NodePtr();
}

// A Write holds its knobs in the encoder it wraps; the native Read holds its own.
NodePtr
knobOwner(const NodePtr& node)
{
    NodePtr embedded = embeddedNode(node);

    return embedded ? embedded : node;
}

std::string
stringValue(const NodePtr& wrapper,
            const char* name)
{
    NodePtr node = knobOwner(wrapper);
    KnobStringBasePtr knob = node ? std::dynamic_pointer_cast<KnobStringBase>(node->getKnobByName(name)) : KnobStringBasePtr();

    EXPECT_TRUE(bool(knob)) << name;

    return knob ? knob->getValue() : std::string();
}

KnobBoolPtr
boolKnob(const NodePtr& wrapper,
         const char* name)
{
    NodePtr node = knobOwner(wrapper);

    return node ? std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(name)) : KnobBoolPtr();
}

void
setProjectFileSpace(const char* knobName,
                    const char* space)
{
    KnobChoicePtr knob = project()->getKnobByNameAndType<KnobChoice>(knobName);

    ASSERT_TRUE(bool(knob)) << knobName;
    knob->setValueFromID(space, 0);
    ASSERT_EQ(std::string(space), knob->getActiveEntry().id) << knobName;
}

// A name written by a file's metadata may be an alias, so compare what the config resolves it to.
std::string
canonicalSpace(const std::string& name)
{
    OCIO_NAMESPACE::ConstColorSpaceRcPtr cs = project()->getColorManagement()->getConfig()->getColorSpace(name.c_str());

    return cs ? std::string(cs->getName()) : std::string();
}

void
userPicksInputSpace(const NodePtr& reader,
                    const char* space)
{
    KnobChoicePtr menu = std::dynamic_pointer_cast<KnobChoice>(reader->getKnobByName(kInputSpaceChoiceKnob));

    ASSERT_TRUE(bool(menu));
    const std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();
    const std::string suffix = std::string("/") + space;
    int index = -1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string& label = entries[i].label;
        const bool endsWithSpace = (label.size() >= suffix.size()) && (label.compare(label.size() - suffix.size(), suffix.size(), suffix) == 0);
        if ((entries[i].id == space) || (label == space) || endsWithSpace) {
            index = (int)i;
            break;
        }
    }
    ASSERT_GE(index, 0) << space;
    menu->setValue(index, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
    ASSERT_EQ(std::string(space), stringValue(reader, kInputSpaceKnob));
}

void
saveResetAndLoad(const QTemporaryDir& tmp,
                 const char* fileName)
{
    const QString dirPath = tmp.path() + QLatin1Char('/');
    QString saved;

    ASSERT_TRUE(project()->saveProject(dirPath, QString::fromUtf8(fileName), &saved));
    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8(fileName)));
}

struct DecodedPng {
    int width;
    int height;
    int bitDepth;
    int channels;
    int firstSample;

    DecodedPng()
        : width(0)
        , height(0)
        , bitDepth(0)
        , channels(0)
        , firstSample(-1)
    {
    }
};

quint32
bigEndian32(const QByteArray& bytes,
            int offset)
{
    return (quint32(quint8(bytes[offset])) << 24) | (quint32(quint8(bytes[offset + 1])) << 16) | (quint32(quint8(bytes[offset + 2])) << 8) | quint32(quint8(bytes[offset + 3]));
}

// Reads the first sample of the first pixel of a non-interlaced 8-bit PNG with the Qt::Core
// zlib; the first scanline's first pixel is raw under every PNG filter type.
bool
decodePngFirstSample(const std::string& path,
                     DecodedPng* out,
                     std::string* error)
{
    QFile file(QString::fromStdString(path));

    if (!file.open(QIODevice::ReadOnly)) {
        *error = "cannot open " + path;

        return false;
    }
    const QByteArray bytes = file.readAll();
    const QByteArray signature("\x89PNG\r\n\x1a\n", 8);
    if (!bytes.startsWith(signature)) {
        *error = "not a PNG: " + path;

        return false;
    }

    int colorType = -1;
    QByteArray idat;
    int pos = 8;
    while (pos + 12 <= bytes.size()) {
        const int length = (int)bigEndian32(bytes, pos);
        const QByteArray type = bytes.mid(pos + 4, 4);
        const int data = pos + 8;
        if (data + length > bytes.size()) {
            *error = "truncated PNG chunk";

            return false;
        }
        if (type == "IHDR") {
            out->width = (int)bigEndian32(bytes, data);
            out->height = (int)bigEndian32(bytes, data + 4);
            out->bitDepth = quint8(bytes[data + 8]);
            colorType = quint8(bytes[data + 9]);
        } else if (type == "IDAT") {
            idat += bytes.mid(data, length);
        } else if (type == "IEND") {
            break;
        }
        pos = data + length + 4;
    }

    switch (colorType) {
    case 0:
        out->channels = 1;
        break;
    case 2:
        out->channels = 3;
        break;
    case 4:
        out->channels = 2;
        break;
    case 6:
        out->channels = 4;
        break;
    default:
        *error = "unsupported PNG colour type";

        return false;
    }
    if ((out->bitDepth != 8) || idat.isEmpty()) {
        *error = "expected an 8-bit PNG with pixel data";

        return false;
    }

    const quint32 expected = (quint32)out->height * (quint32)(1 + out->width * out->channels);
    QByteArray prefixed;
    prefixed.append(char(expected >> 24));
    prefixed.append(char(expected >> 16));
    prefixed.append(char(expected >> 8));
    prefixed.append(char(expected));
    prefixed += idat;
    const QByteArray raw = qUncompress(prefixed);
    if (raw.size() < 2) {
        *error = "cannot inflate the PNG pixel data";

        return false;
    }
    out->firstSample = quint8(raw[1]);

    return true;
}
} // namespace

class ProjectOCIODefaultsTest
    : public testing::Test {
protected:
    virtual void SetUp()
    {
        project()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
    }

    virtual void TearDown()
    {
        project()->reset(false, true);
    }
};

TEST_F(ProjectOCIODefaultsTest, ReadsClassifyByFileRuleTagAndTypeAndTakeTheWorkingSpace)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr eight = createReader(writeEightBit(tmp));
    NodePtr png16 = createReader(fixture("png-16bit.png"));
    NodePtr exr = createReader(fixture("flat-rgb-only.exr"));

    ASSERT_TRUE(bool(eight));
    ASSERT_TRUE(bool(png16));
    ASSERT_TRUE(bool(exr));

    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategory8Bit), stringValue(eight, kInputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(eight, kInputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), canonicalSpace(stringValue(png16, kInputSpaceKnob)));
    EXPECT_EQ(std::string(kFileRuleSpace), stringValue(exr, kInputSpaceKnob));

    const std::string working = project()->getWorkingColorSpace();
    EXPECT_EQ(std::string(kWorkingSpace), working);
    EXPECT_EQ(working, stringValue(eight, kWorkingSpaceKnob));
    EXPECT_EQ(working, stringValue(png16, kWorkingSpaceKnob));
    EXPECT_EQ(working, stringValue(exr, kWorkingSpaceKnob));
}

TEST_F(ProjectOCIODefaultsTest, SixteenBitFileTakesTheSixteenBitDefaultNotTheEightBitOne)
{
    setProjectFileSpace("colorSpace16Bit", kGamma22Space);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr eight = createReader(writeEightBit(tmp));
    NodePtr sixteen = createReader(writeSixteenBit(tmp));

    ASSERT_TRUE(bool(eight));
    ASSERT_TRUE(bool(sixteen));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(eight, kInputSpaceKnob));
    EXPECT_EQ(std::string(kGamma22Space), stringValue(sixteen, kInputSpaceKnob));
}

TEST_F(ProjectOCIODefaultsTest, NativeReadKeepsTheFilesOwnValidColorspaceOverTheProjectDefault)
{
    setProjectFileSpace("colorSpace16Bit", kGamma22Space);

    // OIIO tags the fixture's PNG with oiio:ColorSpace = srgb_rec709_scene, an alias of the
    // config's sRGB space, so the project's 16-bit default must not replace it.
    NodePtr png16 = createReader(fixture("png-16bit.png"));

    ASSERT_TRUE(bool(png16));
    EXPECT_EQ(std::string(kSRGBSpace), canonicalSpace(stringValue(png16, kInputSpaceKnob)));
    EXPECT_EQ(std::string(kWorkingSpace), stringValue(png16, kWorkingSpaceKnob));
}

TEST_F(ProjectOCIODefaultsTest, ChangingAFileDefaultAffectsOnlyNewReads)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string file = writeEightBit(tmp);

    NodePtr existing = createReader(file);

    ASSERT_TRUE(bool(existing));
    ASSERT_EQ(std::string(kSRGBSpace), stringValue(existing, kInputSpaceKnob));

    setProjectFileSpace("colorSpace8Bit", kGamma22Space);

    NodePtr created = createReader(file);

    ASSERT_TRUE(bool(created));
    EXPECT_EQ(std::string(kGamma22Space), stringValue(created, kInputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(existing, kInputSpaceKnob));
}

TEST_F(ProjectOCIODefaultsTest, HostDrivenDefaultsDoNotMarkTheInputSpaceAsUserSet)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createReader(writeEightBit(tmp));

    ASSERT_TRUE(bool(reader));
    KnobBoolPtr userSet = boolKnob(reader, kInputSpaceSetKnob);
    ASSERT_TRUE(bool(userSet));
    EXPECT_FALSE(userSet->getValue());

    userPicksInputSpace(reader, kGamma22Space);
    EXPECT_TRUE(userSet->getValue());
}

TEST_F(ProjectOCIODefaultsTest, AUserSetInputSpaceSurvivesAFilenameChange)
{
    setProjectFileSpace("colorSpace16Bit", kSRGBSpace);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string eight = writeEightBit(tmp);
    const std::string sixteen = writeSixteenBit(tmp);

    NodePtr reader = createReader(eight);

    ASSERT_TRUE(bool(reader));
    userPicksInputSpace(reader, kGamma22Space);

    changeReaderFile(reader, sixteen);
    EXPECT_EQ(std::string(kGamma22Space), stringValue(reader, kInputSpaceKnob));

    changeReaderFile(reader, eight);
    EXPECT_EQ(std::string(kGamma22Space), stringValue(reader, kInputSpaceKnob));
    KnobBoolPtr userSet = boolKnob(reader, kInputSpaceSetKnob);
    ASSERT_TRUE(bool(userSet));
    EXPECT_TRUE(userSet->getValue());
}

TEST_F(ProjectOCIODefaultsTest, AFilenameEditReGuessesWhenTheInputSpaceIsNotUserSet)
{
    setProjectFileSpace("colorSpace16Bit", kGamma22Space);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createReader(writeEightBit(tmp));

    ASSERT_TRUE(bool(reader));
    ASSERT_EQ(std::string(kSRGBSpace), stringValue(reader, kInputSpaceKnob));

    changeReaderFile(reader, writeSixteenBit(tmp));

    EXPECT_EQ(std::string(kGamma22Space), stringValue(reader, kInputSpaceKnob));
    EXPECT_EQ(std::string(kWorkingSpace), stringValue(reader, kWorkingSpaceKnob));
}

TEST_F(ProjectOCIODefaultsTest, AnEmptyReadGivenAnEightBitFileTakesTheProjectDefaults)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createEmptyRead();

    ASSERT_TRUE(bool(reader));

    changeReaderFile(reader, writeEightBit(tmp));

    EXPECT_EQ(std::string(kSRGBSpace), canonicalSpace(stringValue(reader, kInputSpaceKnob)));
    EXPECT_EQ(std::string(kWorkingSpace), stringValue(reader, kWorkingSpaceKnob));
    KnobBoolPtr userSet = boolKnob(reader, kInputSpaceSetKnob);
    ASSERT_TRUE(bool(userSet));
    EXPECT_FALSE(userSet->getValue());
}

TEST_F(ProjectOCIODefaultsTest, AReadWhoseFilenameIsClearedAndSetAgainTakesTheDefaultOfTheNewFile)
{
    setProjectFileSpace("colorSpaceFloat", kLinearRec709Space);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createReader(writeEightBit(tmp));

    ASSERT_TRUE(bool(reader));
    ASSERT_EQ(std::string(kSRGBSpace), stringValue(reader, kInputSpaceKnob));

    changeReaderFile(reader, std::string());
    changeReaderFile(reader, writeFloat(tmp));

    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategoryFloat), stringValue(reader, kInputSpaceKnob));
    EXPECT_EQ(std::string(kLinearRec709Space), stringValue(reader, kInputSpaceKnob));
    EXPECT_EQ(std::string(kWorkingSpace), stringValue(reader, kWorkingSpaceKnob));

    // The config's file rule for .exr outranks the float default.
    changeReaderFile(reader, fixture("flat-rgb-only.exr"));
    EXPECT_EQ(std::string(kFileRuleSpace), stringValue(reader, kInputSpaceKnob));
}

TEST_F(ProjectOCIODefaultsTest, AReloadedReadKeepsTheSpacesItGuessed)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createEmptyRead();

    ASSERT_TRUE(bool(reader));
    changeReaderFile(reader, writeEightBit(tmp));
    const std::string inputSpace = stringValue(reader, kInputSpaceKnob);
    ASSERT_EQ(std::string(kSRGBSpace), canonicalSpace(inputSpace));
    ASSERT_EQ(std::string(kWorkingSpace), stringValue(reader, kWorkingSpaceKnob));
    const std::string name = reader->getScriptName_mt_safe();

    // A restored read that guessed again would take this new 8-bit default.
    setProjectFileSpace("colorSpace8Bit", kGamma22Space);

    saveResetAndLoad(tmp, "guessed-read.ntp");

    ASSERT_EQ(std::string(kGamma22Space), project()->getFileColorSpace(eFileColorCategory8Bit));
    reader = project()->getNodeByName(name);
    ASSERT_TRUE(bool(reader));
    EXPECT_EQ(inputSpace, stringValue(reader, kInputSpaceKnob));
    EXPECT_EQ(std::string(kWorkingSpace), stringValue(reader, kWorkingSpaceKnob));
}

TEST_F(ProjectOCIODefaultsTest, WritePngOutputsTheEightBitDefaultFromTheWorkingSpace)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr png = createWriter(PLUGINID_OFX_WRITEPNG, (tmp.path() + QLatin1String("/out.png")).toStdString());

    ASSERT_TRUE(bool(png));
    ASSERT_TRUE(bool(embeddedNode(png)));
    ASSERT_EQ(std::string(PLUGINID_OFX_WRITEPNG), embeddedNode(png)->getPluginID());
    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategory8Bit), stringValue(png, kOutputSpaceKnob));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(png, kOutputSpaceKnob));
    EXPECT_EQ(project()->getWorkingColorSpace(), stringValue(png, kInputSpaceKnob));
}

TEST_F(ProjectOCIODefaultsTest, WriteOIIOExrOutputsTheFloatDefaultFromTheWorkingSpace)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr exr = createWriter(PLUGINID_OFX_WRITEOIIO, (tmp.path() + QLatin1String("/out.exr")).toStdString());

    ASSERT_TRUE(bool(exr));
    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategoryFloat), stringValue(exr, kOutputSpaceKnob));
    EXPECT_EQ(std::string(kWorkingSpace), stringValue(exr, kOutputSpaceKnob));
    EXPECT_EQ(project()->getWorkingColorSpace(), stringValue(exr, kInputSpaceKnob));
}

TEST_F(ProjectOCIODefaultsTest, AnExistingWriteKeepsItsOutputSpaceAcrossADefaultChangeAndReload)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr writer = createWriter(PLUGINID_OFX_WRITEPNG, (tmp.path() + QLatin1String("/out.png")).toStdString());

    ASSERT_TRUE(bool(writer));
    ASSERT_EQ(std::string(kSRGBSpace), stringValue(writer, kOutputSpaceKnob));
    const std::string name = writer->getScriptName_mt_safe();

    setProjectFileSpace("colorSpace8Bit", kGamma22Space);
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(writer, kOutputSpaceKnob));

    saveResetAndLoad(tmp, "defaults.ntp");

    ASSERT_EQ(std::string(kGamma22Space), project()->getFileColorSpace(eFileColorCategory8Bit));
    writer = project()->getNodeByName(name);
    ASSERT_TRUE(bool(writer));
    EXPECT_EQ(std::string(kSRGBSpace), stringValue(writer, kOutputSpaceKnob));
    EXPECT_EQ(std::string(kWorkingSpace), stringValue(writer, kInputSpaceKnob));
}

// ACEScg 0.18 is mid-gray: linear 0.18 encodes to 0.4614 under the sRGB curve, i.e. code 118.
TEST_F(ProjectOCIODefaultsTest, ProbeOfMidGrayWrittenToAnEightBitPngEncodesToCode118)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QLatin1String("/probe.png")).toStdString();

    CreateNodeArgs constantArgs(std::string("net.sf.openfx.ConstantPlugin"), project());
    NodePtr constant = appPTR->getTopLevelInstance()->createNode(constantArgs);
    ASSERT_TRUE(bool(constant));
    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName("color").get());
    ASSERT_TRUE(color != NULL);
    color->setValues(0.18, 0.18, 0.18, 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    NodePtr writer = createWriter(PLUGINID_OFX_WRITEPNG, path);
    ASSERT_TRUE(bool(writer));
    ASSERT_EQ(std::string(kSRGBSpace), stringValue(writer, kOutputSpaceKnob));
    ASSERT_TRUE(project()->connectNodes(0, constant, writer));

    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);
    QFile::remove(QString::fromStdString(path));
    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
    appPTR->getTopLevelInstance()->startWritersRendering(false, works);
    ASSERT_TRUE(QFile::exists(QString::fromStdString(path))) << "frame was not rendered: " << path;

    DecodedPng png;
    std::string error;
    ASSERT_TRUE(decodePngFirstSample(path, &png, &error)) << error;
    EXPECT_NEAR(118, png.firstSample, 1);
}
