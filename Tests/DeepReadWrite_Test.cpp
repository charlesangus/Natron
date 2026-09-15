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
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/DeepImage.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Deep/DeepRead.h"
#include "Engine/Nodes/Deep/DeepWrite.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// What Tests/fixtures/deep-scanline.exr, deep-tiled.exr and deep-noncanonical.exr hold, in
// Natron's bottom-up pixel coordinates. All three were written by OpenImageIO's Python module,
// which shares no code with the nodes under test:
//
//     tools/ci/local/devshell.sh python3 Tests/fixtures/make-deep-fixtures.py Tests/fixtures
//
// and the values below were transcribed from an independent reader's view of the result,
// `oiiotool --info --dumpdata Tests/fixtures/deep-scanline.exr`, whose rows are in file order
// (top row first) and therefore appear here in the opposite order:
//
//     Pixel (0, 0): 0 samples
//     Pixel (1, 0): 1 samples : R=0.25 G=0.5 B=0.75 A=1 Z=3 ZBack=3.5 AOV=11
//     Pixel (2, 0): 2 samples : R=0.5 G=0.25 B=0.125 A=0.5 Z=5 ZBack=5.25 AOV=12 /  R=0.25 G=0.75 B=0.5 A=0.25 Z=2 ZBack=2.5 AOV=13
//     Pixel (3, 0): 3 samples : R=1 G=0 B=0 A=1 Z=1 ZBack=1 AOV=14 /  R=0 G=1 B=0 A=0.5 Z=4 ZBack=6 AOV=15 /  R=0 G=0 B=1 A=0.75 Z=5.5 ZBack=5.5 AOV=16
//     Pixel (0, 1): 2 samples : R=0.125 G=0.25 B=0.375 A=0.5 Z=7 ZBack=7.25 AOV=20 /  R=0.625 G=0.75 B=0.875 A=1 Z=8 ZBack=8.25 AOV=21
//     Pixel (1, 1): 0 samples
//     Pixel (2, 1): 1 samples : R=0.75 G=0.75 B=0.75 A=0.25 Z=9 ZBack=9 AOV=22
//     Pixel (3, 1): 1 samples : R=0.5 G=0.5 B=0.5 A=0.125 Z=10 ZBack=12 AOV=23
//     Pixel (0, 2): 1 samples : R=0.25 G=0.125 B=0.0625 A=0.5 Z=13 ZBack=13 AOV=30
//     Pixel (1, 2): 2 samples : R=0 G=0.25 B=0.5 A=0.75 Z=14 ZBack=14.5 AOV=31 /  R=0.25 G=0.5 B=0.75 A=1 Z=15 ZBack=15.5 AOV=32
//     Pixel (2, 2): 0 samples
//     Pixel (3, 2): 2 samples : R=1 G=1 B=1 A=1 Z=16 ZBack=16.25 AOV=33 /  R=0.5 G=0.25 B=0.125 A=0.5 Z=17 ZBack=17.5 AOV=34
//
// deep-noncanonical.exr holds the same sample structure and the same A, Z, ZBack and AOV values
// over a channel set with no RGB at all; deep-nozback.exr holds the same structure and the same
// R, G, B, A and Z values with no ZBack channel at all.
#define kDeepFixtureWidth 4
#define kDeepFixtureHeight 3

struct ExpectedSample {
    float r;
    float g;
    float b;
    float a;
    float z;
    float zback;
    float aov;
};

struct ExpectedPixel {
    int x;
    int y;
    int count;
    ExpectedSample samples[3];
};

