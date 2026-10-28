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
#include <bitset>
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

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/NativeGenerator.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

bool
renderRgba(const NodePtr& node,
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

RectI
extentOf(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;

    EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod));

    return rod.toPixelEnclosing(0, 1.);
}

#ifdef NATRON_HAVE_GPU
GpuQuery
queryFor(const NodePtr& node)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    GpuQuery q;

    q.time = kTime;
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

class NativeConstantGpuTest
    : public BaseTest {
};

// The generator writes its knob values untouched: the exact class, with zero in the channels of
// the target layer it leaves unprocessed.
TEST_F(NativeConstantGpuTest, MatchesTheCpuRender)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }

    struct Case {
        const char* name;
        const char* id;
        std::vector<std::string> channels;
        std::vector<double> color;
    };
    const Case cases[] = {
        { "constant", PLUGINID_NATRON_CONSTANT, {}, { 0.25, -0.5, 1.75, 0.6 } },
        { "constant, RG", PLUGINID_NATRON_CONSTANT, { "R", "G" }, { 0.75, 0.5, 0.25, 0.125 } },
        { "constant, A", PLUGINID_NATRON_CONSTANT, { "A" }, { 0.75, 0.5, 0.25, 0.125 } },
    };
    int nCase = 0;
    for (const Case& c : cases) {
        NodePtr node = createNode(QString::fromUtf8(c.id));
        ASSERT_TRUE(bool(node)) << c.name;
        StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL) << c.name;
        ASSERT_TRUE(setKnobValues(node, kConstantParamColor, c.color)) << c.name;
        if (!c.channels.empty()) {
            KnobLayerSelectPtr layer = std::dynamic_pointer_cast<KnobLayerSelect>(node->getLayerKnob());
            ASSERT_TRUE(bool(layer)) << c.name;
            layer->setChannels(c.channels);
        }

        const RectI window = extentOf(node);
        const GpuQuery query = queryFor(node);
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << c.name;

        RenderedPlane cpu;
        ASSERT_TRUE(renderRgba(node, window, &cpu)) << c.name;

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
        ASSERT_TRUE(run.ok) << c.name;
        EXPECT_EQ(0u, run.writtenOutside) << c.name;
        ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << c.name;

        const ImageDiff diff = compareImages(cpu.pixels, run.pixels, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << c.name << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
    }
#endif // NATRON_HAVE_GPU
}

TEST_F(NativeConstantGpuTest, ConstantGradeBlurRunsAsOneRegionWithoutUploads)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    ASSERT_TRUE(bool(constant) && bool(grade) && bool(blur));
    connectNodes(constant, grade, 0, true);
    connectNodes(grade, blur, 0, true);
    ASSERT_TRUE(setKnobValues(constant, kConstantParamColor, { 0.2, 0.4, 0.6, 0.8 }));
    ASSERT_TRUE(setKnobValues(grade, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 3., 3. }));

    const RectI window = extentOf(constant);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ constant, grade, blur }), blur, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    for (std::size_t r = 0; r < result.regionNodes.size(); ++r) {
        const std::vector<NodePtr>& nodes = result.regionNodes[r];
        const bool hasConstant = std::find(nodes.begin(), nodes.end(), constant) != nodes.end();
        const bool hasGrade = std::find(nodes.begin(), nodes.end(), grade) != nodes.end();
        const bool hasBlur = std::find(nodes.begin(), nodes.end(), blur) != nodes.end();
        if (hasConstant || hasGrade || hasBlur) {
            ++holding;
            EXPECT_TRUE(hasConstant && hasGrade && hasBlur);
            EXPECT_TRUE(result.regions[r].externalInputs.empty()) << "the region uploads an image";
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::make(2e-6, 1e-6));
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}
