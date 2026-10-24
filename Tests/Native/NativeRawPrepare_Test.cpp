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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 20;
const int kHeight = 14;

float
mosaicValue(int x,
            int y,
            int c)
{
    return 1000.f + 7.f * (float)x + 13.f * (float)y + 100.f * (float)c;
}

// The cell of the 2x2 black level block at the CFA origin, as raw/black_levels orders it.
int
cellOf(const RawLayout& layout,
       int x,
       int y)
{
    return (((y - layout.originY()) & 1) * 2) + ((x - layout.originX()) & 1);
}

float
prepared(float value,
         double black,
         double white)
{
    return (value - (float)black) / ((float)white - (float)black);
}

ImageMetadata
mosaicMetadata(const std::string& pattern,
               int originX,
               int originY,
               RawStageEnum stage)
{
    ImageMetadata metadata = rawTestMetadata(pattern, originX, originY, stage);

    RawMetadata::setBlackLevels(&metadata, { 100., 200., 300., 400. });
    RawMetadata::setWhiteLevel(&metadata, 4000.);

    return metadata;
}
} // namespace

class NativeRawPrepareTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createSource(const RectI& rod,
                         int nComps,
                         const ImageMetadata& metadata)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));

        EXPECT_TRUE(bool(source));
        if (source) {
            dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, nComps, mosaicValue, metadata);
        }

        return source;
    }

    NodePtr createPrepare(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWPREPARE));

        EXPECT_TRUE(bool(node));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    static ImageMetadata outputMetadata(const NodePtr& node)
    {
        NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());

        return effect ? effect->getOutputMetadata(1., ViewIdx(0)) : ImageMetadata();
    }

    static RectD regionOfDefinition(const NodePtr& node)
    {
        RectD rod;
        bool isProjectFormat = false;
        EffectInstancePtr effect = node->getEffectInstance();
        EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), 1., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));

        return rod;
    }
};

TEST_F(NativeRawPrepareTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWPREPARE, PLUGIN_MAJOR_NATRON_RAWPREPARE, "RawPrepare");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWPREPARE));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getOutputDataKind());
}

// Every photosite loses the black level of its own cell of the 2x2 block and is scaled so the
// white level is 1, whatever the CFA origin.
TEST_F(NativeRawPrepareTest, SubtractsTheBlackOfEachCellAndScalesByWhite)
{
    for (const std::pair<int, int>& origin : { std::make_pair(0, 0), std::make_pair(1, 0), std::make_pair(-1, 3) }) {
        const ImageMetadata metadata = mosaicMetadata("GRBG", origin.first, origin.second, eRawStageRaw);
        const RawLayout layout = *RawLayout::fromMetadata(metadata);
        NodePtr source = createSource(RectI(0, 0, kWidth, kHeight), 1, metadata);
        NodePtr node = createPrepare(source);
        ASSERT_TRUE(source && node);

        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        const double blacks[4] = { 100., 200., 300., 400. };
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                ASSERT_EQ(prepared(mosaicValue(x, y, 0), blacks[cellOf(layout, x, y)], 4000.), rawTestPlaneValue(plane, x, y, 0)) << x << "," << y;
            }
        }

        const ImageMetadata out = outputMetadata(node);
        EXPECT_EQ(eRawStagePrepared, RawMetadata::getStage(out).value_or(eRawStageRaw));
        EXPECT_EQ(1., RawMetadata::getWhiteLevel(out).value_or(0.));
        EXPECT_EQ(std::vector<double>(4, 0.), RawMetadata::getBlackLevels(out).value_or(std::vector<double>()));
        EXPECT_TRUE(RawLayout::fromMetadata(out).value_or(RawLayout()) == layout);
    }
}

TEST_F(NativeRawPrepareTest, OverridesReplaceTheFileLevels)
{
    NodePtr source = createSource(RectI(0, 0, kWidth, kHeight), 1, mosaicMetadata("RGGB", 0, 0, eRawStageRaw));
    NodePtr node = createPrepare(source);
    ASSERT_TRUE(source && node);

    KnobBoolPtr blackOverride = node->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawPrepareParamBlackOverride);
    KnobDoublePtr blackLevels = node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawPrepareParamBlackLevels);
    KnobBoolPtr whiteOverride = node->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawPrepareParamWhiteOverride);
    KnobDoublePtr whiteLevel = node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawPrepareParamWhiteLevel);
    ASSERT_TRUE(blackOverride && blackLevels && whiteOverride && whiteLevel);
    blackOverride->setValue(true);
    for (int i = 0; i < 4; ++i) {
        blackLevels->setValue(50. * (i + 1), ViewSpec::all(), i);
    }
    whiteOverride->setValue(true);
    whiteLevel->setValue(2000.);

    const RawLayout layout = *RawLayout::fromPattern("RGGB");
    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const double black = 50. * (cellOf(layout, x, y) + 1);
            ASSERT_EQ(prepared(mosaicValue(x, y, 0), black, 2000.), rawTestPlaneValue(plane, x, y, 0)) << x << "," << y;
        }
    }
}