const ExpectedPixel kExpectedPixels[] = {
    { 0, 2, 0, { { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f } } },
    { 1, 2, 1, { { 0.25f, 0.5f, 0.75f, 1.f, 3.f, 3.5f, 11.f } } },
    { 2, 2, 2, { { 0.5f, 0.25f, 0.125f, 0.5f, 5.f, 5.25f, 12.f }, { 0.25f, 0.75f, 0.5f, 0.25f, 2.f, 2.5f, 13.f } } },
    { 3, 2, 3, { { 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 14.f }, { 0.f, 1.f, 0.f, 0.5f, 4.f, 6.f, 15.f }, { 0.f, 0.f, 1.f, 0.75f, 5.5f, 5.5f, 16.f } } },
    { 0, 1, 2, { { 0.125f, 0.25f, 0.375f, 0.5f, 7.f, 7.25f, 20.f }, { 0.625f, 0.75f, 0.875f, 1.f, 8.f, 8.25f, 21.f } } },
    { 1, 1, 0, { { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f } } },
    { 2, 1, 1, { { 0.75f, 0.75f, 0.75f, 0.25f, 9.f, 9.f, 22.f } } },
    { 3, 1, 1, { { 0.5f, 0.5f, 0.5f, 0.125f, 10.f, 12.f, 23.f } } },
    { 0, 0, 1, { { 0.25f, 0.125f, 0.0625f, 0.5f, 13.f, 13.f, 30.f } } },
    { 1, 0, 2, { { 0.f, 0.25f, 0.5f, 0.75f, 14.f, 14.5f, 31.f }, { 0.25f, 0.5f, 0.75f, 1.f, 15.f, 15.5f, 32.f } } },
    { 2, 0, 0, { { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f } } },
    { 3, 0, 2, { { 1.f, 1.f, 1.f, 1.f, 16.f, 16.25f, 33.f }, { 0.5f, 0.25f, 0.125f, 0.5f, 17.f, 17.5f, 34.f } } },
};

float
expectedChannelValue(const ExpectedSample& sample,
                     const std::string& channel)
{
    if (channel == "R") {
        return sample.r;
    }
    if (channel == "G") {
        return sample.g;
    }
    if (channel == "B") {
        return sample.b;
    }
    if (channel == "A") {
        return sample.a;
    }
    if (channel == "Z") {
        return sample.z;
    }
    if (channel == "ZBack") {
        return sample.zback;
    }
    if (channel == "AOV") {
        return sample.aov;
    }

    // Nothing compares equal to this, so an unexpected channel name fails rather than passes.
    return std::numeric_limits<float>::quiet_NaN();
}

std::vector<std::string>
channelNamesOf(const DeepImage& image)
{
    std::vector<std::string> names;

    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = image.getChannels().begin(); it != image.getChannels().end(); ++it) {
        names.push_back(it->first);
    }

    return names;
}

// Every pixel of the fixture that bounds covers, sample by sample, against the hard-coded table
// above. expectedChannels is the complete set of channels the file under test carries, so a
// channel appearing that should not -- or missing that should be there -- fails here too.
// zBackIsZ is for a file carrying no ZBack: its samples are point samples, so their back must
// have been read as their front rather than left at zero.
void
expectFixtureContents(const DeepImage& image,
                      const std::vector<std::string>& expectedChannels,
                      const RectI& bounds,
                      bool zBackIsZ = false)
{
    ASSERT_TRUE(image.getBounds() == bounds) << "bounds are " << image.getBounds().x1 << "," << image.getBounds().y1 << " " << image.getBounds().x2 << "," << image.getBounds().y2;
    ASSERT_EQ(expectedChannels, channelNamesOf(image));

    // The fixture's samples are stored back-to-front in one pixel, so nothing downstream may be
    // told they are sorted.
    EXPECT_FALSE(image.isTidy());

    const std::size_t pixelCount = sizeof(kExpectedPixels) / sizeof(kExpectedPixels[0]);
    ASSERT_EQ((std::size_t)(kDeepFixtureWidth * kDeepFixtureHeight), pixelCount);

    U64 expectedTotal = 0;
    for (std::size_t p = 0; p < pixelCount; ++p) {
        if (bounds.contains(kExpectedPixels[p].x, kExpectedPixels[p].y)) {
            expectedTotal += (U64)kExpectedPixels[p].count;
        }
    }
    ASSERT_EQ(expectedTotal, image.getSampleTable().getTotalSampleCount());

    for (std::size_t p = 0; p < pixelCount; ++p) {
        const ExpectedPixel& expected = kExpectedPixels[p];

        if (!bounds.contains(expected.x, expected.y)) {
            continue;
        }

        const std::size_t index = ((std::size_t)(expected.y - bounds.y1) * (std::size_t)bounds.width()) + (std::size_t)(expected.x - bounds.x1);
        ASSERT_EQ((U32)expected.count, image.getSampleTable().getCount(index)) << "at pixel (" << expected.x << ", " << expected.y << ")";

        const U64 offset = image.getSampleTable().getOffset(index);
        for (int s = 0; s < expected.count; ++s) {
            for (std::size_t c = 0; c < expectedChannels.size(); ++c) {
                const DeepChannelBuffer* buffer = image.getChannel(expectedChannels[c]);
                ASSERT_TRUE(buffer != NULL) << "missing channel " << expectedChannels[c];
                const bool asPointSample = zBackIsZ && (expectedChannels[c] == "ZBack");
                const float expectedValue = expectedChannelValue(expected.samples[s], asPointSample ? std::string("Z") : expectedChannels[c]);
                ASSERT_FLOAT_EQ(expectedValue, buffer->data()[offset + (U64)s])
                    << "at pixel (" << expected.x << ", " << expected.y << ") sample " << s << " channel " << expectedChannels[c];
            }
        }
    }
} // expectFixtureContents

