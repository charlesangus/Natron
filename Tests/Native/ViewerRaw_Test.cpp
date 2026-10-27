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

#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "DataKindTestEffect.h"
#include "RawFixtures.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/RawDecodeSupport.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawDisplay.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_USING

class ViewerRawTest
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

        if (read) {
            KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());
            EXPECT_TRUE(file != NULL);
            if (file) {
                file->setValue(path);
            }
        }

        return read;
    }

    // What the Viewer shows of a raw input over window: the input's own plane, as the Viewer asks
    // for it, through RawDisplay, scaled as the Viewer scales it.
    ImagePtr displayed(const NodePtr& input,
                       const RectI& window,
                       std::string* error)
    {
        EffectInstancePtr effect = input->getEffectInstance();
        ImageLayerDesc components, paired;
        EffectInstance::getLayersFromMetadata(*effect->getTimeInvariantMetadata(), -1, &components, &paired);
        std::vector<RenderedPlane> planes;
        if (!renderNodePlanesDirect(input, 0., ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, components), &planes, error) || (planes.size() != 1)) {
            return ImagePtr();
        }
        const RenderedPlane& plane = planes[0];
        const int nComps = (int)plane.channels.size();
        ImagePtr raw = std::make_shared<Image>(components, RectD(window.x1, window.y1, window.x2, window.y2), window, 0, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false);
        {
            Image::WriteAccess access(raw.get());
            for (int y = window.y1; y < window.y2; ++y) {
                float* row = (float*)access.pixelAt(window.x1, y);
                std::memcpy(row, plane.pixels.data() + (std::size_t)(y - window.y1) * (std::size_t)window.width() * (std::size_t)nComps, (std::size_t)window.width() * (std::size_t)nComps * sizeof(float));
            }
        }

        NativeEffectBase* native = dynamic_cast<NativeEffectBase*>(effect.get());
        const std::optional<RawDisplay::Normalisation> normalisation = native ? RawDisplay::normalisationOf(native->getOutputMetadata(0., ViewIdx(0))) : std::nullopt;
        EXPECT_TRUE(normalisation.has_value());

        return RawDisplay::toDisplayImage(raw, normalisation ? &*normalisation : nullptr);
    }

    // The value the Viewer shows for the sensor value v of channel c at (x, y).
    static float expectedShown(const ImageMetadata& metadata,
                               int x,
                               int y,
                               int c,
                               float v)
    {
        const std::optional<RawDisplay::Normalisation> n = RawDisplay::normalisationOf(metadata);
        EXPECT_TRUE(n.has_value());
        if (!n) {
            return v;
        }
        const double black = n->blackAt(x, y, c);

        return (float)((v - black) / (n->white - black));
    }
};

TEST_F(ViewerRawTest, ViewerAcceptsARawReadAndOtherImageInputsDoNot)
{
    NodePtr read = createRead(bayerFixture().path());
    NodePtr viewer = createNode(QString::fromUtf8(PLUGINID_NATRON_VIEWER));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));
    NodePtr adapterSink = createNode(QString::fromUtf8(kTestPluginIDDataKindRawAdapterSink));

    ASSERT_TRUE(read && viewer && imageSink && adapterSink);
    ASSERT_TRUE(dynamic_cast<ViewerInstance*>(viewer->getEffectInstance().get()));
    EXPECT_EQ(eDataKindRaw, read->getEffectiveOutputDataKind());

    EXPECT_EQ(Node::eCanConnectInput_ok, viewer->canConnectInput(read, 0));
    connectNodes(read, viewer, 0, true);
    EXPECT_FALSE(viewer->isInputDataKindUnacceptable(read, 0));
    EXPECT_FALSE(viewer->hasPersistentMessage());

    // An image input whose effect asks for raw through an adapter still cannot take it: only the
    // Viewer can.
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, imageSink->canConnectInput(read, 0));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, adapterSink->canConnectInput(read, 0));

    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(dot));
    connectNodes(read, dot, 0, true);
    // The first viewer's input is taken by the Read, which canConnectInput() reports before it
    // looks at data kinds, so the pass-through goes to a viewer with a free input.
    NodePtr freeViewer = createNode(QString::fromUtf8(PLUGINID_NATRON_VIEWER));
    ASSERT_TRUE(bool(freeViewer));
    EXPECT_EQ(eDataKindRaw, dot->getEffectiveOutputDataKind());
    EXPECT_EQ(Node::eCanConnectInput_ok, freeViewer->canConnectInput(dot, 0));
}

