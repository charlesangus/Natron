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
#include <cstdio>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/AdjustBBox.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

#ifdef NATRON_HAVE_GPU
RectI
pixelRegionOfDefinition(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(0, 1.);
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// The channels of window, inside bounds, of an RGBA image over bounds.
std::vector<float>
pickChannels(const std::vector<float>& rgba,
             const RectI& bounds,
             const RectI& window,
             const std::vector<int>& channels)
{
    std::vector<float> out;

    out.reserve((std::size_t)window.area() * channels.size());
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const std::size_t at = ((std::size_t)(y - bounds.y1) * bounds.width() + (std::size_t)(x - bounds.x1)) * 4;
            for (int c : channels) {
                out.push_back(rgba[at + c]);
            }
        }
    }

    return out;
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeAdjustBBoxGpuTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeAdjustBBoxGpu64x48", 1.));
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }
};

#ifdef NATRON_HAVE_GPU

// Padding with black and with the nearest pixel, cropping, and padding one axis while cropping the
// other, in RGBA and alpha, over the whole region and over a corner tile. The copy is bit-exact.
TEST_F(NativeAdjustBBoxGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    struct Case {
        const char* name;
        double padX;
        double padY;
        bool nearest;
        int nComps;
    };
    const Case cases[] = {
        { "black pad", 5., 3., false, 4 },
        { "nearest pad", 7., 4., true, 4 },
        { "nearest pad alpha", 7., 4., true, 1 },
        { "nearest crop", -4., -2., true, 4 },
        { "nearest pad x crop y", 6., -3., true, 4 },
        { "black pad x crop y", 6., -3., false, 1 },
    };

    std::size_t nCompared = 0;
    int nRenders = 0;
    for (const Case& c : cases) {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ADJUSTBBOX), PLUGIN_MAJOR_NATRON_ADJUSTBBOX);
        ASSERT_TRUE(bool(node)) << c.name;
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL) << c.name;
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setKnobValues(node, kAdjustBBoxParamNumPixels, { c.padX, c.padY })) << c.name;
        ASSERT_TRUE(setKnobValue(node, kAdjustBBoxParamBoundary, c.nearest ? kAdjustBBoxBoundaryNearest : kAdjustBBoxBoundaryBlack)) << c.name;

        const RectI rod = pixelRegionOfDefinition(node);
        ASSERT_FALSE(rod.isNull()) << c.name;
        RenderedPlane cpu;
        ASSERT_TRUE(renderRGBA(node, rod, &cpu)) << c.name;
        ASSERT_EQ(rod, cpu.window) << c.name;

        GpuQuery query;
        query.time = kTime;
        query.nComps = c.nComps;
        query.bitDepth = eImageBitDepthFloat;
        query.plane = (c.nComps == 1) ? ImageLayerDesc::getAlphaComponents() : ImageLayerDesc::getRGBAComponents();
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << c.name;

        const std::vector<int> channels = (c.nComps == 1) ? std::vector<int> { 3 } : std::vector<int> { 0, 1, 2, 3 };
        const RectI corner(rod.x1, rod.y1, std::min(rod.x2, rod.x1 + 11), std::min(rod.y2, rod.y1 + 9));
        for (const RectI& roi : { rod, corner }) {
            const RectI inputRoI = effect->getGpuInputRoI(0, kTime, ViewIdx(0), roi, RenderScale::identity);
            GpuParityImage image;
            image.bounds = inputRoI;
            image.nComps = c.nComps;
            if (!inputRoI.isNull()) {
                RenderedPlane in;
                ASSERT_TRUE(renderRGBA(source, inputRoI, &in)) << c.name;
                ASSERT_EQ(inputRoI, in.window) << c.name;
                image.pixels = pickChannels(in.pixels, inputRoI, inputRoI, channels);
            }
            image.layout.padFloats = 6;
            image.layout.rowPadFloats = 8;

            GpuViewLayout layout;
            layout.padFloats = 4;
            layout.rowPadFloats = 4;
            layout.marginLeft = 2;
            layout.marginBottom = 1;
            layout.marginRight = 3;
            layout.marginTop = 2;
            const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, roi, layout);
            ASSERT_TRUE(run.ok) << c.name;
            EXPECT_EQ(0u, run.writtenOutside) << c.name;
            ++nRenders;

            const std::vector<float> want = pickChannels(cpu.pixels, rod, roi, channels);
            ASSERT_EQ(want.size(), run.pixels.size()) << c.name;
            const ImageDiff diff = compareImages(want, run.pixels, ParityTolerance::exact());
            nCompared += diff.compared;
            EXPECT_EQ(0u, diff.mismatches) << c.name << ": first at float " << diff.firstMismatchIndex << " gpu "
                                           << run.pixels[diff.firstMismatchIndex] << " cpu " << want[diff.firstMismatchIndex];
        }
    }
    EXPECT_GT(nCompared, 0u);
    std::printf("adjustbbox gpu vs cpu: %d renders, %zu floats compared\n", nRenders, nCompared);
}

#endif // NATRON_HAVE_GPU
