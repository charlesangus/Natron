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
#include "Engine/Nodes/Raw/RawDenoise.h"
#include "Engine/Nodes/Raw/RawPrepare.h"

NATRON_NAMESPACE_USING

namespace {
// The device's sqrt is not correctly rounded. Every other operation is an add, a subtract, a
// comparison or a multiply by a power of two, kept unfused, and the soft threshold is continuous,
// so a few ulp at the square root stay a few ulp after the wavelets and the square.
const ParityTolerance kDenoiseGpuTolerance = ParityTolerance::transcendental();

struct Strengths {
    const char* name;
    double threshold;
    double all[RawKernels::kRawDenoiseBands];
    // Offsets from the neutral 0.5 of the red, green and blue strengths, the same in every band.
    double colour[3];
};

const Strengths kStrengths[] = {
    { "default", 0.01, { 0.5, 0.5, 0.5, 0.5, 0.5 }, { 0., 0., 0. } },
    { "shaped", 0.05, { 0.9, 0.7, 0.5, 0.3, 0.1 }, { 0.2, -0.1, 0.4 } },
};

std::string
describe(const std::string& what,
         const RectI& window)
{
    return what + " over " + std::to_string(window.x1) + "," + std::to_string(window.y1) + " " + std::to_string(window.width()) + "x" + std::to_string(window.height());
}
} // namespace

class NativeRawDenoiseGpuTest
    : public RawGpuTest {
protected:
    struct Chain {
        NodePtr read;
        NodePtr prepare;
        NodePtr denoise;
    };

    Chain createChain(const RawFixture& fixture)
    {
        Chain c;

        c.read = createRead(fixture);
        c.prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, c.read);
        c.denoise = createAfter(PLUGINID_NATRON_RAWDENOISE, c.prepare);

        return c;
    }

    void configure(const NodePtr& denoise,
                   const Strengths& strengths)
    {
        EffectInstancePtr effect = denoise->getEffectInstance();
        KnobDoublePtr threshold = effect->getKnobByNameAndType<KnobDouble>(kRawDenoiseParamThreshold);
        ASSERT_TRUE(bool(threshold));
        threshold->setValue(strengths.threshold);
        for (int channel = 0; channel < 4; ++channel) {
            for (int band = 0; band < RawKernels::kRawDenoiseBands; ++band) {
                KnobDoublePtr knob = effect->getKnobByNameAndType<KnobDouble>(RawDenoise::strengthKnobName(channel, band));
                ASSERT_TRUE(bool(knob)) << RawDenoise::strengthKnobName(channel, band);
                knob->setValue((channel == 0) ? strengths.all[band] : 0.5 + strengths.colour[channel - 1]);
            }
        }
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

// The Bayer and X-Trans fixtures at two settings: inside the image, at two corners, where the
// wavelets mirror at the image's edges, and over a one-photosite-wide strip.
TEST_F(NativeRawDenoiseGpuTest, MatchesTheCpuOnTheBayerAndXTransFixtures)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const RawFixture* fixture : { &bayerFixture(), &xtransFixture() }) {
        const Chain c = createChain(*fixture);
        ASSERT_TRUE(c.read && c.prepare && c.denoise);
        const RectI rod = preparedRoD(c.prepare);
        ASSERT_GT(rod.width(), 400);
        ASSERT_GT(rod.height(), 400);
        const RectI windows[] = {
            RectI(200, 150, 264, 198),
            RectI(0, 0, 40, 36),
            RectI(rod.x2 - 41, rod.y2 - 37, rod.x2, rod.y2),
            RectI(301, 211, 302, 250),
        };
        for (const Strengths& strengths : kStrengths) {
            configure(c.denoise, strengths);
            ASSERT_FALSE(HasFatalFailure());
            for (const RectI& window : windows) {
                expectGpuMatchesCpu(*runtime, c.denoise, c.prepare, window, kDenoiseGpuTolerance, describe(std::string(fixture->camera) + " " + strengths.name, window));
            }
        }
    }
}

