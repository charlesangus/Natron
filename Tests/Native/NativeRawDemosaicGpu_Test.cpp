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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawPrepare.h"

NATRON_NAMESPACE_USING

namespace {
// PPG and RCD add, subtract and multiply, which Vulkan rounds exactly, and divide, which it
// rounds within 2.5 ulp: the averages on PPG's border and RCD's direction weights and estimates.
const ParityTolerance kDemosaicGpuTolerance = ParityTolerance::transcendental();
const ParityTolerance kPassThroughGpuTolerance = ParityTolerance::exact();

struct Method {
    const char* name;
    RawDemosaicBayerMethodEnum method;
    double edgeThreshold;
};

const Method kMethods[] = {
    { "RCD", eRawDemosaicBayerRCD, 0. },
    { "PPG", eRawDemosaicBayerPPG, 0. },
    { "PPG with median", eRawDemosaicBayerPPG, 0.02 },
};
} // namespace

class NativeRawDemosaicGpuTest
    : public RawGpuTest {
protected:
    void setChoice(const NodePtr& node,
                   const char* name,
                   int value)
    {
        KnobChoicePtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }

    void setMethod(const NodePtr& demosaic,
                   const Method& method)
    {
        setChoice(demosaic, kRawDemosaicParamBayerMethod, (int)method.method);
        KnobDoublePtr threshold = demosaic->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawDemosaicParamEdgeThreshold);
        ASSERT_TRUE(bool(threshold));
        threshold->setValue(method.edgeThreshold);
    }

    // The size of the prepared image.
    static RectI preparedRoD(const NodePtr& prepare)
    {
        RawPrepare* effect = dynamic_cast<RawPrepare*>(prepare->getEffectInstance().get());
        const std::optional<RawPrepare::Settings> settings = effect ? effect->resolveSettings(kTime, ViewIdx(0)) : std::nullopt;

        return settings ? RectI(0, 0, settings->kept.width(), settings->kept.height()) : RectI();
    }
};

#ifdef NATRON_HAVE_GPU

// PPG, with and without its median, and RCD on the Bayer fixture: inside the image, and at two
// corners, where the buffer the methods run over ends at the image's edge.
TEST_F(NativeRawDemosaicGpuTest, PpgAndRcdMatchTheCpuOnTheBayerFixture)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr read = createRead(bayerFixture());
    NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
    NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
    ASSERT_TRUE(read && prepare && demosaic);
    const RectI rod = preparedRoD(prepare);
    ASSERT_GT(rod.width(), 400);
    ASSERT_GT(rod.height(), 400);
    const RectI windows[] = {
        RectI(200, 150, 264, 198),
        RectI(0, 0, 40, 36),
        RectI(rod.x2 - 41, rod.y2 - 37, rod.x2, rod.y2),
        RectI(301, 211, 302, 250),
    };

    for (const Method& method : kMethods) {
        setMethod(demosaic, method);
        ASSERT_FALSE(HasFatalFailure());
        for (const RectI& window : windows) {
            expectGpuMatchesCpu(*runtime, demosaic, prepare, window, kDemosaicGpuTolerance,
                                std::string(method.name) + " over " + std::to_string(window.x1) + "," + std::to_string(window.y1) + " " + std::to_string(window.width()) + "x" + std::to_string(window.height()));
        }
    }
}

