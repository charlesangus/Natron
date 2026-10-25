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
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawPrepare.h"

NATRON_NAMESPACE_USING

namespace {
// AMaZE branches on many comparisons of quotients, which Vulkan rounds within 2.5 ulp where the
// CPU rounds exactly, so a photosite on a threshold can take the other branch, and the in-place
// refinements carry that flip to the photosites after it. Bound, as the kernel test's, relative
// to the CPU render's mean colour level.
const double kAmazeMeanBound = 1e-4;
const double kAmazeMaxBound = 1e-4;

// One pixel a thousand, rounded up.
int
outlierBudget(const RectI& window)
{
    return (int)(((std::int64_t)window.width() * window.height() + 999) / 1000);
}

std::string
describe(const RectI& window)
{
    return "AMaZE over " + std::to_string(window.x1) + "," + std::to_string(window.y1) + " " + std::to_string(window.width()) + "x" + std::to_string(window.height());
}
} // namespace

class NativeRawDemosaicAmazeGpuTest
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

    // The size of the prepared image.
    static RectI preparedRoD(const NodePtr& prepare)
    {
        RawPrepare* effect = dynamic_cast<RawPrepare*>(prepare->getEffectInstance().get());
        const std::optional<RawPrepare::Settings> settings = effect ? effect->resolveSettings(kTime, ViewIdx(0)) : std::nullopt;

        return settings ? RectI(0, 0, settings->kept.width(), settings->kept.height()) : RectI();
    }

    // The GPU render of window in four tiles is the window rendered whole, bit for bit.
    void expectTiledEqualsWhole(GpuParityRuntime& runtime,
                                const NodePtr& demosaic,
                                const NodePtr& source,
                                const RectI& full,
                                const std::string& what)
    {
        StagedImageEffect* effect = stagedOf(demosaic);
        ASSERT_TRUE(effect != NULL) << what;
        const int splitX = full.x1 + 37;
        const int splitY = full.y1 + 29;
        const RectI tiles[] = {
            RectI(full.x1, full.y1, splitX, splitY),
            RectI(splitX, full.y1, full.x2, splitY),
            RectI(full.x1, splitY, splitX, full.y2),
            RectI(splitX, splitY, full.x2, full.y2),
        };
        const GpuQuery query = queryFor(4);
        RenderedPlane input;
        ASSERT_TRUE(renderCpu(source, effect->getGpuInputRoI(0, full, RenderScale::identity), &input)) << what;
        const std::vector<GpuParityImage> inputs(1, imageOf(input));
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << what;
        const GpuRunResult whole = runRenderGpu(runtime, *effect, query, inputs, nullptr, full);
        ASSERT_TRUE(whole.ok) << what;

        std::vector<float> assembled(whole.pixels.size(), -1.f);
        for (const RectI& tile : tiles) {
            const GpuRunResult part = runRenderGpu(runtime, *effect, query, inputs, nullptr, tile);
            ASSERT_TRUE(part.ok) << what;
            EXPECT_EQ(0u, part.writtenOutside) << what;
            for (int y = tile.y1; y < tile.y2; ++y) {
                const std::size_t from = (std::size_t)(y - tile.y1) * tile.width() * 4;
                const std::size_t to = ((std::size_t)(y - full.y1) * full.width() + (std::size_t)(tile.x1 - full.x1)) * 4;
                std::memcpy(&assembled[to], &part.pixels[from], (std::size_t)tile.width() * 4 * sizeof(float));
            }
        }
        const ImageDiff diff = compareImages(whole.pixels, assembled, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << " (whole " << whole.pixels[diff.firstMismatchIndex]
                                       << ", tiled " << assembled[diff.firstMismatchIndex] << ")";
    }
};

#ifdef NATRON_HAVE_GPU

// AMaZE on the CR2 fixture: inside the image, across the 128-photosite tile grid, at two corners,
// where the buffer AMaZE runs over ends at the image's edge, and over a single column.
TEST_F(NativeRawDemosaicAmazeGpuTest, MatchesTheCpuOnTheCr2)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr read = createRead(bayerFixture());
    NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
    NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
    ASSERT_TRUE(read && prepare && demosaic);
    setChoice(demosaic, kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerAMaZE);
    ASSERT_FALSE(HasFatalFailure());
    const RectI rod = preparedRoD(prepare);
    ASSERT_GT(rod.width(), 400);
    ASSERT_GT(rod.height(), 400);
    const RectI windows[] = {
        RectI(200, 150, 264, 198),
        RectI(100, 100, 230, 230),
        RectI(0, 0, 40, 36),
        RectI(rod.x2 - 41, rod.y2 - 37, rod.x2, rod.y2),
        RectI(301, 211, 302, 250),
    };
    for (const RectI& window : windows) {
        expectGpuMatchesCpuWithin(*runtime, demosaic, prepare, window, kAmazeMeanBound, kAmazeMaxBound, outlierBudget(window), describe(window));
    }
}

// Four tiles of the window, split off the tile grid, equal the window rendered whole.
TEST_F(NativeRawDemosaicAmazeGpuTest, TiledEqualsFullFrame)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr read = createRead(bayerFixture());
    NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
    NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
    ASSERT_TRUE(read && prepare && demosaic);
    setChoice(demosaic, kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerAMaZE);
    ASSERT_FALSE(HasFatalFailure());
    expectTiledEqualsWhole(*runtime, demosaic, prepare, RectI(150, 120, 246, 200), "AMaZE inside the tile grid");
    expectTiledEqualsWhole(*runtime, demosaic, prepare, RectI(100, 100, 230, 270), "AMaZE across the tile grid");
}

#endif // NATRON_HAVE_GPU
