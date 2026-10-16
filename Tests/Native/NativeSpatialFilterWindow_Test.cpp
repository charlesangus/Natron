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
#include <list>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The pixel every render is read at. Its alpha band is the parity source's exact 1 under both
// masks, the one at the origin and the one at kShiftedMaskX, kShiftedMaskY.
const int kPixelX = 30;
const int kPixelY = 29;
const int kShiftedMaskX = 3;
const int kShiftedMaskY = 24;

// The recursive filters start at the buffer's edge; the largest move of a pixel between the full
// frame and a 1x1 window on the parity source, sizes 3 to 20, is 1.8e-2 for the quasi-Gaussian.
const float kRecursiveWindowTolerance = 2e-2f;

enum MaskCase {
    eMaskOff,
    eMaskAtOrigin,
    eMaskShifted
};

const MaskCase kMaskCases[] = { eMaskOff, eMaskAtOrigin, eMaskShifted };

const char*
maskName(MaskCase m)
{
    switch (m) {
    case eMaskOff:
        return "no mask";
    case eMaskAtOrigin:
        return "mask at the origin";
    case eMaskShifted:
        return "shifted mask";
    }

    return "";
}

// The whole frame, a window around the pixel whose lower rows the shifted mask zeroes, and the
// pixel alone.
std::vector<RectI>
renderWindows()
{
    std::vector<RectI> windows;

    windows.push_back(RectI(0, 0, kParitySourceWidth, kParitySourceHeight));
    windows.push_back(RectI(kPixelX - 4, kPixelY - 3, kPixelX + 5, kPixelY + 4));
    windows.push_back(RectI(kPixelX, kPixelY, kPixelX + 1, kPixelY + 1));

    return windows;
}

std::string
describeWindow(const RectI& w)
{
    std::ostringstream s;
    s << "window (" << w.x1 << "," << w.y1 << ")-(" << w.x2 << "," << w.y2 << ")";

    return s.str();
}

struct FilterCase {
    std::string name;
    std::string pluginID;
    int major;
    std::function<bool(const NodePtr&)> configure;
    // 0 when every window must give the same pixel exactly.
    float windowTolerance;
};

} // namespace

class NativeSpatialFilterWindowTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeSpatialFilterWindow64x48", 1.));
    }

    // A fresh filter on a parity source at the origin, with the mask of `mask` on its input 1.
    NodePtr makeFilter(const FilterCase& c,
                       MaskCase mask)
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        NodePtr node = createNode(QString::fromStdString(c.pluginID), c.major);
        EXPECT_TRUE(bool(source));
        EXPECT_TRUE(bool(node));
        if (!source || !node) {
            return NodePtr();
        }
        setParitySourceOrigin(source, 0, 0);
        connectNodes(source, node, 0, true);
        EXPECT_TRUE(c.configure(node)) << c.name;
        if (mask != eMaskOff) {
            NodePtr maskSource = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
            EXPECT_TRUE(bool(maskSource));
            if (!maskSource) {
                return NodePtr();
            }
            if (mask == eMaskShifted) {
                setParitySourceOrigin(maskSource, kShiftedMaskX, kShiftedMaskY);
            } else {
                setParitySourceOrigin(maskSource, 0, 0);
            }
            connectNodes(maskSource, node, 1, true);
            EXPECT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
            KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
            EXPECT_TRUE(maskChannel != NULL);
            if (maskChannel) {
                maskChannel->set("rgba.A");
            }
        }

        return node;
    }

    // The RGBA value of the pixel in a render of `window`.
    bool renderPixel(const NodePtr& node,
                     const RectI& window,
                     float pixel[4])
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
        EXPECT_TRUE(ok) << error;
        if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4) || !(planes[0].window == window)) {
            return false;
        }
        const RenderedPlane& plane = planes[0];
        const std::size_t at = ((std::size_t)(kPixelY - window.y1) * window.width() + (std::size_t)(kPixelX - window.x1)) * 4;
        for (int c = 0; c < 4; ++c) {
            pixel[c] = plane.pixels[at + c];
        }

        return true;
    }

    // Under each mask the pixel is the same in a given window, bit for bit, as both masks are 1
    // there; across the windows it is the same within the case's tolerance.
    void expectWindowAndMaskInvariance(const FilterCase& c)
    {
        resetProject();
        const std::vector<RectI> windows = renderWindows();
        float reference[4] = { 0.f, 0.f, 0.f, 0.f };
        bool haveReference = false;
        float maxWindowDiff = 0.f;
        for (const RectI& window : windows) {
            float unmasked[4] = { 0.f, 0.f, 0.f, 0.f };
            for (MaskCase mask : kMaskCases) {
                const std::string what = c.name + ", " + maskName(mask) + ", " + describeWindow(window);
                const NodePtr node = makeFilter(c, mask);
                ASSERT_TRUE(bool(node)) << what;
                float pixel[4];
                ASSERT_TRUE(renderPixel(node, window, pixel)) << what;
                for (int ch = 0; ch < 4; ++ch) {
                    ASSERT_FALSE(std::isnan(pixel[ch])) << what << " channel " << ch;
                }
                if (mask == eMaskOff) {
                    std::copy(pixel, pixel + 4, unmasked);
                } else {
                    for (int ch = 0; ch < 4; ++ch) {
                        EXPECT_EQ(unmasked[ch], pixel[ch]) << what << " channel " << ch << ": the mask, 1 at the pixel, changed it";
                    }
                }
                if (!haveReference) {
                    std::copy(pixel, pixel + 4, reference);
                    haveReference = true;
                }
                for (int ch = 0; ch < 4; ++ch) {
                    const float diff = std::fabs(pixel[ch] - reference[ch]);
                    maxWindowDiff = std::max(maxWindowDiff, diff);
                    if (c.windowTolerance == 0.f) {
                        EXPECT_EQ(reference[ch], pixel[ch]) << what << " channel " << ch << ": differs from the full-frame render";
                    } else {
                        EXPECT_LE(diff, c.windowTolerance) << what << " channel " << ch << ": " << pixel[ch] << " against the full frame's " << reference[ch];
                    }
                }
            }
        }
        std::cout << "[ window ] " << c.name << ": max diff across windows " << maxWindowDiff << std::endl;
    }
};

