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
#include <cctype>
#include <cmath>
#include <list>
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

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"
#include "Engine/WriteNode.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 16;
const int kHeight = 8;

bool
isNativeRead(const NodePtr& node)
{
    return node && dynamic_cast<NativeRead*>(node->getEffectInstance().get());
}

float
patternValue(int x,
             int y,
             int c)
{
    return (float)((x * 13 + y * 7 + c * 5) % 61) / 60.f;
}

std::string
writeFixture(const QTemporaryDir& dir,
             const char* name,
             OIIO::TypeDesc type)
{
    const std::string path = (dir.path() + QString::fromUtf8("/") + QString::fromUtf8(name)).toStdString();
    OIIO::ImageSpec spec(kWidth, kHeight, 4, type);
    std::vector<float> pixels((std::size_t)kWidth * kHeight * 4);
    for (int j = 0; j < kHeight; ++j) {
        for (int i = 0; i < kWidth; ++i) {
            for (int c = 0; c < 4; ++c) {
                pixels[((std::size_t)j * kWidth + i) * 4 + c] = patternValue(i, j, c);
            }
        }
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    EXPECT_TRUE(bool(out)) << name;
    if (!out) {
        return std::string();
    }
    EXPECT_TRUE(out->open(path, spec)) << out->geterror();
    EXPECT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
    EXPECT_TRUE(out->close()) << out->geterror();

    return path;
}
} // namespace

class NativeReadEntryPointsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

TEST_F(NativeReadEntryPointsTest, UnversionedReadIsTheNativeNode)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ));

    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(isNativeRead(node));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_READ, node->getMajorVersion());
}

TEST_F(NativeReadEntryPointsTest, ExplicitOfxReaderIDBuildsNothing)
{
    CreateNodeArgs args(PLUGINID_OFX_READOIIO, getApp()->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);

    EXPECT_FALSE(bool(getApp()->createNode(args)));
}

TEST_F(NativeReadEntryPointsTest, CreateReaderBuildsANativeReadForEveryFormat)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const struct {
        const char* file;
        OIIO::TypeDesc type;
    } cases[] = {
        { "a.exr", OIIO::TypeDesc::FLOAT },
        { "a.png", OIIO::TypeDesc::UINT8 },
        { "a.dpx", OIIO::TypeDesc::UINT16 },
        { "a.tga", OIIO::TypeDesc::UINT8 },
    };

    for (const auto& c : cases) {
        const std::string path = writeFixture(tmp, c.file, c.type);
        ASSERT_FALSE(path.empty()) << c.file;

        CreateNodeArgs args(PLUGINID_NATRON_READ, getApp()->getProject());
        NodePtr node = getApp()->createReader(path, args);
        ASSERT_TRUE(bool(node)) << c.file;
        EXPECT_TRUE(isNativeRead(node)) << c.file;
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(file != NULL) << c.file;
        EXPECT_EQ(path, file->getValue()) << c.file;
    }
}

TEST_F(NativeReadEntryPointsTest, ReaderLookupFollowsTheReadableExtensions)
{
    const std::vector<std::string>& readable = OiioReadSupport::readableExtensions();
    ASSERT_FALSE(readable.empty());

    std::vector<std::string> supported;
    appPTR->getSupportedReaderFileFormats(&supported);
    EXPECT_EQ(readable, supported);

    for (const std::string& ext : readable) {
        EXPECT_EQ(std::string(PLUGINID_NATRON_READ), appPTR->getReaderPluginIDForFileType(ext)) << ext;
        std::string upper = ext;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return (char)std::toupper(ch); });
        EXPECT_EQ(std::string(PLUGINID_NATRON_READ), appPTR->getReaderPluginIDForFileType(upper)) << upper;
    }
    EXPECT_EQ(std::string(), appPTR->getReaderPluginIDForFileType("cr2"));
    EXPECT_EQ(std::string(), appPTR->getReaderPluginIDForFileType("mov"));
    EXPECT_EQ(std::string(), appPTR->getReaderPluginIDForFileType(""));
}

TEST_F(NativeReadEntryPointsTest, UnversionedReadKeepsItsClassThroughSaveAndLoad)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ));
    ASSERT_TRUE(isNativeRead(node));
    const std::string name = node->getScriptName();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("unversioned-read.ntp");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_TRUE(isNativeRead(loaded));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_READ, loaded->getMajorVersion());
}

TEST_F(NativeReadEntryPointsTest, WriteReadBackIsANativeReadThatRendersTheWrittenFile)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string source = writeFixture(tmp, "source.exr", OIIO::TypeDesc::FLOAT);
    ASSERT_FALSE(source.empty());

    CreateNodeArgs readArgs(PLUGINID_NATRON_READ, getApp()->getProject());
    NodePtr read = getApp()->createReader(source, readArgs);
    ASSERT_TRUE(isNativeRead(read));

    NodePtr write = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(write));
    connectNodes(read, write, 0, true);

    const std::string output = (tmp.path() + QString::fromUtf8("/written.exr")).toStdString();
    write->getEffectInstance()->setOutputFilesForWriter(output);

    std::list<AppInstance::RenderWork> works;
    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(write->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);
    works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
    getApp()->startWritersRendering(false, works);
    ASSERT_TRUE(QFile::exists(QString::fromStdString(output)));

    KnobBool* readBack = dynamic_cast<KnobBool*>(write->getKnobByName(kNatronWriteParamReadBack).get());
    ASSERT_TRUE(readBack != NULL);
    readBack->setValue(true);

    NodeGroup* group = dynamic_cast<NodeGroup*>(write->getEffectInstance().get());
    ASSERT_TRUE(group != NULL);
    NodePtr decoder = group->getNodeByName("internalDecoderNode");
    ASSERT_TRUE(bool(decoder));
    EXPECT_TRUE(isNativeRead(decoder));

    KnobFile* file = dynamic_cast<KnobFile*>(decoder->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(file != NULL);
    EXPECT_EQ(output, file->getValue());

    KnobIPtr outputSpace = write->getKnobByName("ocioOutputSpace");
    KnobIPtr inputSpace = decoder->getKnobByName("ocioInputSpace");
    ASSERT_TRUE(bool(inputSpace));
    if (outputSpace) {
        EXPECT_TRUE(inputSpace->getMaster(0).second.get() == outputSpace.get()) << "the decoder's input space is not bound to the Write's output space";
    }

    // The Write converts to its output space and the decoder back from it, so the round trip
    // returns what the source Read produced, to the precision of a half-float file.
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> expected, planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(read, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), layers, &expected, &error)) << error;
    ASSERT_TRUE(renderNodePlanesDirect(decoder, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), layers, &planes, &error)) << error;
    ASSERT_EQ(1u, expected.size());
    ASSERT_EQ(1u, planes.size());
    ASSERT_EQ((std::size_t)kWidth * kHeight * 4, planes[0].pixels.size());
    ASSERT_EQ(expected[0].pixels.size(), planes[0].pixels.size());
    double maxDiff = 0.;
    for (std::size_t i = 0; i < planes[0].pixels.size(); ++i) {
        maxDiff = std::max(maxDiff, (double)std::abs(expected[0].pixels[i] - planes[0].pixels[i]));
    }
    EXPECT_LT(maxDiff, 2e-3);
}
