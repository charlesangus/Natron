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
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawDngOpcodes.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 20;
const int kHeight = 14;
const int kMapW = 4;
const int kMapH = 3;
const double kSpacingH = 0.3;
const double kSpacingV = 0.45;
const double kOriginH = 0.05;
const double kOriginV = 0.1;
const double kBlacks[4] = { 100., 200., 300., 400. };
const double kWhite = 4000.;

float
mosaicValue(int x,
            int y,
            int /*c*/)
{
    return 1000.f + 7.f * (float)x + 13.f * (float)y;
}

double
mapGain(int cell,
        int row,
        int col)
{
    return 0.8 + 0.1 * cell + 0.07 * row + 0.03 * col;
}

// The maps are listed in an order other than the cell order, with their top and left carrying the cell.
std::vector<DngGainMap>
syntheticMaps()
{
    std::vector<DngGainMap> maps;

    for (const int cell : { 2, 0, 3, 1 }) {
        DngGainMap g;
        g.top = (uint32_t)(cell >> 1);
        g.left = (uint32_t)(cell & 1);
        g.bottom = kHeight;
        g.right = kWidth;
        g.plane = 0;
        g.planes = 1;
        g.rowPitch = 2;
        g.colPitch = 2;
        g.mapPointsV = kMapH;
        g.mapPointsH = kMapW;
        g.mapSpacingV = kSpacingV;
        g.mapSpacingH = kSpacingH;
        g.mapOriginV = kOriginV;
        g.mapOriginH = kOriginH;
        g.mapPlanes = 1;
        for (int r = 0; r < kMapH; ++r) {
            for (int c = 0; c < kMapW; ++c) {
                g.gains.push_back((float)mapGain(cell, r, c));
            }
        }
        maps.push_back(g);
    }

    return maps;
}

// The gain of the sensor photosite at (col, row), counted from the sensor's top-left corner.
double
expectedGain(int col,
             int row)
{
    const int cell = ((row & 1) << 1) + (col & 1);
    const double u = std::min(std::max(((double)col / kWidth - kOriginH) / kSpacingH, 0.), (double)kMapW);
    const double v = std::min(std::max(((double)row / kHeight - kOriginV) / kSpacingV, 0.), (double)kMapH);
    const int u0 = std::min((int)u, kMapW - 1);
    const int u1 = std::min(u0 + 1, kMapW - 1);
    const int v0 = std::min((int)v, kMapH - 1);
    const int v1 = std::min(v0 + 1, kMapH - 1);
    const double fu = u - u0;
    const double fv = v - v0;
    const double top = (1. - fu) * mapGain(cell, v0, u0) + fu * mapGain(cell, v0, u1);
    const double bottom = (1. - fu) * mapGain(cell, v1, u0) + fu * mapGain(cell, v1, u1);

    return (1. - fv) * top + fv * bottom;
}

ImageMetadata
metadataWith(const std::vector<DngGainMap>& maps)
{
    ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStageRaw);

    RawMetadata::setBlackLevels(&metadata, { kBlacks[0], kBlacks[1], kBlacks[2], kBlacks[3] });
    RawMetadata::setWhiteLevel(&metadata, kWhite);
    if (!maps.empty()) {
        RawMetadata::setGainMaps(&metadata, maps);
    }

    return metadata;
}

double
preparedValue(int x,
              int y)
{
    const double black = kBlacks[(((y) & 1) * 2) + (x & 1)];

    return ((double)mosaicValue(x, y, 0) - black) / (kWhite - black);
}
} // namespace

class NativeRawPrepareFlatFieldTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createPrepare(const ImageMetadata& metadata)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWPREPARE));

        EXPECT_TRUE(source && node);
        if (source && node) {
            dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 1, mosaicValue, metadata);
            connectNodes(source, node, 0, true);
        }
        _source = source;

        return node;
    }

    static void setFlatField(const NodePtr& node,
                             RawPrepareFlatFieldEnum mode)
    {
        KnobChoicePtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawPrepareParamFlatField);

        ASSERT_TRUE(bool(knob));
        knob->setValue((int)mode);
    }

    static bool flatFieldActive(const NodePtr& node)
    {
        RawPrepare* effect = dynamic_cast<RawPrepare*>(node->getEffectInstance().get());
        const std::optional<RawPrepare::Settings> settings = effect->resolveSettings(1., ViewIdx(0));

        return settings && settings->flatField;
    }

    static bool canRenderOnGpu(const NodePtr& node)
    {
#ifdef NATRON_HAVE_GPU
        StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
        GpuQuery q;

        q.time = 1.;
        q.view = ViewIdx(0);
        q.nComps = 1;
        q.bitDepth = eImageBitDepthFloat;
        for (int i = 0; i < 4; ++i) {
            q.processChannels[i] = true;
        }

        return effect->canRenderOnGpu(q);
#else
        Q_UNUSED(node);

        return false;
#endif
    }

    NodePtr _source;
};