std::vector<std::string>
fullChannelSet()
{
    std::vector<std::string> names;

    // DeepImage keeps its channels name-ordered, which is the order this must be in.
    names.push_back("A");
    names.push_back("AOV");
    names.push_back("B");
    names.push_back("G");
    names.push_back("R");
    names.push_back("Z");
    names.push_back("ZBack");

    return names;
}

std::vector<std::string>
nozbackChannelSet()
{
    std::vector<std::string> names;

    names.push_back("A");
    names.push_back("B");
    names.push_back("G");
    names.push_back("R");
    names.push_back("Z");
    names.push_back("ZBack");

    return names;
}

std::vector<std::string>
noncanonicalChannelSet()
{
    std::vector<std::string> names;

    names.push_back("A");
    names.push_back("AOV");
    names.push_back("Z");
    names.push_back("ZBack");

    return names;
}

QString
fixturePath(const char* name)
{
    return QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/") + QString::fromUtf8(name);
}

// Runs oiiotool, the reference implementation's own command line tool, and hands back what it
// printed. Nothing here is skipped when it is absent: the image both CI and tools/ci/local use
// ships it, and a silent skip would turn the only independent check of DeepWrite's output into
// no check at all.
int
runOiiotool(const QStringList& args,
            QString* output)
{
    QProcess process;

    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QString::fromUtf8("oiiotool"), args);
    if (!process.waitForStarted(30000)) {
        *output = QString::fromUtf8("oiiotool could not be started");

        return -2;
    }
    if (!process.waitForFinished(120000)) {
        *output = QString::fromUtf8("oiiotool did not finish");

        return -3;
    }
    *output = QString::fromUtf8(process.readAll());

    return (process.exitStatus() == QProcess::NormalExit) ? process.exitCode() : -1;
}

} // namespace