TEST_F(ViewerRawTest, MosaicDisplaysAsGreyWithTheMosaicValues)
{
    for (const RawFixture* fixture : { &bayerFixture(), &xtransFixture() }) {
        SCOPED_TRACE(fixture->file);
        NodePtr read = createRead(fixture->path());
        ASSERT_TRUE(bool(read));
        std::string error;
        const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(fixture->path(), &error);
        ASSERT_TRUE(bool(header)) << error;

        NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(read->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL);
        const RawDisplay::ModeEnum mode = RawDisplay::modeOf(effect->getOutputMetadata(0., ViewIdx(0)), EffectInstance::getNCompsFromMetadata(*effect->getTimeInvariantMetadata(), -1));
        EXPECT_EQ(RawDisplay::eModeMosaic, mode);
        EXPECT_EQ(std::string("RAW mosaic"), RawDisplay::label(mode));

        const RectI window(header->window.x1 + 11, header->window.y1 + 5, header->window.x1 + 11 + 32, header->window.y1 + 5 + 24);
        const ImagePtr shown = displayed(read, window, &error);
        ASSERT_TRUE(bool(shown)) << error;
        ASSERT_EQ(4u, shown->getComponentsCount());

        std::vector<float> mosaic((std::size_t)window.area());
        ASSERT_TRUE(RawDecodeSupport::decode(fixture->path(), window, mosaic.data(), (std::size_t)window.width(), &error)) << error;
        const ImageMetadata metadata = effect->getOutputMetadata(0., ViewIdx(0));
        Image::ReadAccess access(shown.get());
        std::size_t mismatches = 0;
        bool varies = false;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const float* rgba = (const float*)access.pixelAt(x, y);
                const float raw = mosaic[(std::size_t)(y - window.y1) * (std::size_t)window.width() + (std::size_t)(x - window.x1)];
                const float value = expectedShown(metadata, x, y, 0, raw);
                varies = varies || (value != raw);
                if ((rgba[0] != value) || (rgba[1] != value) || (rgba[2] != value) || (rgba[3] != 1.f)) {
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(0u, mismatches);
        EXPECT_TRUE(varies) << "the display differs from the sensor values";
        EXPECT_FALSE(read->hasPersistentMessage());
    }
}

TEST_F(ViewerRawTest, LinearDngDisplaysItsThreeChannels)
{
    NodePtr read = createRead(linearFixture().path());
    ASSERT_TRUE(bool(read));
    std::string error;
    const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(linearFixture().path(), &error);
    ASSERT_TRUE(bool(header)) << error;
    ASSERT_EQ(3, header->channels);

    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(read->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    EXPECT_EQ(3, EffectInstance::getNCompsFromMetadata(*effect->getTimeInvariantMetadata(), -1));
    const RawDisplay::ModeEnum mode = RawDisplay::modeOf(effect->getOutputMetadata(0., ViewIdx(0)), 3);
    EXPECT_EQ(RawDisplay::eModeLinear, mode);
    EXPECT_EQ(std::string("RAW linear"), RawDisplay::label(mode));

    const RectI window(header->window.x1 + 100, header->window.y1 + 100, header->window.x1 + 132, header->window.y1 + 120);
    const ImagePtr shown = displayed(read, window, &error);
    ASSERT_TRUE(bool(shown)) << error;

    std::vector<float> linear((std::size_t)window.area() * 3);
    ASSERT_TRUE(RawDecodeSupport::decode(linearFixture().path(), window, linear.data(), (std::size_t)window.width() * 3, &error)) << error;
    Image::ReadAccess access(shown.get());
    std::size_t mismatches = 0;
    bool channelsDiffer = false;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* rgba = (const float*)access.pixelAt(x, y);
            const float* want = linear.data() + ((std::size_t)(y - window.y1) * (std::size_t)window.width() + (std::size_t)(x - window.x1)) * 3;
            const ImageMetadata metadata = effect->getOutputMetadata(0., ViewIdx(0));
            if ((rgba[0] != expectedShown(metadata, x, y, 0, want[0])) || (rgba[1] != expectedShown(metadata, x, y, 1, want[1])) || (rgba[2] != expectedShown(metadata, x, y, 2, want[2])) || (rgba[3] != 1.f)) {
                ++mismatches;
            }
            channelsDiffer = channelsDiffer || (want[0] != want[1]) || (want[1] != want[2]);
        }
    }
    EXPECT_EQ(0u, mismatches);
    EXPECT_TRUE(channelsDiffer) << "the three channels are shown, not one of them as grey";
}

