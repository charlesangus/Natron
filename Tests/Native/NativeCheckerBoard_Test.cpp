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
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Image/NativeGenerator.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardID = PLUGINID_NATRON_CHECKERBOARD;
const int kOfxCheckerBoardMajor = 1;
const int kNativeCheckerBoardMajor = PLUGIN_MAJOR_NATRON_CHECKERBOARD;
const double kTime = 1.;

// Every pixel is one of six knob colours chosen by comparisons, so the exact class.
const ParityTolerance kCheckerBoardTolerance = ParityTolerance::exact();

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// The project window and a margin around it, so clipping to the region of definition is
// compared too.
RectI
caseWindow(unsigned mipmapLevel,
           double par)
{
    return RectD(-8. * par, -8., 72. * par, 56.).toPixelEnclosing(mipmapLevel, par);
}

// The openfx-misc per-pixel test: the reference every run must match.
CheckerBoardColorEnum
perPixelColor(const CheckerBoardGeometry& g,
              int x,
              int y)
{
    if (((g.centerY - g.centerlineInfY) <= y) && (y < (g.centerY + g.centerlineSupY))) {
        return eCheckerBoardColorCenterline;
    }
    const double yline = g.centerY + g.boxSizeY * std::floor((y - g.centerY) / g.boxSizeY + 0.5);
    if (((yline - g.lineInfY) <= y) && (y < (yline + g.lineSupY))) {
        return eCheckerBoardColorLine;
    }
    const int ybox = (int)std::floor((y - g.centerY) / g.boxSizeY);
    if (((g.centerX - g.centerlineInfX) <= x) && (x < (g.centerX + g.centerlineSupX))) {
        return eCheckerBoardColorCenterline;
    }
    const double xline = g.centerX + g.boxSizeX * std::floor((x - g.centerX) / g.boxSizeX + 0.5);
    if (((xline - g.lineInfX) <= x) && (x < (xline + g.lineSupX))) {
        return eCheckerBoardColorLine;
    }
    const int xbox = static_cast<int>(std::floor((x - g.centerX) / g.boxSizeX));
    if (ybox & 1) {
        return (xbox & 1) ? eCheckerBoardColor2 : eCheckerBoardColor3;
    }

    return (xbox & 1) ? eCheckerBoardColor1 : eCheckerBoardColor0;
}

// Walks [x0, xEnd) of row y run by run and counts the pixels whose run colour differs from the
// per-pixel colour, reporting the first few.
int
countRunMismatches(const CheckerBoardGeometry& g,
                   int y,
                   int x0,
                   int xEnd,
                   const std::string& what)
{
    const CheckerBoardRow row = checkerBoardRow(g, y);
    int mismatches = 0;
    int x = x0;
    while (x < xEnd) {
        CheckerBoardColorEnum color = eCheckerBoardColor0;
        const int end = checkerBoardRunEnd(g, row, x, xEnd, &color);
        if ((end <= x) || (end > xEnd)) {
            ADD_FAILURE() << what << ": run from " << x << " ends at " << end << ", row ends at " << xEnd;

            return mismatches + 1;
        }
        for (; x < end; ++x) {
            const CheckerBoardColorEnum expected = perPixelColor(g, x, y);
            if (expected != color) {
                if (++mismatches <= 3) {
                    ADD_FAILURE() << what << ": pixel (" << x << ", " << y << ") gets colour " << color << ", per pixel " << expected;
                }
            }
        }
    }

    return mismatches;
}

} // namespace

class NativeCheckerBoardTest
    : public BaseTest {
protected:
    // A fresh project whose default format is 64x48 at the given pixel aspect ratio.
    void resetProject(double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        // The format choice is keyed by name, so each pixel aspect ratio needs its own.
        project->setOrAddProjectFormat(Format(0, 0, 64, 48, "nativeCheckerBoardPar" + std::to_string(par), par));
    }

    // Both versions with nothing connected. The parity source is only there because
    // compareParity() requires one; every case passes its own window.
    ParityPair makePair()
    {
        ParityPair pair;

        pair.id = kCheckerBoardID;
        pair.ofxMajor = kOfxCheckerBoardMajor;
        pair.nativeMajor = kNativeCheckerBoardMajor;
        pair.app = getApp();
        pair.source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        if (isPluginMajorRegistered(kCheckerBoardID, kOfxCheckerBoardMajor)) {
            pair.ofx = createNodeAtMajor(getApp(), kCheckerBoardID, kOfxCheckerBoardMajor);
        }
        pair.native = createNodeAtMajor(getApp(), kCheckerBoardID, kNativeCheckerBoardMajor);
        EXPECT_TRUE(bool(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX CheckerBoard is retired, so parity replays the recorded references";
        EXPECT_TRUE(isNative(pair.native));

        return pair;
    }

    // Compares the pair at mipmap 0, 1 and 2: the box size, the line widths and the centre all
    // scale with the render scale, and the centre lines stay at least a pixel wide.
    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record,
                      double par = 1.,
                      double time = kTime)
    {
        ParityOptions options;

        options.time = time;
        for (unsigned mipmapLevel = 0; mipmapLevel <= 2; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, caseWindow(mipmapLevel, par), mipmapLevel, kCheckerBoardTolerance, record, options);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] CheckerBoard " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }
};

TEST_F(NativeCheckerBoardTest, Default)
{
    resetProject();
    ParityPair pair = makePair();
    expectParity(pair, "default", true);
}

TEST_F(NativeCheckerBoardTest, SmallBoxesWithLines)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamBoxSize, { 10., 7. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamLineWidth, { 2. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamCenterLineWidth, { 3. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamLineColor, { 0.25, 0.5, 0.75, 1. }));
    expectParity(pair, "boxes-line2", true);
}

TEST_F(NativeCheckerBoardTest, PixelAspectRatioTwoProject)
{
    resetProject(2.);
    ParityPair pair = makePair();
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamBoxSize, { 12., 9. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamLineWidth, { 2. }));
    expectParity(pair, "par2", true, 2.);
}

