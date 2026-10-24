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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "DataKindTestEffect.h"
#include "NativePluginIdentity.h"
#include "RawFixtures.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/RawDecodeSupport.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawToImage.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

class NativeRawToImageTest
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

    NodePtr createConverterOn(const std::string& path)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWTOIMAGE));

        EXPECT_TRUE(read && node);
        if (!read || !node) {
            return NodePtr();
        }
        KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());
        EXPECT_TRUE(file != NULL);
        if (file) {
            file->setValue(path);
        }
        connectNodes(read, node, 0, true);

        return node;
    }

    static std::string persistentMessage(const NodePtr& node)
    {
        QString message;
        int type = 0;

        node->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }
};

TEST_F(NativeRawToImageTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWTOIMAGE, PLUGIN_MAJOR_NATRON_RAWTOIMAGE, "RawToImage");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWTOIMAGE));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindImage, node->getEffectInstance()->getOutputDataKind());
    EXPECT_EQ(eDataKindImage, node->getEffectiveOutputDataKind());
}

TEST_F(NativeRawToImageTest, OutputIsAnImageAnImageInputAccepts)
{
    NodePtr node = createConverterOn(bayerFixture().path());
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(node && imageSink);
    EXPECT_EQ(Node::eCanConnectInput_ok, imageSink->canConnectInput(node, 0));
    EXPECT_EQ(4, node->getEffectInstance()->getMetadataNComps(-1));
}

TEST_F(NativeRawToImageTest, MosaicBecomesGreyWithTheInputValues)
{
    for (const RawFixture* fixture : { &bayerFixture(), &xtransFixture() }) {
        SCOPED_TRACE(fixture->file);
        NodePtr node = createConverterOn(fixture->path());
        ASSERT_TRUE(bool(node));
        std::string error;
        const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(fixture->path(), &error);
        ASSERT_TRUE(bool(header)) << error;

        const RectI window(header->window.x1 + 7, header->window.y1 + 9, header->window.x1 + 7 + 40, header->window.y1 + 9 + 30);
        RenderedPlane plane;
        ASSERT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << error;
        ASSERT_EQ(4u, plane.channels.size());

        std::vector<float> mosaic((std::size_t)window.area());
        ASSERT_TRUE(RawDecodeSupport::decode(fixture->path(), window, mosaic.data(), (std::size_t)window.width(), &error)) << error;
        std::size_t mismatches = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const float value = mosaic[(std::size_t)(y - window.y1) * (std::size_t)window.width() + (std::size_t)(x - window.x1)];
                for (int c = 0; c < 3; ++c) {
                    if (rawTestPlaneValue(plane, x, y, c) != value) {
                        ++mismatches;
                    }
                }
                if (rawTestPlaneValue(plane, x, y, 3) != 1.f) {
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(0u, mismatches);
        EXPECT_FALSE(node->hasPersistentMessage()) << persistentMessage(node);

        NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL);
        const ImageMetadata metadata = effect->getOutputMetadata(0., ViewIdx(0));
        EXPECT_FALSE(metadata.contains(kRawMetadataCfaPattern));
        EXPECT_FALSE(metadata.contains(kRawMetadataStage));
        EXPECT_EQ(RawMetadata::getWhiteLevel(header->metadata), RawMetadata::getWhiteLevel(metadata));
    }
}

TEST_F(NativeRawToImageTest, LinearKeepsItsChannels)
{
    NodePtr node = createConverterOn(linearFixture().path());
    ASSERT_TRUE(bool(node));
    std::string error;
    const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(linearFixture().path(), &error);
    ASSERT_TRUE(bool(header)) << error;
    ASSERT_EQ(3, header->channels);

    const RectI window(header->window.x1 + 200, header->window.y1 + 150, header->window.x1 + 232, header->window.y1 + 174);
    RenderedPlane plane;
    ASSERT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << error;
    ASSERT_EQ(4u, plane.channels.size());

    std::vector<float> linear((std::size_t)window.area() * 3);
    ASSERT_TRUE(RawDecodeSupport::decode(linearFixture().path(), window, linear.data(), (std::size_t)window.width() * 3, &error)) << error;
    std::size_t mismatches = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* want = linear.data() + ((std::size_t)(y - window.y1) * (std::size_t)window.width() + (std::size_t)(x - window.x1)) * 3;
            for (int c = 0; c < 3; ++c) {
                if (rawTestPlaneValue(plane, x, y, c) != want[c]) {
                    ++mismatches;
                }
            }
            if (rawTestPlaneValue(plane, x, y, 3) != 1.f) {
                ++mismatches;
            }
        }
    }
    EXPECT_EQ(0u, mismatches);
    EXPECT_FALSE(node->hasPersistentMessage()) << persistentMessage(node);
}
