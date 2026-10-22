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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "DataKindTestEffect.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/RawExtensions.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
// None of these exist: the kind is decided by the extension alone, before any file is opened.
const char* const kPngPath = "/nonexistent/native-source-kind/plate.png";
const char* const kRawPath = "/nonexistent/native-source-kind/plate.CR2";

void
setFile(const NodePtr& read,
        const std::string& path)
{
    KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());

    ASSERT_TRUE(file != NULL);
    file->setValue(path);
}

DataKindEnum
kindOf(const NodePtr& node)
{
    bool ambiguous = true;
    const DataKindEnum kind = node->getEffectiveOutputDataKind(&ambiguous);

    EXPECT_FALSE(ambiguous);

    return kind;
}
} // namespace

class NativeSourceKindTest
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

    NodePtr createRead(const std::string& path)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);

        EXPECT_TRUE(read && dynamic_cast<NativeRead*>(read->getEffectInstance().get()));
        if (read && !path.empty()) {
            setFile(read, path);
        }

        return read;
    }
};

TEST(RawExtensionsTest, MatchesTheExtensionCaseInsensitively)
{
    EXPECT_TRUE(RawExtensions::isRawExtension("cr2"));
    EXPECT_TRUE(RawExtensions::isRawExtension(".NEF"));
    EXPECT_TRUE(RawExtensions::isRawExtension("Dng"));
    EXPECT_TRUE(RawExtensions::isRawExtension("3FR"));
    EXPECT_FALSE(RawExtensions::isRawExtension("png"));
    EXPECT_FALSE(RawExtensions::isRawExtension(""));
    // No rawspeed decoder exists for these at the pinned commit.
    EXPECT_FALSE(RawExtensions::isRawExtension("cr3"));
    EXPECT_FALSE(RawExtensions::isRawExtension("x3f"));

    EXPECT_TRUE(RawExtensions::isRawPath("/shots/plate.####.ARW"));
    EXPECT_TRUE(RawExtensions::isRawPath("plate.%04d.raf"));
    EXPECT_FALSE(RawExtensions::isRawPath("/shots/a.dng/plate.exr"));
    EXPECT_FALSE(RawExtensions::isRawPath("/shots/a.dng/plate"));
    EXPECT_FALSE(RawExtensions::isRawPath("plate."));
    EXPECT_FALSE(RawExtensions::isRawPath(""));
}

TEST_F(NativeSourceKindTest, FilenameFlipsTheKindBetweenImageAndRaw)
{
    NodePtr read = createRead(std::string());

    ASSERT_TRUE(bool(read));
    EXPECT_EQ(eDataKindImage, kindOf(read));

    setFile(read, kPngPath);
    EXPECT_EQ(eDataKindImage, kindOf(read));

    setFile(read, kRawPath);
    EXPECT_EQ(eDataKindRaw, kindOf(read));

    setFile(read, kPngPath);
    EXPECT_EQ(eDataKindImage, kindOf(read));

    setFile(read, std::string());
    EXPECT_EQ(eDataKindImage, kindOf(read));
}

// The image consumers a Read has always fed -- here an image-only input, behind a Dot -- see the
// same image kind as before for a non-raw file, with nothing to report.
TEST_F(NativeSourceKindTest, NonRawReadStillFeedsImageConsumers)
{
    NodePtr read = createRead(kPngPath);
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(read && dot && imageSink);
    EXPECT_EQ(Node::eCanConnectInput_ok, imageSink->canConnectInput(read, 0));
    connectNodes(read, dot, 0, true);
    connectNodes(dot, imageSink, 0, true);

    EXPECT_EQ(eDataKindImage, kindOf(dot));
    EXPECT_FALSE(imageSink->hasPersistentMessage());
    EXPECT_FALSE(dot->hasPersistentMessage());
}

// Switching the file to a raw one never disconnects anything: the image consumer is put in error,
// as the existing conflict reporting does for any edge whose kind no longer fits, and switching back
// takes the error away again.
TEST_F(NativeSourceKindTest, SwitchingToRawKeepsTheEdgeAndReportsTheImageConsumer)
{
    NodePtr read = createRead(kPngPath);
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(read && dot && imageSink);
    connectNodes(read, dot, 0, true);
    connectNodes(dot, imageSink, 0, true);
    ASSERT_FALSE(imageSink->hasPersistentMessage());

    setFile(read, kRawPath);
    EXPECT_EQ(eDataKindRaw, kindOf(read));
    EXPECT_EQ(dot, imageSink->getInput(0));
    EXPECT_EQ(read, dot->getInput(0));
    EXPECT_TRUE(imageSink->hasPersistentMessage());

    setFile(read, kPngPath);
    EXPECT_EQ(eDataKindImage, kindOf(dot));
    EXPECT_FALSE(imageSink->hasPersistentMessage());
}

