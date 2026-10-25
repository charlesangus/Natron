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
// VNG compares each gradient with a threshold over values the linear pass divided, which Vulkan
// rounds within 2.5 ulp: a pixel whose gradient sits on the threshold can take the other set of
// directions. Everywhere else the two agree to a few ulp.
const double kVngMeanBound = 1e-4;
const double kVngMaxBound = 1e-4;

// Markesteijn compares green smoothness and counts homogeneous neighbours over values divided by
// 3 or by a count, which Vulkan rounds within 2.5 ulp: a pixel on a threshold can average other
// directions.
const double kMarkesteijnMeanBound = 1e-4;
const double kMarkesteijnMaxBound = 1e-4;

// One pixel a thousand, rounded up.
int
outlierBudget(const RectI& window)
{
    return (int)(((std::int64_t)window.width() * window.height() + 999) / 1000);
}

const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

void
texturedScene(int x,
              int y,
              float rgb[3])
{
    const float r2 = ((float)x - 9.f) * ((float)x - 9.f) + ((float)y - 8.f) * ((float)y - 8.f);

    rgb[0] = 0.4f + 0.3f * std::cos(r2 * 0.05f);
    rgb[1] = 0.5f + 0.25f * std::sin((float)x * 0.7f) * std::cos((float)y * 0.5f);
    rgb[2] = 0.3f + 0.002f * (float)(x * y % 97);
}

std::string
describe(const std::string& what,
         const RectI& window)
{
    return what + " over " + std::to_string(window.x1) + "," + std::to_string(window.y1) + " " + std::to_string(window.width()) + "x" + std::to_string(window.height());
}
} // namespace

class NativeRawDemosaicXTransGpuTest
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

    // A RawTestSource of the scene seen through pattern at (originX, originY) over rod, a flat
    // grey of flatGrey instead when it is not negative, and a RawDemosaic after it.
    bool createMosaic(const RectI& rod,
                      const std::string& pattern,
                      int originX,
                      int originY,
                      NodePtr* source,
                      NodePtr* demosaic,
                      float flatGrey = -1.f)
    {
        const ImageMetadata metadata = rawTestMetadata(pattern, originX, originY, eRawStagePrepared);
        const std::optional<RawLayout> layout = RawLayout::fromMetadata(metadata);
        EXPECT_TRUE(bool(layout)) << pattern;
        if (!layout) {
            return false;
        }
        *source = createNode(QString::fromUtf8(kRawTestSourceID));
        EXPECT_TRUE(bool(*source));
        if (!*source) {
            return false;
        }
        const RawLayout cfa = *layout;
        dynamic_cast<RawTestSource*>((*source)->getEffectInstance().get())->setRaw(rod, 1, [cfa, flatGrey](int x, int y, int) {
            if (flatGrey >= 0.f) {
                return flatGrey;
            }
            float rgb[3];
            texturedScene(x, y, rgb);

            return rgb[cfa.colourAt(x, y)]; }, metadata);
        *demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, *source);

        return bool(*demosaic);
    }

    // The GPU render of window, RGBA from its bottom row.
    bool renderGpu(GpuParityRuntime& runtime,
                   const NodePtr& demosaic,
                   const NodePtr& source,
                   const RectI& window,
                   std::vector<float>* pixels)
    {
        StagedImageEffect* effect = stagedOf(demosaic);
        EXPECT_TRUE(effect != NULL);
        if (!effect) {
            return false;
        }
        const GpuQuery query = queryFor(4);
        RenderedPlane input;
        EXPECT_TRUE(renderCpu(source, effect->getGpuInputRoI(0, window, RenderScale::identity), &input));
        EXPECT_TRUE(effect->canRenderOnGpu(query));
        const std::vector<GpuParityImage> inputs(1, imageOf(input));
        GpuRunResult result = runRenderGpu(runtime, *effect, query, inputs, nullptr, window);
        EXPECT_TRUE(result.ok);
        if (!result.ok) {
            return false;
        }
        *pixels = std::move(result.pixels);

        return true;
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

// X-Trans VNG on the RAF fixture and VNG4 on the CR2 fixture: inside the image, at two corners,
// where the buffer VNG runs over ends at the image's edge, and over a single column.
TEST_F(NativeRawDemosaicXTransGpuTest, VngMatchesTheCpuOnBothFixtures)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    struct Fixture {
        const char* name;
        RawFixture fixture;
        const char* knob;
        int method;
    };
    const Fixture fixtures[] = {
        { "X-Trans VNG", xtransFixture(), kRawDemosaicParamXTransMethod, (int)eRawDemosaicXTransVNG },
        { "VNG4", bayerFixture(), kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerVNG4 },
    };

    for (const Fixture& fixture : fixtures) {
        NodePtr read = createRead(fixture.fixture);
        NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
        NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
        ASSERT_TRUE(read && prepare && demosaic) << fixture.name;
        setChoice(demosaic, fixture.knob, fixture.method);
        ASSERT_FALSE(HasFatalFailure());
        const RectI rod = preparedRoD(prepare);
        ASSERT_GT(rod.width(), 400) << fixture.name;
        ASSERT_GT(rod.height(), 400) << fixture.name;
        const RectI windows[] = {
            RectI(200, 150, 264, 198),
            RectI(0, 0, 40, 36),
            RectI(rod.x2 - 41, rod.y2 - 37, rod.x2, rod.y2),
            RectI(301, 211, 302, 250),
        };
        for (const RectI& window : windows) {
            expectGpuMatchesCpuWithin(*runtime, demosaic, prepare, window, kVngMeanBound, kVngMaxBound, outlierBudget(window), describe(fixture.name, window));
        }
        expectTiledEqualsWhole(*runtime, demosaic, prepare, RectI(150, 120, 246, 200), fixture.name);
    }
}

