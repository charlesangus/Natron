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

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {
// Sums in the same order on both sides; the device rounds the mean's division within 2.5 ulp.
const ParityTolerance kSuperpixelGpuTolerance = ParityTolerance::transcendental();
} // namespace

/**
 * RawDemosaic below full resolution with Zoomed-Out Demosaic at Fast: demosaic_superpixel against
 * RawSuperpixel on the CPU, fed the CPU render of the full-resolution source.
 **/
class NativeRawSuperpixelGpuTest
    : public RawGpuTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        RawGpuTest::SetUp();
        _preference = appPTR->getCurrentSettings()->getKnobByNameAndType<KnobChoice>("zoomedOutDemosaic");
        ASSERT_TRUE(bool(_preference));
        _saved = _preference->getValue();
        _preference->setValue(0);
    }

    virtual void TearDown() OVERRIDE
    {
        if (_preference) {
            _preference->setValue(_saved);
        }
        RawGpuTest::TearDown();
    }

    // The size of the prepared image.
    static RectI preparedRoD(const NodePtr& prepare)
    {
        RawPrepare* effect = dynamic_cast<RawPrepare*>(prepare->getEffectInstance().get());
        const std::optional<RawPrepare::Settings> settings = effect ? effect->resolveSettings(kTime, ViewIdx(0)) : std::nullopt;

        return settings ? RectI(0, 0, settings->kept.width(), settings->kept.height()) : RectI();
    }

#ifdef NATRON_HAVE_GPU
    // demosaic over window at mipmapLevel on the CPU and through renderGpu(), expected within
    // kSuperpixelGpuTolerance.
    void expectSuperpixelGpuMatchesCpu(GpuParityRuntime& runtime,
                                       const NodePtr& demosaic,
                                       const NodePtr& source,
                                       unsigned int mipmapLevel,
                                       const RectI& window,
                                       const std::string& what)
    {
        StagedImageEffect* effect = stagedOf(demosaic);
        ASSERT_TRUE(effect != NULL) << what;

        std::list<ImageLayerDesc> layers;
        demosaic->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);
        std::list<ImageLayerDesc> colour;
        for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
            if (it->isColorLayer()) {
                colour.push_back(*it);
                break;
            }
        }
        ASSERT_FALSE(colour.empty()) << what;
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(demosaic, kTime, ViewIdx(0), mipmapLevel, window, colour, &planes, &error, true)) << what << ": " << error;
        ASSERT_EQ(1u, planes.size()) << what;
        const RenderedPlane& cpu = planes[0];
        ASSERT_TRUE(cpu.window == window) << what;
        ASSERT_EQ(4u, cpu.channels.size()) << what;

        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        const RectI sourceWindow = effect->getGpuInputRoI(0, kTime, ViewIdx(0), window, scale);
        ASSERT_FALSE(sourceWindow.isNull()) << what;
        RenderedPlane input;
        ASSERT_TRUE(renderCpu(source, sourceWindow, &input)) << what;

        GpuQuery query = queryFor(4);
        query.mappedScale = scale;
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << what;
        const GpuRunResult run = runRenderGpu(runtime, *effect, query, std::vector<GpuParityImage>(1, imageOf(input)), nullptr, window);
        ASSERT_TRUE(run.ok) << what;
        EXPECT_EQ(0u, run.writtenOutside) << what;
        ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
        const ImageDiff diff = compareImages(cpu.pixels, run.pixels, kSuperpixelGpuTolerance);
        const std::size_t first = diff.firstMismatchIndex;
        EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " of " << diff.compared << " floats differ, the first at float " << first
                                       << " (cpu " << cpu.pixels[first] << ", gpu " << run.pixels[first] << "), the worst by " << diff.worstAbsolute;
    }
#endif // NATRON_HAVE_GPU

    KnobChoicePtr _preference;
    int _saved = 0;
};