TEST_F(NativeSourceKindTest, RawConsumerReportsAConflictWhenTheReadFlipsBackToImage)
{
    NodePtr read = createRead(kRawPath);
    NodePtr rawSink = createNode(QString::fromUtf8(kTestPluginIDDataKindRawSink));

    ASSERT_TRUE(read && rawSink);
    EXPECT_EQ(Node::eCanConnectInput_ok, rawSink->canConnectInput(read, 0));
    connectNodes(read, rawSink, 0, true);
    EXPECT_FALSE(rawSink->hasPersistentMessage());

    setFile(read, kPngPath);
    EXPECT_EQ(eDataKindImage, kindOf(read));
    EXPECT_EQ(read, rawSink->getInput(0));
    EXPECT_TRUE(rawSink->isInputDataKindUnacceptable(read, 0));
    EXPECT_TRUE(rawSink->hasPersistentMessage());

    setFile(read, kRawPath);
    EXPECT_FALSE(rawSink->isInputDataKindUnacceptable(read, 0));
    EXPECT_FALSE(rawSink->hasPersistentMessage());
}

// A connection made while the Read holds a png is checked against the png's kind.
TEST_F(NativeSourceKindTest, ConnectionCheckFollowsTheCurrentFile)
{
    NodePtr read = createRead(kPngPath);
    NodePtr rawSink = createNode(QString::fromUtf8(kTestPluginIDDataKindRawSink));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(read && rawSink && imageSink);
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, rawSink->canConnectInput(read, 0));
    EXPECT_EQ(Node::eCanConnectInput_ok, imageSink->canConnectInput(read, 0));

    setFile(read, kRawPath);
    EXPECT_EQ(Node::eCanConnectInput_ok, rawSink->canConnectInput(read, 0));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, imageSink->canConnectInput(read, 0));
}

// The kind is never serialized, so a reloaded Read has to arrive at it again from its restored
// filename, with every consumer that was satisfied before still satisfied.
TEST_F(NativeSourceKindTest, ResolvedKindSurvivesSaveAndLoad)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr rawRead = createRead(kRawPath);
    NodePtr rawSink = createNode(QString::fromUtf8(kTestPluginIDDataKindRawSink));
    NodePtr pngRead = createRead(kPngPath);
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(rawRead && rawSink && pngRead && imageSink);
    connectNodes(rawRead, rawSink, 0, true);
    connectNodes(pngRead, imageSink, 0, true);

    const std::string rawReadName = rawRead->getScriptName();
    const std::string rawSinkName = rawSink->getScriptName();
    const std::string pngReadName = pngRead->getScriptName();
    const std::string imageSinkName = imageSink->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("native-source-kind.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr rawRead2 = project->getNodeByName(rawReadName);
    NodePtr rawSink2 = project->getNodeByName(rawSinkName);
    NodePtr pngRead2 = project->getNodeByName(pngReadName);
    NodePtr imageSink2 = project->getNodeByName(imageSinkName);
    ASSERT_TRUE(rawRead2 && rawSink2 && pngRead2 && imageSink2);

    EXPECT_EQ(eDataKindRaw, kindOf(rawRead2));
    EXPECT_EQ(eDataKindImage, kindOf(pngRead2));
    EXPECT_EQ(rawRead2, rawSink2->getInput(0));
    EXPECT_EQ(pngRead2, imageSink2->getInput(0));
    EXPECT_FALSE(rawSink2->hasPersistentMessage());
    EXPECT_FALSE(imageSink2->hasPersistentMessage());
}

// Rendering a Read on a raw file reports an error on the Read and renders nothing; OpenImageIO
// never gets to read the file as an image.
TEST_F(NativeSourceKindTest, RawFileFailsCleanlyWithoutARawDecoder)
{
    NodePtr read = createRead(kRawPath);

    ASSERT_TRUE(bool(read));

    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    EXPECT_FALSE(renderNodePlanesDirect(read, 1., ViewIdx(0), 0, RectI(0, 0, 8, 8), layers, &planes, &error));
    EXPECT_TRUE(planes.empty());
    EXPECT_TRUE(read->hasPersistentMessage());

    QString message;
    int type = 0;
    read->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("raw"), Qt::CaseInsensitive)) << message.toStdString();
}