// An odd crop moves the kept area to the origin, and the CFA origin with it, so every photosite
// keeps its colour.
TEST_F(NativeRawPrepareTest, AnOddCropKeepsTheColourOfEveryPhotosite)
{
    ImageMetadata metadata = mosaicMetadata("RGGB", 0, 0, eRawStageRaw);
    const RectI crop(1, 3, kWidth - 2, kHeight - 1);
    RawMetadata::setCrop(&metadata, crop);
    const RawLayout input = *RawLayout::fromMetadata(metadata);
    NodePtr source = createSource(RectI(0, 0, kWidth, kHeight), 1, metadata);
    NodePtr node = createPrepare(source);
    ASSERT_TRUE(source && node);

    const RectD rod = regionOfDefinition(node);
    EXPECT_EQ(RectD(0., 0., crop.width(), crop.height()), rod);

    const ImageMetadata out = outputMetadata(node);
    const std::optional<RawLayout> output = RawLayout::fromMetadata(out);
    ASSERT_TRUE(bool(output));
    const RectI window(0, 0, crop.width(), crop.height());
    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << error;
    const double blacks[4] = { 100., 200., 300., 400. };
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const int sx = x + crop.x1;
            const int sy = y + crop.y1;
            EXPECT_EQ(input.colourAt(sx, sy), output->colourAt(x, y)) << x << "," << y;
            ASSERT_EQ(prepared(mosaicValue(sx, sy, 0), blacks[cellOf(input, sx, sy)], 4000.), rawTestPlaneValue(plane, x, y, 0)) << x << "," << y;
        }
    }
    EXPECT_EQ(RectI(0, 0, crop.width(), crop.height()), RawMetadata::getCrop(out).value_or(RectI()));

    // The same crop through the override knobs.
    NodePtr plain = createSource(RectI(0, 0, kWidth, kHeight), 1, mosaicMetadata("RGGB", 0, 0, eRawStageRaw));
    NodePtr overridden = createPrepare(plain);
    ASSERT_TRUE(plain && overridden);
    EffectInstancePtr effect = overridden->getEffectInstance();
    effect->getKnobByNameAndType<KnobBool>(kRawPrepareParamCropOverride)->setValue(true);
    effect->getKnobByNameAndType<KnobInt>(kRawPrepareParamCropLeft)->setValue(crop.x1);
    effect->getKnobByNameAndType<KnobInt>(kRawPrepareParamCropBottom)->setValue(crop.y1);
    effect->getKnobByNameAndType<KnobInt>(kRawPrepareParamCropRight)->setValue(kWidth - crop.x2);
    effect->getKnobByNameAndType<KnobInt>(kRawPrepareParamCropTop)->setValue(kHeight - crop.y2);
    EXPECT_EQ(rod, regionOfDefinition(overridden));
    EXPECT_TRUE(RawLayout::fromMetadata(outputMetadata(overridden)).value_or(RawLayout()) == *output);
    RenderedPlane overriddenPlane;
    ASSERT_TRUE(renderRawTestPlane(overridden, window, &overriddenPlane, &error)) << error;
    EXPECT_EQ(plane.pixels, overriddenPlane.pixels);
}

TEST_F(NativeRawPrepareTest, LinearDataIsScaledPerChannel)
{
    ImageMetadata metadata = rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStageRaw);
    RawMetadata::setBlackLevels(&metadata, { 10., 20., 30. });
    RawMetadata::setWhiteLevel(&metadata, 3000.);
    NodePtr source = createSource(RectI(0, 0, kWidth, kHeight), 3, metadata);
    NodePtr node = createPrepare(source);
    ASSERT_TRUE(source && node);

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    ASSERT_EQ(3u, plane.channels.size());
    const double blacks[3] = { 10., 20., 30. };
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_EQ(prepared(mosaicValue(x, y, c), blacks[c], 3000.), rawTestPlaneValue(plane, x, y, c)) << x << "," << y << " channel " << c;
            }
        }
    }
}

TEST_F(NativeRawPrepareTest, PreparedDataPassesThroughWithAWarning)
{
    const ImageMetadata metadata = mosaicMetadata("BGGR", 0, 0, eRawStagePrepared);
    NodePtr source = createSource(RectI(0, 0, kWidth, kHeight), 1, metadata);
    NodePtr node = createPrepare(source);
    ASSERT_TRUE(source && node);

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            ASSERT_EQ(mosaicValue(x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << x << "," << y;
        }
    }
    EXPECT_EQ(metadata, outputMetadata(node));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);

    dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setMetadata(mosaicMetadata("BGGR", 0, 0, eRawStageRaw));
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    EXPECT_FALSE(node->hasPersistentMessage());
}

// A source that does not say which raw stage it is at is not raw data: nothing renders, and the
// node says why.
TEST_F(NativeRawPrepareTest, ASourceWithoutAStageRaisesTheStageError)
{
    ImageMetadata metadata = mosaicMetadata("RGGB", 0, 0, eRawStageRaw);
    metadata.remove(kRawMetadataStage);
    NodePtr source = createSource(RectI(0, 0, kWidth, kHeight), 1, metadata);
    NodePtr node = createPrepare(source);
    ASSERT_TRUE(source && node);

    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_EQ(QString::fromUtf8(RawMetadata::requireStage(metadata, eRawStageRaw, node->getLabel()).value_or(std::string()).c_str()), message);

    dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setMetadata(mosaicMetadata("RGGB", 0, 0, eRawStageRaw));
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    EXPECT_FALSE(node->hasPersistentMessage());
}