TEST_F(NativeRawPrepareFlatFieldTest, DefaultsToEmbeddedGainMap)
{
    NodePtr node = createPrepare(metadataWith(syntheticMaps()));
    ASSERT_TRUE(bool(node));
    KnobChoicePtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawPrepareParamFlatField);
    ASSERT_TRUE(bool(knob));
    EXPECT_EQ((int)eRawPrepareFlatFieldEmbedded, knob->getValue());
}

TEST_F(NativeRawPrepareFlatFieldTest, MultipliesEachPhotositeByItsBilinearGain)
{
    NodePtr node = createPrepare(metadataWith(syntheticMaps()));
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(flatFieldActive(node));

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const double expected = preparedValue(x, y) * expectedGain(x, kHeight - 1 - y);
            ASSERT_NEAR(expected, rawTestPlaneValue(plane, x, y, 0), 1e-5) << x << "," << y;
        }
    }
}

TEST_F(NativeRawPrepareFlatFieldTest, DisabledEqualsNoMaps)
{
    NodePtr withMaps = createPrepare(metadataWith(syntheticMaps()));
    ASSERT_TRUE(bool(withMaps));
    setFlatField(withMaps, eRawPrepareFlatFieldDisabled);
    EXPECT_FALSE(flatFieldActive(withMaps));
    RenderedPlane disabled;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(withMaps, RectI(0, 0, kWidth, kHeight), &disabled, &error)) << error;

    NodePtr without = createPrepare(metadataWith({}));
    ASSERT_TRUE(bool(without));
    RenderedPlane plain;
    ASSERT_TRUE(renderRawTestPlane(without, RectI(0, 0, kWidth, kHeight), &plain, &error)) << error;
    EXPECT_EQ(plain.pixels, disabled.pixels);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            ASSERT_NEAR(preparedValue(x, y), rawTestPlaneValue(plain, x, y, 0), 1e-6) << x << "," << y;
        }
    }
}

TEST_F(NativeRawPrepareFlatFieldTest, AFileWithoutMapsIsUnchangedWithTheKnobOn)
{
    NodePtr node = createPrepare(metadataWith({}));
    ASSERT_TRUE(bool(node));
    setFlatField(node, eRawPrepareFlatFieldEmbedded);
    EXPECT_FALSE(flatFieldActive(node));

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            ASSERT_NEAR(preparedValue(x, y), rawTestPlaneValue(plane, x, y, 0), 1e-6) << x << "," << y;
        }
    }
}

TEST_F(NativeRawPrepareFlatFieldTest, MapsThatDoNotFitAreIgnored)
{
    std::vector<DngGainMap> three = syntheticMaps();
    three.pop_back();
    std::vector<DngGainMap> offGrid = syntheticMaps();
    offGrid[2].mapOriginH += 0.01;
    std::vector<DngGainMap> partial = syntheticMaps();
    partial[1].bottom = kHeight - 2;

    for (const std::vector<DngGainMap>& maps : { three, offGrid, partial }) {
        NodePtr node = createPrepare(metadataWith(maps));
        ASSERT_TRUE(bool(node));
        EXPECT_FALSE(flatFieldActive(node));
    }
}

// The maps follow the sensor, not the kept area: cropping by an odd count of photosites moves
// the output window but not the gain of any photosite.
TEST_F(NativeRawPrepareFlatFieldTest, AnOddCropKeepsEachPhotositesGain)
{
    ImageMetadata metadata = metadataWith(syntheticMaps());
    const RectI crop(1, 3, kWidth - 2, kHeight - 1);
    RawMetadata::setCrop(&metadata, crop);
    NodePtr node = createPrepare(metadata);
    ASSERT_TRUE(bool(node));

    const RectI window(0, 0, crop.width(), crop.height());
    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << error;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const int sx = x + crop.x1;
            const int sy = y + crop.y1;
            const double expected = preparedValue(sx, sy) * expectedGain(sx, kHeight - 1 - sy);
            ASSERT_NEAR(expected, rawTestPlaneValue(plane, x, y, 0), 1e-5) << x << "," << y;
        }
    }
}

TEST_F(NativeRawPrepareFlatFieldTest, ActiveMapsTakeTheNodeOffTheGpu)
{
    NodePtr node = createPrepare(metadataWith(syntheticMaps()));
    ASSERT_TRUE(bool(node));
    EXPECT_FALSE(canRenderOnGpu(node));
    setFlatField(node, eRawPrepareFlatFieldDisabled);
#ifdef NATRON_HAVE_GPU
    EXPECT_TRUE(canRenderOnGpu(node));
#endif
}