TEST_F(NativeSpatialFilterWindowTest, ShiftedMaskZeroesTheLowerRowsOfTheSmallWindowButNotThePixel)
{
    EXPECT_EQ(1.f, paritySourceColorValue(3, kPixelX, kPixelY, kTime));
    EXPECT_EQ(1.f, paritySourceColorValue(3, kPixelX - kShiftedMaskX, kPixelY - kShiftedMaskY, kTime));
    const RectI small = renderWindows()[1];
    for (int y = small.y1; y < kShiftedMaskY + 4; ++y) {
        for (int x = small.x1; x < small.x2; ++x) {
            const bool inside = (x >= kShiftedMaskX) && (y >= kShiftedMaskY);
            const float alpha = inside ? paritySourceColorValue(3, x - kShiftedMaskX, y - kShiftedMaskY, kTime) : 0.f;
            EXPECT_EQ(0.f, alpha) << "(" << x << "," << y << ")";
        }
    }
}

TEST_F(NativeSpatialFilterWindowTest, FirBlurIsWindowAndMaskInvariant)
{
    FilterCase c;
    c.name = "FIR Gaussian blur 6";
    c.pluginID = PLUGINID_NATRON_BLUR;
    c.major = PLUGIN_MAJOR_NATRON_BLUR;
    c.configure = [](const NodePtr& node) {
        return setKnobValue(node, kBlurParamFilter, std::string(kBlurParamFilterFIRGaussian)) && setKnobValues(node, kBlurParamSize, { 6. });
    };
    c.windowTolerance = 0.f;
    expectWindowAndMaskInvariance(c);
}

TEST_F(NativeSpatialFilterWindowTest, RecursiveBlurIsMaskInvariantAndWindowBounded)
{
    for (const char* filter : { kBlurParamFilterGaussian, kBlurParamFilterQuasiGaussian }) {
        FilterCase c;
        c.name = std::string(filter) + " blur 6";
        c.pluginID = PLUGINID_NATRON_BLUR;
        c.major = PLUGIN_MAJOR_NATRON_BLUR;
        const std::string filterName(filter);
        c.configure = [filterName](const NodePtr& node) {
            return setKnobValue(node, kBlurParamFilter, filterName) && setKnobValues(node, kBlurParamSize, { 6. });
        };
        c.windowTolerance = kRecursiveWindowTolerance;
        expectWindowAndMaskInvariance(c);
    }
}

TEST_F(NativeSpatialFilterWindowTest, ErodeDilateIsWindowAndMaskInvariant)
{
    FilterCase c;
    c.name = "erode 6x-3";
    c.pluginID = PLUGINID_NATRON_ERODE;
    c.major = PLUGIN_MAJOR_NATRON_ERODE;
    c.configure = [](const NodePtr& node) {
        return setKnobValues(node, kErodeDilateParamSize, { 6., -3. });
    };
    c.windowTolerance = 0.f;
    expectWindowAndMaskInvariance(c);
}

TEST_F(NativeSpatialFilterWindowTest, EdgeDetectFiniteDifferencesAreWindowAndMaskInvariant)
{
    FilterCase c;
    c.name = "EdgeDetect Sobel, erode 2, suppression";
    c.pluginID = PLUGINID_NATRON_EDGEDETECT;
    c.major = PLUGIN_MAJOR_NATRON_EDGEDETECT;
    c.configure = [](const NodePtr& node) {
        return setKnobValue(node, kEdgeDetectParamFilter, std::string(kEdgeDetectParamFilterSobel)) && setKnobValues(node, kEdgeDetectParamBlurSize, { 0. })
            && setKnobValues(node, kEdgeDetectParamErodeSize, { 2. }) && setKnobValues(node, kEdgeDetectParamNMS, { 1. });
    };
    c.windowTolerance = 0.f;
    expectWindowAndMaskInvariance(c);
}
