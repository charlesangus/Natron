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
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>
#include <ofxMetadata.h>

#include "BaseTest.h"
#include "RawFixtures.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/RawDecodeSupport.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/IO/RawspeedRuntime.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

class NativeReadRawTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        RawDecodeSupport::clearCaches();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        RawDecodeSupport::clearCaches();
        BaseTest::TearDown();
    }

    NodePtr createRead(const std::string& path)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);

        EXPECT_TRUE(read && dynamic_cast<NativeRead*>(read->getEffectInstance().get()));
        if (read) {
            KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());
            EXPECT_TRUE(file != NULL);
            if (file) {
                file->setValue(path);
            }
        }

        return read;
    }

    static ImageMetadata outputMetadata(const NodePtr& node,
                                        double time = 0.)
    {
        NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());

        return effect ? effect->getOutputMetadata(time, ViewIdx(0)) : ImageMetadata();
    }

    static RectI regionOfDefinition(const NodePtr& node)
    {
        RectD rod;
        bool isProjectFormat = false;
        EffectInstancePtr effect = node->getEffectInstance();
        EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), 0., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));

        return RectI((int)rod.x1, (int)rod.y1, (int)rod.x2, (int)rod.y2);
    }

    static std::string persistentMessage(const NodePtr& node)
    {
        QString message;
        int type = 0;

        node->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }

    static bool isSecret(const NodePtr& node,
                         const std::string& name)
    {
        KnobIPtr knob = node->getKnobByName(name);

        EXPECT_TRUE(bool(knob)) << name;

        return knob && knob->getIsSecret();
    }
};

TEST_F(NativeReadRawTest, EachFixtureReadsAsRawWithItsChannelsMetadataAndPixels)
{
    for (const RawFixture& fixture : rawFixtures()) {
        SCOPED_TRACE(fixture.file);
        std::string error;
        const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(fixture.path(), &error);
        ASSERT_TRUE(bool(header)) << error;

        NodePtr read = createRead(fixture.path());
        ASSERT_TRUE(bool(read));
        EffectInstancePtr effect = read->getEffectInstance();
        EXPECT_EQ(eDataKindRaw, read->getEffectiveOutputDataKind());
        EXPECT_EQ(fixture.channels, EffectInstance::getNCompsFromMetadata(*effect->getTimeInvariantMetadata(), -1));
        EXPECT_EQ(header->window, regionOfDefinition(read));

        const ImageMetadata metadata = outputMetadata(read);
        for (const char* key : { kRawMetadataCfaPattern, kRawMetadataBlackLevels, kRawMetadataWhiteLevel, kRawMetadataCrop, kRawMetadataStage }) {
            EXPECT_TRUE(metadata.contains(key)) << key;
        }
        EXPECT_EQ(std::optional<RawStageEnum>(eRawStageRaw), RawMetadata::getStage(metadata));
        EXPECT_EQ(std::optional<RectI>(header->window), RawMetadata::getCrop(metadata));
        EXPECT_EQ(RawMetadata::getBlackLevels(header->metadata), RawMetadata::getBlackLevels(metadata));
        EXPECT_EQ(RawMetadata::getWhiteLevel(header->metadata), RawMetadata::getWhiteLevel(metadata));
        EXPECT_EQ(RawMetadata::getWbCoeffs(header->metadata), RawMetadata::getWbCoeffs(metadata));
        EXPECT_EQ(RawMetadata::getCamToXyz(header->metadata), RawMetadata::getCamToXyz(metadata));
        EXPECT_EQ(header->metadata.getString(kRawMetadataCfaPattern), metadata.getString(kRawMetadataCfaPattern));
        EXPECT_EQ(std::optional<std::string>(fixture.path()), metadata.getString(kOfxMetadataKeyFilePath));
        const std::optional<RawLayout> layout = RawLayout::fromMetadata(metadata);
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(fixture.period, layout->period());

        // An odd offset into the active area, so the window does not sit on the CFA period.
        const RectI window(header->window.x1 + 37, header->window.y1 + 23, header->window.x1 + 37 + 48, header->window.y1 + 23 + 40);
        RenderedPlane plane;
        ASSERT_TRUE(renderRawTestPlane(read, window, &plane, &error)) << error;
        ASSERT_EQ((std::size_t)fixture.channels, plane.channels.size());
        const std::size_t nComps = (std::size_t)fixture.channels;
        std::vector<float> expected((std::size_t)window.area() * nComps);
        ASSERT_TRUE(RawDecodeSupport::decode(fixture.path(), window, expected.data(), (std::size_t)window.width() * nComps, &error)) << error;
        std::size_t mismatches = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                for (std::size_t c = 0; c < nComps; ++c) {
                    const float want = expected[((std::size_t)(y - window.y1) * (std::size_t)window.width() + (std::size_t)(x - window.x1)) * nComps + c];
                    if (rawTestPlaneValue(plane, x, y, (int)c) != want) {
                        ++mismatches;
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches);
        EXPECT_FALSE(read->hasPersistentMessage()) << persistentMessage(read);
    }
}

TEST_F(NativeReadRawTest, ColourKnobsAreHiddenAndProxyDisabledForARawFile)
{
    NodePtr read = createRead(std::string(NATRON_TESTS_FIXTURES_DIR "/png-8bit.png"));
    ASSERT_TRUE(bool(read));
    EXPECT_FALSE(isSecret(read, kNatronReadNodeOCIOParamInputSpace "Index"));
    EXPECT_FALSE(isSecret(read, "Context"));
    KnobIPtr proxy = read->getKnobByName(kOfxImageEffectProxyParamName);
    ASSERT_TRUE(bool(proxy));
    EXPECT_TRUE(proxy->isEnabled(0));

    KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(bayerFixture().path());
    EXPECT_TRUE(isSecret(read, kNatronReadNodeOCIOParamInputSpace "Index"));
    EXPECT_TRUE(isSecret(read, kNatronReadNodeOCIOParamInputSpace));
    EXPECT_TRUE(isSecret(read, "Context"));
    EXPECT_FALSE(proxy->isEnabled(0));
    EXPECT_NE(std::string::npos, proxy->getHintToolTip().find("raw"));

    file->setValue(std::string(NATRON_TESTS_FIXTURES_DIR "/png-8bit.png"));
    EXPECT_FALSE(isSecret(read, kNatronReadNodeOCIOParamInputSpace "Index"));
    EXPECT_TRUE(proxy->isEnabled(0));
}

TEST_F(NativeReadRawTest, NonRawFileStillOutputsAnImage)
{
    NodePtr read = createRead(std::string(NATRON_TESTS_FIXTURES_DIR "/png-8bit.png"));
    ASSERT_TRUE(bool(read));
    EXPECT_EQ(eDataKindImage, read->getEffectiveOutputDataKind());
    EXPECT_FALSE(outputMetadata(read).contains(kRawMetadataStage));

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(read, RectI(0, 0, 4, 4), &plane, &error)) << error;
    EXPECT_GE(plane.channels.size(), 3u);
}

