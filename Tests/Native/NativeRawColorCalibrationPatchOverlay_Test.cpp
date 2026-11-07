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

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawColorCalibrationChecker.h"
#include "Engine/Nodes/Raw/RawColorCheckerData.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
namespace Checker = RawColorCalibrationChecker;
using RawColorCheckerData::ColorChecker;
using RawColorCheckerData::ColorCheckerTarget;

const Checker::Quad kBox = { { { 35.f, 180.f }, { 280.f, 170.f }, { 292.f, 13.f }, { 25.f, 20.f } } };

const ColorChecker&
chart()
{
    return RawColorCheckerData::colorChecker(ColorCheckerTarget::Xrite24_2000);
}

Checker::ChartPlacement
placement()
{
    Checker::ChartPlacement p;

    p.checker = &chart();
    p.box = kBox;
    p.patchScale = 0.5f;

    return p;
}

std::size_t
countOfType(const OverlayDrawList& list,
            OverlayDrawList::PrimitiveTypeEnum type)
{
    std::size_t count = 0;

    for (const OverlayDrawList::Primitive& p : list.primitives()) {
        count += (p.type == type) ? 1 : 0;
    }

    return count;
}

// The primitives of patch k: its outlines, any crossings, then its disc.
std::vector<OverlayDrawList::Primitive>
patchPrimitives(const OverlayDrawList& list,
                std::size_t k)
{
    std::vector<OverlayDrawList::Primitive> found;
    std::size_t patch = 0;

    for (std::size_t i = 2; i < list.primitives().size(); ++i) {
        const OverlayDrawList::Primitive& p = list.primitives()[i];
        if (patch == k) {
            found.push_back(p);
        }
        if (p.type == OverlayDrawList::ePrimitiveTypePolygon) {
            ++patch;
        }
    }

    return found;
}

std::size_t
crossingPoints(const OverlayDrawList& list,
               std::size_t k)
{
    std::size_t points = 0;

    for (const OverlayDrawList::Primitive& p : patchPrimitives(list, k)) {
        if ((p.type == OverlayDrawList::ePrimitiveTypeLines) && (p.width < 3.f)) {
            points += p.count;
        }
    }

    return points;
}
} // namespace

TEST(RawColorCalibrationPatchOverlay, HomographyScalingIsTheAreaRatio)
{
    const Checker::Quad unit = Checker::idealBox();
    Checker::Quad doubled = unit;
    for (Checker::Point& p : doubled) {
        p.x = 2.f * p.x + 7.f;
        p.y = 2.f * p.y - 3.f;
    }
    const std::optional<Checker::Homography> same = Checker::getHomography(unit, unit);
    const std::optional<Checker::Homography> scaled = Checker::getHomography(unit, doubled);
    ASSERT_TRUE(same && scaled);
    EXPECT_NEAR(1.f, Checker::applyHomographyScaling({ 0.3f, 0.7f }, *same), 1e-3f);
    EXPECT_NEAR(4.f, Checker::applyHomographyScaling({ 0.3f, 0.7f }, *scaled), 1e-2f);
}

TEST(RawColorCalibrationPatchOverlay, NoChartDrawsNothing)
{
    OverlayDrawList list;
    Checker::ChartPlacement empty;

    Checker::buildPatchOverlay(empty, nullptr, &list);
    EXPECT_TRUE(list.empty());
}