TEST_F(ViewerRawTest, NormalisationScalesARawStageMosaicByTheBlackOfEachColour)
{
    ImageMetadata metadata;
    RawLayout::fromPattern("RGGB", 0, 0)->writeTo(&metadata);
    RawMetadata::setBlackLevels(&metadata, { 10., 20., 30., 40. });
    RawMetadata::setWhiteLevel(&metadata, 110.);
    RawMetadata::setStage(&metadata, eRawStageRaw);
    const std::optional<RawDisplay::Normalisation> n = RawDisplay::normalisationOf(metadata);
    ASSERT_TRUE(n.has_value());

    const RectI bounds(0, 0, 4, 4);
    ImagePtr src = std::make_shared<Image>(ImageLayerDesc::getAlphaComponents(), RectD(0, 0, 4, 4), bounds, 0, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false);
    {
        Image::WriteAccess access(src.get());
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                *(float*)access.pixelAt(x, y) = 60.f;
            }
        }
    }
    const ImagePtr shown = RawDisplay::toDisplayImage(src, &*n);
    const ImagePtr kept = RawDisplay::toDisplayImage(src);
    Image::ReadAccess shownAccess(shown.get());
    Image::ReadAccess keptAccess(kept.get());
    const float blacks[4] = { 10.f, 20.f, 30.f, 40.f };
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const float black = blacks[(y & 1) * 2 + (x & 1)];
            const float* rgba = (const float*)shownAccess.pixelAt(x, y);
            EXPECT_NEAR((60.f - black) / (110.f - black), rgba[0], 1e-6f);
            EXPECT_EQ(rgba[0], rgba[1]);
            EXPECT_EQ(rgba[0], rgba[2]);
            EXPECT_EQ(60.f, ((const float*)keptAccess.pixelAt(x, y))[0]);
        }
    }
}

TEST_F(ViewerRawTest, NormalisationPreparedStreamsAndMissingMetadataAreNotScaled)
{
    ImageMetadata metadata;
    RawMetadata::setBlackLevels(&metadata, { 10. });
    RawMetadata::setWhiteLevel(&metadata, 110.);
    EXPECT_FALSE(RawDisplay::normalisationOf(metadata).has_value());

    RawMetadata::setStage(&metadata, eRawStagePrepared);
    EXPECT_FALSE(RawDisplay::normalisationOf(metadata).has_value());

    ImageMetadata noWhite;
    RawMetadata::setStage(&noWhite, eRawStageRaw);
    EXPECT_FALSE(RawDisplay::normalisationOf(noWhite).has_value());
}