TEST_F(NativeCheckerBoardTest, UnversionedRequestsGetTheNativeCheckerBoard)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kCheckerBoardID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeCheckerBoardMajor, unversioned->getMajorVersion());
}

TEST_F(NativeCheckerBoardTest, RendersTheSameInBothSchedulerModes)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kCheckerBoardID), kNativeCheckerBoardMajor);
    ASSERT_TRUE(isNative(node));
    ASSERT_TRUE(setKnobValues(node, kCheckerBoardParamBoxSize, { 10., 7. }));
    ASSERT_TRUE(setKnobValues(node, kCheckerBoardParamLineWidth, { 2. }));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 2; ++mipmapLevel) {
        std::vector<int> unplannedPulls;
        const RectI window = RectD(0., 0., 64., 48.).toPixelEnclosing(mipmapLevel, 1.);
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
            EXPECT_EQ(0, unplannedPulls[p]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[p];
        }
    }
}

// The runs give every pixel the colour of the per-pixel test, at odd box sizes, line widths,
// region offsets, render scales and pixel aspect ratios, over rows that start anywhere: the
// whole window, and the window cut into short tiles so that runs start inside boxes and lines.
TEST(CheckerBoardRuns, MatchThePerPixelTest)
{
    const double boxSizes[][2] = { { 64., 64. }, { 1., 1. }, { 3., 7. }, { 7.5, 2.5 }, { 10., 10. }, { 13.3, 5.7 } };
    const double lineWidths[] = { 0., 1., 2.5 };
    const double centerlineWidths[] = { 0., 1., 3. };
    const RectD rods[] = { RectD(0., 0., 64., 48.), RectD(-3., 5., 61., 53.), RectD(1.5, -2.25, 100.75, 31.) };
    const double scales[] = { 1., 0.5, 0.3 };
    const double pars[] = { 1., 2. };
    const int tile = 13;

    int mismatches = 0;
    for (const auto& box : boxSizes) {
        for (double lineWidth : lineWidths) {
            for (double centerlineWidth : centerlineWidths) {
                for (const RectD& rod : rods) {
                    for (double scale : scales) {
                        for (double par : pars) {
                            const CheckerBoardGeometry g = checkerBoardGeometry(box[0], box[1], lineWidth, centerlineWidth, rod, scale, scale, par);
                            const int x1 = (int)std::floor(rod.x1 * scale / par) - 9;
                            const int x2 = (int)std::ceil(rod.x2 * scale / par) + 9;
                            const int y1 = (int)std::floor(rod.y1 * scale) - 5;
                            const int y2 = (int)std::ceil(rod.y2 * scale) + 5;
                            std::ostringstream what;
                            what << "box " << box[0] << "x" << box[1] << ", line " << lineWidth << ", centerline " << centerlineWidth
                                 << ", rod (" << rod.x1 << ", " << rod.y1 << ", " << rod.x2 << ", " << rod.y2 << "), scale " << scale << ", par " << par;
                            for (int y = y1; y < y2; ++y) {
                                mismatches += countRunMismatches(g, y, x1, x2, what.str());
                                for (int x0 = x1; x0 < x2; x0 += tile) {
                                    mismatches += countRunMismatches(g, y, x0, (std::min)(x0 + tile, x2), what.str() + ", tiled");
                                }
                            }
                            if (mismatches > 20) {
                                FAIL() << "stopping after " << mismatches << " mismatches";
                            }
                        }
                    }
                }
            }
        }
    }
    EXPECT_EQ(0, mismatches);
}