class DeepReadWriteTest
    : public BaseTest {
protected:
    // Renders node's deep data the way the scheduler does, so EffectInstance::aborted() has
    // something to read.
    EffectInstance::RenderRoIRetCode renderDeepFrame(const NodePtr& node,
                                                     double time,
                                                     const RectI& roi,
                                                     DeepImagePtr* outputDeepImage,
                                                     unsigned int mipmapLevel = 0)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstance::RenderDeepRoIArgs args(time,
                                               RenderScale::fromMipmapLevel(mipmapLevel),
                                               mipmapLevel,
                                               ViewIdx(0),
                                               false /*byPassCache*/,
                                               roi,
                                               RectD(),
                                               0 /*caller*/,
                                               time);

        return node->getEffectInstance()->renderDeepRoI(args, outputDeepImage);
    }

    NodePtr createDeepRead(const QString& filename)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPREAD));

        if (!read) {
            return read;
        }
        KnobFile* knob = dynamic_cast<KnobFile*>(read->getKnobByName("filename").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(filename.toStdString());

        return read;
    }

    NodePtr createDeepWrite(const QString& filename,
                            bool tiled)
    {
        NodePtr write = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPWRITE));

        if (!write) {
            return write;
        }
        KnobOutputFile* knob = dynamic_cast<KnobOutputFile*>(write->getKnobByName("filename").get());
        KnobBool* tiledKnob = dynamic_cast<KnobBool*>(write->getKnobByName("tiled").get());
        if (!knob || !tiledKnob) {
            return NodePtr();
        }
        knob->setValue(filename.toStdString());
        tiledKnob->setValue(tiled);

        return write;
    }

    // The whole frame, which is also the fixtures' region of definition.
    static RectI fullFrame()
    {
        return RectI(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);
    }
};

TEST_F(DeepReadWriteTest, BothNodesAreRegisteredAndInstantiable)
{
    NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPREAD));
    NodePtr write = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPWRITE));

    ASSERT_TRUE(read != NULL);
    ASSERT_TRUE(write != NULL);
    EXPECT_EQ(eDataKindDeep, read->getEffectInstance()->getOutputDataKind());
    EXPECT_EQ(eDataKindDeep, write->getEffectInstance()->getOutputDataKind());
    EXPECT_EQ(eDataKindDeep, write->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(0, read->getEffectInstance()->getNInputs());
    EXPECT_EQ(1, write->getEffectInstance()->getNInputs());

    // Connecting the pair is what makes the chain the tests below render legal at all.
    connectNodes(read, write, 0, true);
}

TEST_F(DeepReadWriteTest, OutputFormatIsTheFilesDisplayWindowNotTheProjectDefault)
{
    NodePtr read = createDeepRead(fixturePath("deep-scanline.exr"));

    ASSERT_TRUE(read != NULL);
    read->getEffectInstance()->refreshMetadata_public(false);

    EXPECT_TRUE(fullFrame() == read->getEffectInstance()->getOutputFormat());
}

// Reproduces the real-world bug report: with no explicit refresh call, setting the filename
// knob (as the GUI's file dialog and Python's Param.set() both do) must itself trigger a
// metadata refresh, the same way an OFX reader's clip-preferences-slave params do.
TEST_F(DeepReadWriteTest, OutputFormatRefreshesWhenTheFileKnobChangesWithNoExplicitRefresh)
{
    NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPREAD));

    ASSERT_TRUE(read != NULL);

    const RectI projectDefaultFormat = read->getEffectInstance()->getOutputFormat();

    KnobFile* knob = dynamic_cast<KnobFile*>(read->getKnobByName("filename").get());
    ASSERT_TRUE(knob != NULL);
    knob->setValue(fixturePath("deep-scanline.exr").toStdString());

    EXPECT_TRUE(fullFrame() == read->getEffectInstance()->getOutputFormat());
    EXPECT_FALSE(fullFrame() == projectDefaultFormat);
}

TEST_F(DeepReadWriteTest, ReadsADeepScanlinePartExactly)
{
    NodePtr read = createDeepRead(fixturePath("deep-scanline.exr"));

    ASSERT_TRUE(read != NULL);

    DeepImagePtr image;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(read, 1., fullFrame(), &image));
    ASSERT_TRUE(image != NULL);
    expectFixtureContents(*image, fullChannelSet(), fullFrame());
}

TEST_F(DeepReadWriteTest, ReadsADeepTiledPartExactly)
{
    NodePtr read = createDeepRead(fixturePath("deep-tiled.exr"));

    ASSERT_TRUE(read != NULL);

    DeepImagePtr image;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(read, 1., fullFrame(), &image));
    ASSERT_TRUE(image != NULL);
    expectFixtureContents(*image, fullChannelSet(), fullFrame());
}