// A window rendered on the GPU in tiles is the window rendered whole, bit for bit: each tile runs
// over the source region its own photosites need, which their values do not depend on.
TEST_F(NativeRawDenoiseGpuTest, TiledGpuRenderEqualsTheFullFrameGpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const RawFixture* fixture : { &bayerFixture(), &xtransFixture() }) {
        const Chain c = createChain(*fixture);
        ASSERT_TRUE(c.read && c.prepare && c.denoise);
        configure(c.denoise, kStrengths[1]);
        ASSERT_FALSE(HasFatalFailure());
        StagedImageEffect* effect = stagedOf(c.denoise);
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
        RenderedPlane input;
        ASSERT_TRUE(renderCpu(c.prepare, effect->getGpuInputRoI(0, effect->getCurrentTime(), effect->getCurrentView(), full, RenderScale::identity), &input));
        const int nComps = (int)input.channels.size();
        const GpuQuery query = queryFor(nComps);
        const std::vector<GpuParityImage> inputs(1, imageOf(input));
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << fixture->camera;
        const GpuRunResult whole = runRenderGpu(*runtime, *effect, query, inputs, nullptr, full);
        ASSERT_TRUE(whole.ok) << fixture->camera;
        ASSERT_EQ((std::size_t)full.area() * nComps, whole.pixels.size()) << fixture->camera;

        std::vector<float> assembled(whole.pixels.size(), -1.f);
        for (const RectI& tile : tiles) {
            const GpuRunResult part = runRenderGpu(*runtime, *effect, query, inputs, nullptr, tile);
            ASSERT_TRUE(part.ok) << fixture->camera;
            EXPECT_EQ(0u, part.writtenOutside) << fixture->camera;
            for (int y = tile.y1; y < tile.y2; ++y) {
                const std::size_t from = (std::size_t)(y - tile.y1) * tile.width() * nComps;
                const std::size_t to = ((std::size_t)(y - full.y1) * full.width() + (std::size_t)(tile.x1 - full.x1)) * nComps;
                std::memcpy(&assembled[to], &part.pixels[from], (std::size_t)tile.width() * nComps * sizeof(float));
            }
        }
        const ImageDiff diff = compareImages(whole.pixels, assembled, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << fixture->camera << ": first at float " << diff.firstMismatchIndex << " (whole " << whole.pixels[diff.firstMismatchIndex]
                                       << ", tiled " << assembled[diff.firstMismatchIndex] << ")";
    }
}

// A threshold of 0 turns the node off: the GPU's result is the source, bit for bit, as the CPU's is.
TEST_F(NativeRawDenoiseGpuTest, ThresholdZeroIsTheIdentity)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const RawFixture* fixture : { &bayerFixture(), &xtransFixture() }) {
        const Chain c = createChain(*fixture);
        ASSERT_TRUE(c.read && c.prepare && c.denoise);
        Strengths off = kStrengths[1];
        off.threshold = 0.;
        configure(c.denoise, off);
        ASSERT_FALSE(HasFatalFailure());
        const RectI window(200, 150, 264, 198);
        const RenderedPlane cpu = expectGpuMatchesCpu(*runtime, c.denoise, c.prepare, window, ParityTolerance::exact(), std::string("threshold 0, ") + fixture->camera);
        RenderedPlane input;
        ASSERT_TRUE(renderCpu(c.prepare, window, &input));
        EXPECT_EQ(input.pixels, cpu.pixels) << fixture->camera;
    }
}

// Linear data has no CFA and passes through unchanged.
TEST_F(NativeRawDenoiseGpuTest, LinearPassThroughMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const Chain c = createChain(linearFixture());
    ASSERT_TRUE(c.read && c.prepare && c.denoise);
    expectGpuMatchesCpu(*runtime, c.denoise, c.prepare, RectI(200, 150, 264, 198), ParityTolerance::exact(), "linear pass-through");
}

#endif // NATRON_HAVE_GPU