// Markesteijn 1- and 3-pass on the RAF fixture: inside the image, at two corners, where the
// buffer the tiles cover ends at the image's edge and the mirrored fill and VNG's border take
// over, and over a single column.
TEST_F(NativeRawDemosaicXTransGpuTest, MarkesteijnMatchesTheCpuOnTheRaf)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const struct {
        const char* name;
        int method;
    } methods[] = {
        { "Markesteijn 1-pass", (int)eRawDemosaicXTransMarkesteijn1 },
        { "Markesteijn 3-pass", (int)eRawDemosaicXTransMarkesteijn3 },
    };

    for (const auto& method : methods) {
        NodePtr read = createRead(xtransFixture());
        NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
        NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
        ASSERT_TRUE(read && prepare && demosaic) << method.name;
        setChoice(demosaic, kRawDemosaicParamXTransMethod, method.method);
        ASSERT_FALSE(HasFatalFailure());
        const RectI rod = preparedRoD(prepare);
        ASSERT_GT(rod.width(), 400) << method.name;
        ASSERT_GT(rod.height(), 400) << method.name;
        const RectI windows[] = {
            RectI(200, 150, 264, 198),
            RectI(0, 0, 40, 36),
            RectI(rod.x2 - 41, rod.y2 - 37, rod.x2, rod.y2),
            RectI(301, 211, 302, 250),
        };
        for (const RectI& window : windows) {
            expectGpuMatchesCpuWithin(*runtime, demosaic, prepare, window, kMarkesteijnMeanBound, kMarkesteijnMaxBound, outlierBudget(window), describe(method.name, window));
        }
        expectTiledEqualsWhole(*runtime, demosaic, prepare, RectI(150, 120, 246, 200), method.name);
    }
}

// A flat grey mosaic demosaics to the same grey at each of the 36 positions of the X-Trans
// pattern, border included, on the GPU as on the CPU.
TEST_F(NativeRawDemosaicXTransGpuTest, MarkesteijnKeepsAFlatGreyNeutralAtEveryPhase)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const float grey = 0.35f;
    const RectI rod(0, 0, 61, 53);
    for (const int method : { (int)eRawDemosaicXTransMarkesteijn1, (int)eRawDemosaicXTransMarkesteijn3 }) {
        for (int originY = 0; originY < 6; ++originY) {
            for (int originX = 0; originX < 6; ++originX) {
                const std::string what = std::string(method == (int)eRawDemosaicXTransMarkesteijn1 ? "1-pass" : "3-pass") + " at phase " + std::to_string(originX) + "," + std::to_string(originY);
                NodePtr source;
                NodePtr demosaic;
                ASSERT_TRUE(createMosaic(rod, kXTransPattern, originX, originY, &source, &demosaic, grey)) << what;
                setChoice(demosaic, kRawDemosaicParamXTransMethod, method);
                ASSERT_FALSE(HasFatalFailure());
                std::vector<float> pixels;
                ASSERT_TRUE(renderGpu(*runtime, demosaic, source, rod, &pixels)) << what;
                ASSERT_EQ((std::size_t)rod.width() * rod.height() * 4, pixels.size()) << what;
                std::size_t off = 0;
                for (std::size_t p = 0; p < pixels.size(); p += 4) {
                    for (int c = 0; c < 3; ++c) {
                        off += (std::fabs(pixels[p + c] - grey) <= 1e-6f) ? 0 : 1;
                    }
                }
                EXPECT_EQ(0u, off) << what;
                expectGpuMatchesCpuWithin(*runtime, demosaic, source, rod, kMarkesteijnMeanBound, kMarkesteijnMaxBound, outlierBudget(rod), what);
            }
        }
    }
}

// Below 33 photosites AMaZE, and below 19 Markesteijn 3-pass, fall back on VNG, which then runs
// on the GPU, over the whole image and part of it, at origins that shift the pattern.
TEST_F(NativeRawDemosaicXTransGpuTest, SmallImagesFallBackOnTheGpuVng)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    struct Crop {
        const char* name;
        RectI rod;
        std::string pattern;
        int originX;
        int originY;
        const char* knob;
        int method;
    };
    const Crop crops[] = {
        { "AMaZE on RGGB", RectI(0, 0, 30, 28), "RGGB", 0, 0, kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerAMaZE },
        { "AMaZE on GBRG", RectI(3, 5, 35, 33), "GBRG", 1, 0, kRawDemosaicParamBayerMethod, (int)eRawDemosaicBayerAMaZE },
        { "Markesteijn 3-pass", RectI(0, 0, 18, 17), kXTransPattern, 0, 0, kRawDemosaicParamXTransMethod, (int)eRawDemosaicXTransMarkesteijn3 },
        { "Markesteijn 3-pass, shifted", RectI(2, 1, 20, 19), kXTransPattern, 4, 1, kRawDemosaicParamXTransMethod, (int)eRawDemosaicXTransMarkesteijn3 },
    };

    for (const Crop& crop : crops) {
        NodePtr source;
        NodePtr demosaic;
        ASSERT_TRUE(createMosaic(crop.rod, crop.pattern, crop.originX, crop.originY, &source, &demosaic)) << crop.name;
        setChoice(demosaic, crop.knob, crop.method);
        ASSERT_FALSE(HasFatalFailure());
        const RectI inner(crop.rod.x1 + 3, crop.rod.y1 + 2, crop.rod.x2 - 4, crop.rod.y2 - 5);
        for (const RectI& window : { crop.rod, inner }) {
            expectGpuMatchesCpuWithin(*runtime, demosaic, source, window, kVngMeanBound, kVngMaxBound, outlierBudget(window), describe(crop.name, window));
        }
    }
}

#endif // NATRON_HAVE_GPU