// A reduced-resolution pixel carries the sample list of the first full-resolution pixel of the
// block it covers: at level 1 the 4x3 fixture reads as 2x2, and (1, 1) holds what (2, 2) holds.
TEST_F(DeepReadWriteTest, ReadsAtAReducedMipmapLevelBySubsampling)
{
    NodePtr read = createDeepRead(fixturePath("deep-scanline.exr"));

    ASSERT_TRUE(read != NULL);

    const RectI halfFrame(0, 0, 2, 2);
    DeepImagePtr image;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(read, 1., halfFrame, &image, 1 /*mipmapLevel*/));
    ASSERT_TRUE(image != NULL);
    ASSERT_TRUE(image->getBounds() == halfFrame);

    const SampleTable& table = image->getSampleTable();
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const ExpectedPixel* expected = 0;
            for (std::size_t p = 0; p < sizeof(kExpectedPixels) / sizeof(kExpectedPixels[0]); ++p) {
                if ((kExpectedPixels[p].x == 2 * x) && (kExpectedPixels[p].y == 2 * y)) {
                    expected = &kExpectedPixels[p];
                }
            }
            ASSERT_TRUE(expected != NULL);

            const std::size_t index = ((std::size_t)y * 2) + (std::size_t)x;
            ASSERT_EQ((U32)expected->count, table.getCount(index)) << "at pixel (" << x << ", " << y << ")";

            const U64 offset = table.getOffset(index);
            const std::vector<std::string> channels = fullChannelSet();
            for (int s = 0; s < expected->count; ++s) {
                for (std::size_t c = 0; c < channels.size(); ++c) {
                    const DeepChannelBuffer* buffer = image->getChannel(channels[c]);
                    ASSERT_TRUE(buffer != NULL) << "missing channel " << channels[c];
                    ASSERT_FLOAT_EQ(expectedChannelValue(expected->samples[s], channels[c]), buffer->data()[offset + (U64)s])
                        << "at pixel (" << x << ", " << y << ") sample " << s << " channel " << channels[c];
                }
            }
        }
    }
}

// The channels of deep-noncanonical.exr reach a reader as A, Z, ZBack, AOV: the alpha is first,
// the depth is second and there is no RGB at all, so anything that read channels by position
// rather than by name lands the values in the wrong places.
TEST_F(DeepReadWriteTest, ReadsAFileWhoseChannelsAreNotInRGBAZOrder)
{
    NodePtr read = createDeepRead(fixturePath("deep-noncanonical.exr"));

    ASSERT_TRUE(read != NULL);

    DeepImagePtr image;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(read, 1., fullFrame(), &image));
    ASSERT_TRUE(image != NULL);
    expectFixtureContents(*image, noncanonicalChannelSet(), fullFrame());
    EXPECT_TRUE(image->getChannel("R") == NULL);
    EXPECT_TRUE(image->getChannel("G") == NULL);
    EXPECT_TRUE(image->getChannel("B") == NULL);
}

// deep-nozback.exr carries no ZBack channel at all, so per the deep spec every sample in it is a
// point sample: its back must read as equal to its front rather than as zero, which would
// instead read as a zero-thickness volumetric sample starting at the origin.
TEST_F(DeepReadWriteTest, ReadsAFileWithNoZBackChannelAsPointSamples)
{
    NodePtr read = createDeepRead(fixturePath("deep-nozback.exr"));

    ASSERT_TRUE(read != NULL);

    DeepImagePtr image;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(read, 1., fullFrame(), &image));
    ASSERT_TRUE(image != NULL);
    expectFixtureContents(*image, nozbackChannelSet(), fullFrame(), true /*zBackIsZ*/);
}

