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

#include <bitset>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/Native/NativeShapeTestSupport.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/ColorWheel.h"
#include "Engine/Nodes/Generator/Radial.h"
#include "Engine/Nodes/Generator/Ramp.h"
#include "Engine/Nodes/Generator/Rectangle.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
using namespace NativeShapeTest;

namespace {

#ifdef NATRON_HAVE_GPU
// The kernels run the C++ curves line for line; Vulkan rounds division, square root and atan2
// within a few ulps, which moves a weight or a hue by far less than this.
const ParityTolerance kShapesGpuTolerance = ParityTolerance::make(1e-5, 1e-6);

typedef std::vector<std::pair<std::string, std::vector<double>>> Knobs;

struct Case {
    std::string name;
    const char* pluginID;
    Knobs knobs;
};

std::vector<Case>
cases()
{
    std::vector<Case> out;
    const char* const falloffs[] = { "linear", "smooth", "easein", "easeout", "quadratic" };

    for (int f = 0; f < 5; ++f) {
        out.push_back({ std::string("ramp diagonal ") + falloffs[f], PLUGINID_NATRON_RAMP,
                        Knobs { { kRampParamPoint0, { 5.5, 7.25 } },
                                { kRampParamPoint1, { 49., 38.5 } },
                                { kRampParamColor0, { 0.1, 0.2, 0.3, 0.25 } },
                                { kRampParamColor1, { 0.9, 0.6, 0.05, 1. } },
                                { kRampParamFalloff, { (double)f } } } });
    }
    out.push_back({ "ramp vertical, HDR colours", PLUGINID_NATRON_RAMP,
                    Knobs { { kRampParamPoint0, { 20., 4. } },
                            { kRampParamPoint1, { 20., 40. } },
                            { kRampParamColor0, { -1., 0.5, 2., 0. } },
                            { kRampParamColor1, { 16., 4., -0.5, 1. } } } });
    out.push_back({ "ramp with coincident points", PLUGINID_NATRON_RAMP,
                    Knobs { { kRampParamPoint0, { 30., 20. } },
                            { kRampParamPoint1, { 30., 20. } } } });

    out.push_back({ "radial hard edge", PLUGINID_NATRON_RADIAL,
                    Knobs { { kRadialParamSoftness, { 0. } },
                            { kRadialParamColor0, { 0.05, 0.1, 0.15, 0. } },
                            { kRadialParamColor1, { 0.8, 0.7, 0.6, 1. } } } });
    for (int f = 0; f < 5; ++f) {
        out.push_back({ std::string("radial soft ") + falloffs[f], PLUGINID_NATRON_RADIAL,
                        Knobs { { kRadialParamSoftness, { 9.5 } },
                                { kRadialParamFalloff, { (double)f } } } });
    }
    out.push_back({ "radial wider than its softness is deep", PLUGINID_NATRON_RADIAL,
                    Knobs { { kRadialParamSoftness, { 1000. } },
                            { kRadialParamColor1, { 4., 2., -1., 1. } } } });

    out.push_back({ "rectangle hard edge, elliptical corners", PLUGINID_NATRON_RECTANGLE,
                    Knobs { { kRectangleParamSoftness, { 0. } },
                            { kRectangleParamCornerRadius, { 6., 3. } } } });
    for (int f = 0; f < 5; ++f) {
        out.push_back({ std::string("rectangle soft, round corners ") + falloffs[f], PLUGINID_NATRON_RECTANGLE,
                        Knobs { { kRectangleParamSoftness, { 7.25 } },
                                { kRectangleParamCornerRadius, { 12., 12. } },
                                { kRectangleParamFalloff, { (double)f } },
                                { kRectangleParamColor0, { 0.2, 0.1, 0., 0. } },
                                { kRectangleParamColor1, { 0.5, 1.5, 3., 1. } } } });
    }
    out.push_back({ "rectangle with clamped corners", PLUGINID_NATRON_RECTANGLE,
                    Knobs { { kRectangleParamSoftness, { 3. } },
                            { kRectangleParamCornerRadius, { 1000., 30. } } } });

    out.push_back({ "colorwheel defaults", PLUGINID_NATRON_COLORWHEEL, Knobs() });
    out.push_back({ "colorwheel soft, rotated, blended", PLUGINID_NATRON_COLORWHEEL,
                    Knobs { { kColorWheelParamSoftness, { 6.5 } },
                            { kColorWheelParamRotate, { 37. } },
                            { kColorWheelParamCenterSaturation, { 0.2 } },
                            { kColorWheelParamEdgeSaturation, { 0.8 } },
                            { kColorWheelParamCenterValue, { 0.5 } },
                            { kColorWheelParamEdgeValue, { 2. } } } });

    return out;
}

bool
setCaseKnobs(const NodePtr& node,
             const Case& c)
{
    // An odd-sized extent off the origin, with odd edges at mipmap 0 and 1 (55x41 and 28x21 or so).
    bool ok = setKnobValue(node, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize);
    ok = ok && setKnobValues(node, kNativeGeneratorParamBottomLeft, { 3., 2. });
    ok = ok && setKnobValues(node, kNativeGeneratorParamSize, { 55., 41. });
    for (const auto& k : c.knobs) {
        ok = ok && setKnobValues(node, k.first, k.second);
    }

    return ok;
}

RectI
extentOf(const NodePtr& node,
         unsigned mipmapLevel)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;

    EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod));

    return rod.toPixelEnclosing(mipmapLevel, 1.);
}

GpuQuery
queryFor(const NodePtr& node,
         unsigned mipmapLevel)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    GpuQuery q;

    q.time = kTime;
    q.mappedScale = RenderScale::fromMipmapLevel(mipmapLevel);
    q.view = ViewIdx(0);
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;
    const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), ImageLayerDesc::mapNCompsToColorLayer(4));
    for (int i = 0; i < 4; ++i) {
        q.processChannels[i] = channels[i];
    }

    return q;
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeShapesGpuTest
    : public BaseTest {
};

// Each shape generator's GPU kernel against its CPU render at mipmap 0 and 1, within
// kShapesGpuTolerance, into tight and padded output views.
TEST_F(NativeShapesGpuTest, MatchesTheCpuRender)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }

    int nCase = 0;
    for (const Case& c : cases()) {
        for (unsigned level = 0; level <= 1; ++level) {
            resetProject(getApp(), 64, 48);
            NodePtr node = createNode(QString::fromUtf8(c.pluginID), 1);
            ASSERT_TRUE(bool(node)) << c.name;
            StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
            ASSERT_TRUE(effect != NULL) << c.name;
            ASSERT_TRUE(setCaseKnobs(node, c)) << c.name;

            const RectI window = extentOf(node, level);
            const GpuQuery query = queryFor(node, level);
            ASSERT_TRUE(effect->canRenderOnGpu(query)) << c.name;

            RenderedPlane cpu;
            ASSERT_TRUE(renderRgba(node, window, &cpu, level)) << c.name;

            GpuViewLayout layout;
            if (nCase++ % 2) {
                layout.padFloats = 8;
                layout.rowPadFloats = 5;
                layout.marginLeft = 3;
                layout.marginBottom = 2;
                layout.marginRight = 4;
                layout.marginTop = 1;
            }
            const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(), nullptr, window, layout);
            ASSERT_TRUE(run.ok) << c.name << ", mipmap " << level;
            EXPECT_EQ(0u, run.writtenOutside) << c.name;
            ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << c.name;
            EXPECT_EQ(0, countNonFinite(cpu)) << c.name << ", mipmap " << level;

            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, kShapesGpuTolerance);
            EXPECT_EQ(0u, diff.mismatches) << c.name << ", mipmap " << level << ": first at float " << diff.firstMismatchIndex << " gpu "
                                           << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex] << ", worst "
                                           << diff.worstAbsolute;
        }
    }
#endif // NATRON_HAVE_GPU
}