TEST(RawColorCalibrationPatchOverlay, DrawsTheAxesAndEachPatch)
{
    OverlayDrawList list;

    Checker::buildPatchOverlay(placement(), nullptr, &list);
    const std::size_t patches = chart().patches;
    ASSERT_EQ(2 + 3 * patches, list.primitives().size());
    EXPECT_EQ(2 * patches, countOfType(list, OverlayDrawList::ePrimitiveTypePolyline));
    EXPECT_EQ(patches, countOfType(list, OverlayDrawList::ePrimitiveTypePolygon));

    const std::optional<Checker::Homography> h = Checker::getHomography(Checker::idealBox(), kBox);
    ASSERT_TRUE(h.has_value());
    const float radiusX = chart().radius * std::hypot(1.f, chart().ratio) * 0.5f;
    const float radiusY = radiusX / chart().ratio;
    for (std::size_t k = 0; k < patches; ++k) {
        const std::vector<OverlayDrawList::Primitive> primitives = patchPrimitives(list, k);
        ASSERT_EQ(3u, primitives.size());
        EXPECT_EQ(5.f, primitives[0].width);
        EXPECT_EQ(2.f, primitives[1].width);
        ASSERT_EQ(4u, primitives[1].count);
        EXPECT_TRUE(primitives[1].closed);

        const Checker::Point center = { chart().values[k].x, chart().values[k].y };
        const Checker::Point corner = Checker::applyHomography({ center.x + radiusX, center.y + radiusY }, *h);
        const OverlayDrawList::Point& drawn = list.points()[primitives[1].first + 2];
        EXPECT_NEAR(corner.x, drawn.x, 1e-3);
        EXPECT_NEAR(corner.y, drawn.y, 1e-3);

        const OverlayDrawList::Primitive& disc = primitives[2];
        const Checker::Point mapped = Checker::applyHomography(center, *h);
        double meanX = 0.;
        double meanY = 0.;
        for (std::size_t i = 0; i < disc.count; ++i) {
            meanX += list.points()[disc.first + i].x;
            meanY += list.points()[disc.first + i].y;
        }
        EXPECT_NEAR(mapped.x, meanX / (double)disc.count, 1e-3);
        EXPECT_NEAR(mapped.y, meanY / (double)disc.count, 1e-3);
        const double radius = 0.25 * (double)(radiusX + radiusY) * std::sqrt((double)Checker::applyHomographyScaling(center, *h));
        EXPECT_NEAR(radius, std::hypot(list.points()[disc.first].x - mapped.x, list.points()[disc.first].y - mapped.y), 1e-3);
        EXPECT_FALSE(disc.color.nodeColor);
    }
}

TEST(RawColorCalibrationPatchOverlay, TheGreyPatchDiscIsNeutral)
{
    OverlayDrawList list;

    Checker::buildPatchOverlay(placement(), nullptr, &list);
    const std::vector<OverlayDrawList::Primitive> primitives = patchPrimitives(list, chart().middleGrey);
    ASSERT_EQ(3u, primitives.size());
    const OverlayColor& color = primitives[2].color;
    EXPECT_NEAR(color.r, color.g, 0.05f);
    EXPECT_NEAR(color.g, color.b, 0.05f);
    EXPECT_GT(color.g, 0.2f);
    EXPECT_LT(color.g, 0.8f);
}

TEST(RawColorCalibrationPatchOverlay, DeltaECrossesThePatch)
{
    std::vector<float> deltaE(chart().patches, 0.f);
    deltaE[0] = 2.3f;
    deltaE[1] = 2.31f;
    deltaE[2] = 4.6f;
    deltaE[3] = 4.61f;
    OverlayDrawList list;

    Checker::buildPatchOverlay(placement(), &deltaE, &list);
    EXPECT_EQ(0u, crossingPoints(list, 0));
    EXPECT_EQ(2u, crossingPoints(list, 1));
    EXPECT_EQ(2u, crossingPoints(list, 2));
    EXPECT_EQ(4u, crossingPoints(list, 3));
    EXPECT_EQ(0u, crossingPoints(list, 4));
}

TEST(RawColorCalibrationPatchOverlay, AnotherChartsDeltaEIsIgnored)
{
    const std::vector<float> deltaE(chart().patches + 1, 10.f);
    OverlayDrawList list;

    Checker::buildPatchOverlay(placement(), &deltaE, &list);
    // The chart's two axes are lines too; no patch adds a crossing.
    EXPECT_EQ(2u, countOfType(list, OverlayDrawList::ePrimitiveTypeLines));
}

class NativeRawColorCalibrationPatchOverlayTest
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

TEST_F(NativeRawColorCalibrationPatchOverlayTest, TheKnobTurnsTheOverlayOnAndStaysOutOfTheHash)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    const KnobBoolPtr show = effect->getKnobByNameAndType<KnobBool>(kRawColorCalibrationParamCheckerShowPatches);
    ASSERT_TRUE(bool(show));
    EXPECT_EQ(std::string("Show Patches"), show->getLabel());
    EXPECT_FALSE(show->getValue());
    EXPECT_FALSE(show->getIsPersistent());
    EXPECT_TRUE(effect->buildOverlay_public(1., RenderScale::identity, ViewIdx(0)).empty());

    const U64 hash = node->getHashValue();
    show->setValue(true);
    EXPECT_EQ(hash, node->getHashValue());
    const OverlayDrawList& list = effect->buildOverlay_public(1., RenderScale::identity, ViewIdx(0));
    EXPECT_EQ(2 + 3 * chart().patches, list.primitives().size());
}