// A window rendered on the GPU in tiles is the window rendered whole, bit for bit: each tile runs
// over the source region its own pixels need, which their values do not depend on.
TEST_F(NativeRawDemosaicGpuTest, TiledGpuRenderEqualsTheFullFrameGpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr read = createRead(bayerFixture());
    NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
    NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
    ASSERT_TRUE(read && prepare && demosaic);
    StagedImageEffect* effect = stagedOf(demosaic);
    ASSERT_TRUE(effect != NULL);
    const RectI full(150, 120, 246, 200);
    const int splitX = full.x1 + 37;
    const int splitY = full.y1 + 29;
    const RectI tiles[] = {
        RectI(full.x1, full.y1, splitX, splitY),
        RectI(splitX, full.y1, full.x2, splitY),
        RectI(full.x1, splitY, splitX, full.y2),
        RectI(splitX, splitY, full.x2, full.y2),
    };
    const GpuQuery query = queryFor(4);

    for (const Method& method : kMethods) {
        setMethod(demosaic, method);
        ASSERT_FALSE(HasFatalFailure());
        RenderedPlane input;
        ASSERT_TRUE(renderCpu(prepare, effect->getGpuInputRoI(0, full, RenderScale::identity), &input));
        const std::vector<GpuParityImage> inputs(1, imageOf(input));
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << method.name;
        const GpuRunResult whole = runRenderGpu(*runtime, *effect, query, inputs, nullptr, full);
        ASSERT_TRUE(whole.ok) << method.name;

        std::vector<float> assembled(whole.pixels.size(), -1.f);
        for (const RectI& tile : tiles) {
            const GpuRunResult part = runRenderGpu(*runtime, *effect, query, inputs, nullptr, tile);
            ASSERT_TRUE(part.ok) << method.name;
            EXPECT_EQ(0u, part.writtenOutside) << method.name;
            for (int y = tile.y1; y < tile.y2; ++y) {
                const std::size_t from = (std::size_t)(y - tile.y1) * tile.width() * 4;
                const std::size_t to = ((std::size_t)(y - full.y1) * full.width() + (std::size_t)(tile.x1 - full.x1)) * 4;
                std::memcpy(&assembled[to], &part.pixels[from], (std::size_t)tile.width() * 4 * sizeof(float));
            }
        }
        const ImageDiff diff = compareImages(whole.pixels, assembled, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << method.name << ": first at float " << diff.firstMismatchIndex << " (whole " << whole.pixels[diff.firstMismatchIndex]
                                       << ", tiled " << assembled[diff.firstMismatchIndex] << ")";
    }
}

// Linear data has no CFA and passes through as RGB with an alpha of 1, in the output kernel.
TEST_F(NativeRawDemosaicGpuTest, LinearPassThroughMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr read = createRead(linearFixture());
    NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
    NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
    ASSERT_TRUE(read && prepare && demosaic);
    expectGpuMatchesCpu(*runtime, demosaic, prepare, RectI(200, 150, 264, 198), kPassThroughGpuTolerance, "linear pass-through");
}

// AMaZE, Markesteijn, green equilibration and colour smoothing have no GPU path, so a chain using
// them renders on the CPU. VNG has one.
TEST_F(NativeRawDemosaicGpuTest, MethodsWithoutAGpuPathStayOnTheCpu)
{
    NodePtr bayer = createRead(bayerFixture());
    NodePtr bayerPrepare = createAfter(PLUGINID_NATRON_RAWPREPARE, bayer);
    NodePtr bayerDemosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, bayerPrepare);
    NodePtr xtrans = createRead(xtransFixture());
    NodePtr xtransPrepare = createAfter(PLUGINID_NATRON_RAWPREPARE, xtrans);
    NodePtr xtransDemosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, xtransPrepare);
    ASSERT_TRUE(bayerDemosaic && xtransDemosaic);
    StagedImageEffect* onBayer = stagedOf(bayerDemosaic);
    StagedImageEffect* onXTrans = stagedOf(xtransDemosaic);
    const GpuQuery query = queryFor(4);

    EXPECT_TRUE(onBayer->canRenderOnGpu(query));
    setChoice(bayerDemosaic, kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerAMaZE);
    EXPECT_FALSE(onBayer->canRenderOnGpu(query));
    setChoice(bayerDemosaic, kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerVNG4);
    EXPECT_TRUE(onBayer->canRenderOnGpu(query));
    setChoice(bayerDemosaic, kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerRCD);
    setChoice(bayerDemosaic, kRawDemosaicParamGreenEq, (int)eRawDemosaicGreenEqLocal);
    EXPECT_FALSE(onBayer->canRenderOnGpu(query));
    setChoice(bayerDemosaic, kRawDemosaicParamGreenEq, (int)eRawDemosaicGreenEqFull);
    EXPECT_FALSE(onBayer->canRenderOnGpu(query));
    setChoice(bayerDemosaic, kRawDemosaicParamGreenEq, (int)eRawDemosaicGreenEqNone);
    bayerDemosaic->getEffectInstance()->getKnobByNameAndType<KnobInt>(kRawDemosaicParamColorSmoothing)->setValue(1);
    EXPECT_FALSE(onBayer->canRenderOnGpu(query));

    for (int method : { (int)eRawDemosaicXTransMarkesteijn1, (int)eRawDemosaicXTransMarkesteijn3 }) {
        setChoice(xtransDemosaic, kRawDemosaicParamXTransMethod, method);
        EXPECT_FALSE(onXTrans->canRenderOnGpu(query)) << "method " << method;
    }
    setChoice(xtransDemosaic, kRawDemosaicParamXTransMethod, (int)eRawDemosaicXTransVNG);
    EXPECT_TRUE(onXTrans->canRenderOnGpu(query));
}

#endif // NATRON_HAVE_GPU
