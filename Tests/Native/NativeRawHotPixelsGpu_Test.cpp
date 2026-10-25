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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawHotPixels.h"
#include "Engine/Nodes/Raw/RawPrepare.h"

NATRON_NAMESPACE_USING

namespace {
// A comparison and a copy on both sides, apart from one product the kernel keeps a single rounding.
const ParityTolerance kHotPixelsGpuTolerance = ParityTolerance::exact();

// A prepared window of each fixture, big enough that the neighbours of an injected photosite and
// the buffer's border both fall inside it.
const RectI kWindow(200, 150, 296, 214);

// Far above any neighbour at every strength.
const float kHot = 1000.f;

// Photosites of the window, as offsets from its corner: interior ones, ones within the buffer's
// border, and two same-coloured photosites two apart (one apart without a CFA) that only the
// three-neighbour mode catches.
const std::pair<int, int> kInjected[] = {
    { 20, 20 },
    { 33, 11 },
    { 47, 30 },
    { 9, 37 },
    { 70, 30 },
    { 72, 30 },
    { 71, 30 },
    { 1, 20 },
    { 94, 50 },
    { 30, 63 },
    { 40, 0 },
    { 2, 2 },
    { 93, 61 },
};
// The first four are alone, so that every mode fixes them.
const int kAloneInjected = 4;

// Windows inside kWindow: the whole of it, a tile in its middle, ones at its corners and edges,
// and a single photosite.
const RectI kTiles[] = {
    kWindow,
    RectI(kWindow.x1 + 10, kWindow.y1 + 8, kWindow.x1 + 40, kWindow.y1 + 30),
    RectI(kWindow.x1, kWindow.y1, kWindow.x1 + 7, kWindow.y1 + 5),
    RectI(kWindow.x2 - 9, kWindow.y2 - 6, kWindow.x2, kWindow.y2),
    RectI(kWindow.x1 + 20, kWindow.y1 + 20, kWindow.x1 + 21, kWindow.y1 + 21),
    RectI(kWindow.x1 + 62, kWindow.y1 + 25, kWindow.x1 + 80, kWindow.y1 + 36),
    RectI(kWindow.x1, kWindow.y1 + 14, kWindow.x1 + 3, kWindow.y1 + 40),
};
} // namespace

class NativeRawHotPixelsGpuTest
    : public RawGpuTest {
protected:
    // A source holding the prepared window of fixture with the injected photosites set to kHot, and
    // the node to test after it. *clean is the window as prepared.
    bool createChain(const RawFixture& fixture,
                     NodePtr* source,
                     NodePtr* hotPixels,
                     RenderedPlane* clean)
    {
        NodePtr read = createRead(fixture);
        NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
        EXPECT_TRUE(read && prepare);
        if (!read || !prepare) {
            return false;
        }
        // Through the cache: a bypassing render clears every cache before and after, and each clear
        // decodes the whole raw file again to recompute the Read's metadata.
        std::string error;
        const bool rendered = renderRawTestPlane(prepare, kWindow, clean, &error, false);
        EXPECT_TRUE(rendered) << error;
        if (!rendered) {
            return false;
        }
        NativeEffectBase* prepared = dynamic_cast<NativeEffectBase*>(prepare->getEffectInstance().get());
        EXPECT_TRUE(prepared != NULL);
        if (!prepared) {
            return false;
        }
        const ImageMetadata metadata = prepared->getOutputMetadata(kTime, ViewIdx(0));
        // Left in the project, the Read would decode its file again on every cache clear, which
        // each render of the chain below makes twice.
        prepare->destroyNode(true, false);
        read->destroyNode(true, false);
        const int nComps = (int)clean->channels.size();
        std::vector<float> pixels = clean->pixels;
        int index = 0;
        for (const std::pair<int, int>& p : kInjected) {
            const int c = index++ % nComps;
            pixels[((std::size_t)p.second * (std::size_t)kWindow.width() + (std::size_t)p.first) * nComps + c] = kHot;
        }
        const RectI window = kWindow;
        const RawTestSource::ValueFn value = [pixels, window, nComps](int x, int y, int c) {
            return pixels[((std::size_t)(y - window.y1) * (std::size_t)window.width() + (std::size_t)(x - window.x1)) * nComps + c];
        };
        *source = createNode(QString::fromUtf8(kRawTestSourceID));
        EXPECT_TRUE(bool(*source));
        if (!*source) {
            return false;
        }
        dynamic_cast<RawTestSource*>((*source)->getEffectInstance().get())->setRaw(kWindow, nComps, value, metadata);
        *hotPixels = createAfter(PLUGINID_NATRON_RAWHOTPIXELS, *source);

        return bool(*hotPixels);
    }

    void configure(const NodePtr& node,
                   double strength,
                   bool permissive)
    {
        KnobDoublePtr strengthKnob = node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawHotPixelsParamStrength);
        KnobBoolPtr permissiveKnob = node->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawHotPixelsParamPermissive);
        ASSERT_TRUE(strengthKnob && permissiveKnob);
        strengthKnob->setValue(strength);
        permissiveKnob->setValue(permissive);
    }
};