TEST_F(NativeReadRawTest, UnsupportedCameraSetsTheNamedPersistentError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::vector<char> bytes = readFixtureBytes(bayerFixture().path());
    const std::string real = "Canon EOS 5D Mark III";
    const std::string stub = "Canon EOS 5D Mark IX";
    for (std::vector<char>::iterator it = bytes.begin(); (it = std::search(it, bytes.end(), real.begin(), real.end())) != bytes.end(); it += (std::ptrdiff_t)real.size()) {
        std::copy(stub.begin(), stub.end(), it);
    }
    const std::string path = dir.path().toStdString() + "/unsupported.cr2";
    ASSERT_TRUE(writeFixtureBytes(path, bytes.data(), bytes.size()));

    NodePtr read = createRead(path);
    ASSERT_TRUE(bool(read));
    EXPECT_EQ(eDataKindRaw, read->getEffectiveOutputDataKind());

    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderRawTestPlane(read, RectI(0, 0, 8, 8), &plane, &error));
    ASSERT_TRUE(read->hasPersistentMessage());
    const std::string message = persistentMessage(read);
    std::string expected;
    EXPECT_FALSE(bool(RawDecodeSupport::readHeader(path, &expected)));
    EXPECT_EQ(expected, message);
    EXPECT_NE(std::string::npos, message.find("Canon EOS 5D Mark IX")) << message;
    EXPECT_NE(std::string::npos, message.find(RawspeedRuntime::version())) << message;
}

TEST_F(NativeReadRawTest, RawSequenceFollowsTheTimeDomain)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::error_code ec;
    for (int frame = 1; frame <= 2; ++frame) {
        const std::string link = dir.path().toStdString() + "/shot.000" + std::to_string(frame) + ".cr2";
        std::filesystem::create_symlink(bayerFixture().path(), link, ec);
        ASSERT_FALSE(ec) << ec.message();
    }
    NodePtr read = createRead(dir.path().toStdString() + "/shot.####.cr2");
    ASSERT_TRUE(bool(read));
    EXPECT_EQ(eDataKindRaw, read->getEffectiveOutputDataKind());

    double first = 0.;
    double last = 0.;
    read->getEffectInstance()->getFrameRange_public(read->getHashValue(), &first, &last, true);
    EXPECT_EQ(1., first);
    EXPECT_EQ(2., last);

    const ImageMetadata second = outputMetadata(read, 2.);
    EXPECT_EQ(std::optional<int>(2), second.getInt(kOfxMetadataKeySourceFrame));
    EXPECT_EQ(std::optional<RawStageEnum>(eRawStageRaw), RawMetadata::getStage(second));
    EXPECT_TRUE(RawLayout::fromMetadata(second).has_value());
}