TEST_F(DeepReadWriteTest, WritesADeepScanlinePartOiiotoolCannotTellFromTheFixture)
{
    QTemporaryDir tmp;

    ASSERT_TRUE(tmp.isValid());
    const QString written = tmp.path() + QString::fromUtf8("/written-scanline.exr");

    NodePtr read = createDeepRead(fixturePath("deep-scanline.exr"));
    NodePtr write = createDeepWrite(written, false /*tiled*/);
    ASSERT_TRUE(read != NULL);
    ASSERT_TRUE(write != NULL);
    connectNodes(read, write, 0, true);

    DeepImagePtr image;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(write, 1., fullFrame(), &image));
    ASSERT_TRUE(image != NULL);

    // DeepWrite passes its input through as well as writing it.
    expectFixtureContents(*image, fullChannelSet(), fullFrame());

    QString output;
    const int rc = runOiiotool(QStringList() << QString::fromUtf8("--diff") << fixturePath("deep-scanline.exr") << written, &output);
    EXPECT_EQ(0, rc) << output.toStdString();

    // A diff that compared nothing would also return 0, so pin that it can fail: the fixture
    // differs from the file with no RGB in it.
    QString controlOutput;
    const int controlRc = runOiiotool(QStringList() << QString::fromUtf8("--diff") << fixturePath("deep-scanline.exr") << fixturePath("deep-noncanonical.exr"), &controlOutput);
    EXPECT_NE(0, controlRc) << controlOutput.toStdString();

    // And the part written is a scanline one, as asked for.
    QString infoOutput;
    ASSERT_EQ(0, runOiiotool(QStringList() << QString::fromUtf8("--info") << QString::fromUtf8("-v") << written, &infoOutput)) << infoOutput.toStdString();
    EXPECT_FALSE(infoOutput.contains(QString::fromUtf8("tile size"))) << infoOutput.toStdString();
}

TEST_F(DeepReadWriteTest, WritesADeepTiledPartOiiotoolCannotTellFromTheFixture)
{
    QTemporaryDir tmp;

    ASSERT_TRUE(tmp.isValid());
    const QString written = tmp.path() + QString::fromUtf8("/written-tiled.exr");

    NodePtr read = createDeepRead(fixturePath("deep-scanline.exr"));
    NodePtr write = createDeepWrite(written, true /*tiled*/);
    ASSERT_TRUE(read != NULL);
    ASSERT_TRUE(write != NULL);
    connectNodes(read, write, 0, true);

    DeepImagePtr image;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(write, 1., fullFrame(), &image));
    ASSERT_TRUE(image != NULL);

    QString infoOutput;
    ASSERT_EQ(0, runOiiotool(QStringList() << QString::fromUtf8("--info") << QString::fromUtf8("-v") << written, &infoOutput)) << infoOutput.toStdString();
    EXPECT_TRUE(infoOutput.contains(QString::fromUtf8("tile size"))) << infoOutput.toStdString();

    QString output;
    const int rc = runOiiotool(QStringList() << QString::fromUtf8("--diff") << fixturePath("deep-scanline.exr") << written, &output);
    EXPECT_EQ(0, rc) << output.toStdString();
}

// Reading back what DeepWrite produced cannot catch a bug the two nodes share, which is what the
// hard-coded table and the oiiotool diffs above are for; what it does catch is anything the file
// loses that oiiotool's diff does not compare -- the channel names and the data window.
TEST_F(DeepReadWriteTest, WhatDeepWriteWroteReadsBackAsTheFixtureDid)
{
    QTemporaryDir tmp;

    ASSERT_TRUE(tmp.isValid());
    const QString written = tmp.path() + QString::fromUtf8("/round-trip.exr");

    NodePtr read = createDeepRead(fixturePath("deep-noncanonical.exr"));
    NodePtr write = createDeepWrite(written, false /*tiled*/);
    ASSERT_TRUE(read != NULL);
    ASSERT_TRUE(write != NULL);
    connectNodes(read, write, 0, true);

    DeepImagePtr image;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(write, 1., fullFrame(), &image));
    ASSERT_TRUE(image != NULL);

    NodePtr reread = createDeepRead(written);
    ASSERT_TRUE(reread != NULL);

    DeepImagePtr rereadImage;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(reread, 1., fullFrame(), &rereadImage));
    ASSERT_TRUE(rereadImage != NULL);
    expectFixtureContents(*rereadImage, noncanonicalChannelSet(), fullFrame());
}