TEST_F(NativeRawSuperpixelGpuTest, FastRendersAtReducedScaleFullDoesNot)
{
    NodePtr read = createRead(bayerFixture());
    NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
    NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
    ASSERT_TRUE(read && prepare && demosaic);
    StagedImageEffect* effect = stagedOf(demosaic);
    ASSERT_TRUE(effect != NULL);
    const RectI window(40, 30, 72, 54);
    const RenderScale half = RenderScale::fromMipmapLevel(1);

    // Two levels' footprint grown by the Bayer period.
    EXPECT_TRUE(RectI(78, 58, 146, 110) == effect->getGpuInputRoI(0, kTime, ViewIdx(0), window, half));
    _preference->setValue(1);
    EXPECT_EQ(EffectInstance::eSupportsNo, demosaic->getEffectInstance()->supportsRenderScaleMaybe());
    _preference->setValue(0);
    EXPECT_EQ(EffectInstance::eSupportsYes, demosaic->getEffectInstance()->supportsRenderScaleMaybe());
}

#ifdef NATRON_HAVE_GPU

// The Bayer superpixel at levels 1 and 2, inside the image and at two corners, where footprints
// end at the image's edge.
TEST_F(NativeRawSuperpixelGpuTest, BayerSuperpixelMatchesTheCpu)
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
    for (unsigned int mip : { 1u, 2u }) {
        const int w = rod.width() >> mip;
        const int h = rod.height() >> mip;
        ASSERT_GT(w, 100);
        ASSERT_GT(h, 100);
        const RectI windows[] = {
            RectI(50, 40, 90, 70),
            RectI(0, 0, 21, 17),
            RectI(w - 19, h - 23, w, h),
        };
        for (const RectI& window : windows) {
            expectSuperpixelGpuMatchesCpu(*runtime, demosaic, prepare, mip, window,
                                          "Bayer level " + std::to_string(mip) + " over " + std::to_string(window.x1) + "," + std::to_string(window.y1));
        }
    }
}

// Bayer photosite colour and monochrome, which the superpixel box-filters.
TEST_F(NativeRawSuperpixelGpuTest, BayerPhotositeModesMatchTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr read = createRead(bayerFixture());
    NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
    NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
    ASSERT_TRUE(read && prepare && demosaic);
    KnobChoicePtr method = demosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamBayerMethod);
    ASSERT_TRUE(bool(method));
    for (int m : { (int)eRawDemosaicBayerPhotositeColor, (int)eRawDemosaicBayerPassthroughMonochrome }) {
        method->setValue(m);
        expectSuperpixelGpuMatchesCpu(*runtime, demosaic, prepare, 1, RectI(50, 40, 90, 70), "Bayer photosite method " + std::to_string(m));
    }
}

// X-Trans, whose 2x2 footprints can lack red or blue and grow to find them.
TEST_F(NativeRawSuperpixelGpuTest, XTransSuperpixelMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr read = createRead(xtransFixture());
    NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
    NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
    ASSERT_TRUE(read && prepare && demosaic);
    for (unsigned int mip : { 1u, 2u }) {
        expectSuperpixelGpuMatchesCpu(*runtime, demosaic, prepare, mip, RectI(50, 40, 90, 70), "X-Trans level " + std::to_string(mip));
        expectSuperpixelGpuMatchesCpu(*runtime, demosaic, prepare, mip, RectI(0, 0, 13, 11), "X-Trans level " + std::to_string(mip) + " at the corner");
    }
}

// Data without a CFA is box-filtered.
TEST_F(NativeRawSuperpixelGpuTest, LinearBoxMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr read = createRead(linearFixture());
    NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
    NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, prepare);
    ASSERT_TRUE(read && prepare && demosaic);
    expectSuperpixelGpuMatchesCpu(*runtime, demosaic, prepare, 1, RectI(50, 40, 90, 70), "linear level 1");
}

#endif // NATRON_HAVE_GPU