#ifdef NATRON_HAVE_GPU

// The CR2 (Bayer), RAF (X-Trans) and linear DNG fixtures, at two strengths and in both detection
// modes, over the whole window and over tiles whose neighbours cross their edges.
TEST_F(NativeRawHotPixelsGpuTest, MatchesTheCpuOnEveryFixture)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const RawFixture& fixture : rawFixtures()) {
        NodePtr source;
        NodePtr hotPixels;
        RenderedPlane clean;
        ASSERT_TRUE(createChain(fixture, &source, &hotPixels, &clean));
        for (double strength : { 0.25, 1. }) {
            for (bool permissive : { false, true }) {
                configure(hotPixels, strength, permissive);
                ASSERT_FALSE(HasFatalFailure());
                const std::string what = std::string("RawHotPixels strength ") + std::to_string(strength) + (permissive ? ", 3 neighbours, " : ", 4 neighbours, ") + fixture.camera;
                for (const RectI& tile : kTiles) {
                    const RenderedPlane cpu = expectGpuMatchesCpu(*runtime, hotPixels, source, tile, kHotPixelsGpuTolerance, what);
                    if (tile != kWindow) {
                        continue;
                    }
                    const int nComps = (int)cpu.channels.size();
                    ASSERT_EQ(clean.pixels.size(), cpu.pixels.size());
                    for (int i = 0; i < kAloneInjected; ++i) {
                        const std::pair<int, int>& p = kInjected[i];
                        const std::size_t at = ((std::size_t)p.second * (std::size_t)kWindow.width() + (std::size_t)p.first) * nComps + (std::size_t)(i % nComps);
                        EXPECT_LT(cpu.pixels[at], 10.f) << what << " injected photosite " << i;
                    }
                }
            }
        }
    }
}

// Strength 0 turns the node off: the GPU's result is the source, bit for bit.
TEST_F(NativeRawHotPixelsGpuTest, StrengthZeroIsTheIdentity)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const RawFixture& fixture : rawFixtures()) {
        NodePtr source;
        NodePtr hotPixels;
        RenderedPlane clean;
        ASSERT_TRUE(createChain(fixture, &source, &hotPixels, &clean));
        configure(hotPixels, 0., false);
        ASSERT_FALSE(HasFatalFailure());
        const RenderedPlane cpu = expectGpuMatchesCpu(*runtime, hotPixels, source, kWindow, kHotPixelsGpuTolerance, std::string("strength 0, ") + fixture.camera);
        RenderedPlane input;
        ASSERT_TRUE(renderCpu(source, kWindow, &input));
        EXPECT_EQ(input.pixels, cpu.pixels) << fixture.camera;
    }
}

// Each tile of the GPU render is the same floats as the whole window's.
TEST_F(NativeRawHotPixelsGpuTest, TilesMatchTheFullFrame)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const RawFixture& fixture : rawFixtures()) {
        NodePtr source;
        NodePtr hotPixels;
        RenderedPlane clean;
        ASSERT_TRUE(createChain(fixture, &source, &hotPixels, &clean));
        configure(hotPixels, 1., true);
        ASSERT_FALSE(HasFatalFailure());
        StagedImageEffect* effect = stagedOf(hotPixels);
        ASSERT_TRUE(effect != NULL);
        const int nComps = (int)clean.channels.size();
        const GpuQuery query = queryFor(nComps);
        std::vector<float> full;
        for (const RectI& window : kTiles) {
            RenderedPlane input;
            ASSERT_TRUE(renderCpu(source, effect->getGpuInputRoI(0, window, RenderScale::identity), &input));
            const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, imageOf(input)), nullptr, window);
            ASSERT_TRUE(run.ok) << fixture.camera;
            if (window == kWindow) {
                full = run.pixels;
                continue;
            }
            ASSERT_EQ(full.size(), (std::size_t)kWindow.area() * nComps);
            ASSERT_EQ(run.pixels.size(), (std::size_t)window.area() * nComps);
            for (int y = window.y1; y < window.y2; ++y) {
                for (int x = window.x1; x < window.x2; ++x) {
                    for (int c = 0; c < nComps; ++c) {
                        const float whole = full[((std::size_t)(y - kWindow.y1) * (std::size_t)kWindow.width() + (std::size_t)(x - kWindow.x1)) * nComps + c];
                        const float tiled = run.pixels[((std::size_t)(y - window.y1) * (std::size_t)window.width() + (std::size_t)(x - window.x1)) * nComps + c];
                        ASSERT_EQ(whole, tiled) << fixture.camera << " at " << x << "," << y << " channel " << c;
                    }
                }
            }
        }
    }
}

#endif // NATRON_HAVE_GPU
