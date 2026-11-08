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
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawKernels.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"
#include "Engine/OverlayImage.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

// Qt's slots macro would rewrite a member of this header.
#undef slots
#include "Engine/Nodes/Raw/RawSegmentation.h"

NATRON_NAMESPACE_USING

namespace {
const RectI kRod(0, 0, 150, 120);
const float kCoeffs[3] = { 2.f, 1.f, 1.5f };

// Indexed by the colours marked: bit 0 red, bit 1 green, bit 2 blue.
const char* const kPlanes[8] = {
    nullptr,
    kRawHighlightsMaskPlaneRed,
    kRawHighlightsMaskPlaneGreen,
    kRawHighlightsMaskPlaneYellow,
    kRawHighlightsMaskPlaneBlue,
    kRawHighlightsMaskPlaneMagenta,
    kRawHighlightsMaskPlaneCyan,
    kRawHighlightsMaskPlaneWhite,
};

// A disc where every colour clipped, a rim around it where only green did, and a second disc where
// only red did, in a flat reddish scene the segmentation finds candidates in.
RawTestSource::ValueFn
clippedScene(const RawLayout& layout)
{
    return [layout](int x, int y, int) {
        const int colour = layout.colourAt(x, y);
        const float scene[3] = { 3.f, 1.f, 1.f };
        float raw = 0.12f * scene[colour];
        const float d = std::hypot((float)x - 45.f, (float)y - 60.f);
        if (d < 28.f) {
            raw = 1.05f;
        } else if ((d < 32.f) && (colour == 1)) {
            raw = 1.05f;
        } else if ((std::hypot((float)x - 110.f, (float)y - 60.f) < 20.f) && (colour == 0)) {
            raw = 1.05f;
        }

        return raw * kCoeffs[colour];
    };
}

ImageMetadata
whiteBalancedMetadata(const std::string& pattern)
{
    ImageMetadata metadata = rawTestMetadata(pattern, 0, 0, eRawStagePrepared);

    RawMetadata::setWhiteLevel(&metadata, 1.);
    RawMetadata::setWbApplied(&metadata, { kCoeffs[0], kCoeffs[1], kCoeffs[2] });
    RawMetadata::setProcessedMaximum(&metadata, { kCoeffs[0], kCoeffs[1], kCoeffs[2] });

    return metadata;
}

float
planeValue(const OverlayImageFrame& frame,
           const char* name,
           int x,
           int y)
{
    for (const OverlayImagePlane& plane : frame.planes) {
        if (plane.name != name) {
            continue;
        }
        for (const std::shared_ptr<const OverlayImageTile>& tile : plane.tiles) {
            const RectI& b = tile->bounds;
            if ((x >= b.x1) && (x < b.x2) && (y >= b.y1) && (y < b.y2)) {
                return tile->values[(std::size_t)(y - b.y1) * (std::size_t)b.width() + (std::size_t)(x - b.x1)] / 255.f;
            }
        }
    }

    return 0.f;
}

float
anyPlaneValue(const OverlayImageFrame& frame,
              int x,
              int y)
{
    float value = 0.f;

    for (int i = 1; i < 8; ++i) {
        value = std::max(value, planeValue(frame, kPlanes[i], x, y));
    }

    return value;
}
} // namespace

class NativeRawHighlightsMaskViewsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
    }

    virtual void TearDown() OVERRIDE
    {
        appPTR->clearAllCaches();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createChain(RawHighlightsModeEnum mode,
                        const RawTestSource::ValueFn& scene,
                        const ImageMetadata& metadata,
                        int nComps = 1)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(kRod, nComps, scene, metadata);
        connectNodes(source, node, 0, true);
        choice(node, kRawHighlightsParamMode)->setValue((int)mode);

        return node;
    }

    NodePtr createChain(RawHighlightsModeEnum mode)
    {
        const ImageMetadata metadata = whiteBalancedMetadata("RGGB");

        return createChain(mode, clippedScene(*RawLayout::fromMetadata(metadata)), metadata);
    }

    static KnobChoicePtr choice(const NodePtr& node,
                                const char* name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(name);
    }

    static OverlayImageFrameConstPtr pass(const NodePtr& node,
                                          const RectI& window = kRod)
    {
        OverlayImageRequest request;

        request.time = 1.;
        request.view = ViewIdx(0);
        request.mipmapLevel = 0;
        request.draftMode = false;
        request.visibleCanonical = window;
        EXPECT_TRUE(node->getEffectInstance()->renderOverlayImages(request, AbortableRenderInfo::create(false, 0)));

        return node->getOverlayImageFrame();
    }

    static OverlayImageFrameConstPtr showView(const NodePtr& node,
                                              RawHighlightsMaskViewEnum view,
                                              const RectI& window = kRod)
    {
        choice(node, kRawHighlightsParamMaskView)->setValue((int)view);

        return pass(node, window);
    }
};

// darktable's process_visualize() and demosaic_box3(): each colour's clipped share of a pixel's 3x3
// photosites, drawn in the mix of the colours that have any.
TEST_F(NativeRawHighlightsMaskViewsTest, ClippedMarksEachColourOverThreeByThreePhotosites)
{
    const ImageMetadata metadata = whiteBalancedMetadata("RGGB");
    const RawLayout layout = *RawLayout::fromMetadata(metadata);
    const RawTestSource::ValueFn scene = clippedScene(layout);
    NodePtr node = createChain(eRawHighlightsModeClip, scene, metadata);
    ASSERT_TRUE(bool(node));

    const OverlayImageFrameConstPtr frame = showView(node, eRawHighlightsMaskViewClipped);
    ASSERT_TRUE(bool(frame));

    for (int y = kRod.y1; y < kRod.y2; ++y) {
        for (int x = kRod.x1; x < kRod.x2; ++x) {
            float sum[3] = { 0.f, 0.f, 0.f };
            int count[3] = { 0, 0, 0 };
            for (int ny = std::max(kRod.y1, y - 1); ny <= std::min(kRod.y2 - 1, y + 1); ++ny) {
                for (int nx = std::max(kRod.x1, x - 1); nx <= std::min(kRod.x2 - 1, x + 1); ++nx) {
                    const int colour = layout.colourAt(nx, ny);
                    sum[colour] += (scene(nx, ny, 0) >= kCoeffs[colour]) ? 1.f : 0.f;
                    ++count[colour];
                }
            }
            int marked = 0;
            float coverage = 0.f;
            for (int c = 0; c < 3; ++c) {
                if (sum[c] > 0.f) {
                    marked |= 1 << c;
                    coverage = std::max(coverage, sum[c] / (float)count[c]);
                }
            }
            for (int i = 1; i < 8; ++i) {
                ASSERT_FLOAT_EQ((i == marked) ? overlayImageCoverageByte(coverage) / 255.f : 0.f, planeValue(*frame, kPlanes[i], x, y)) << kPlanes[i] << " at " << x << "," << y;
            }
        }
    }
    EXPECT_FLOAT_EQ(1.f, planeValue(*frame, kRawHighlightsMaskPlaneWhite, 45, 60));
    EXPECT_FLOAT_EQ(1.f, planeValue(*frame, kRawHighlightsMaskPlaneRed, 110, 60));
}

// process_visualize() takes the method's magic factor: 0.99 of the white level clips under Inpaint
// Opposed's 0.987 and not under Clip Highlights' 1.
TEST_F(NativeRawHighlightsMaskViewsTest, ClippedFollowsTheMethodsThreshold)
{
    const ImageMetadata metadata = whiteBalancedMetadata("RGGB");
    const RawLayout layout = *RawLayout::fromMetadata(metadata);
    const RawTestSource::ValueFn scene = [layout](int x, int y, int) {
        return ((std::hypot((float)x - 75.f, (float)y - 60.f) < 20.f) ? 0.99f : 0.2f) * kCoeffs[(int)layout.colourAt(x, y)];
    };

    NodePtr clip = createChain(eRawHighlightsModeClip, scene, metadata);
    ASSERT_TRUE(bool(clip));
    OverlayImageFrameConstPtr frame = showView(clip, eRawHighlightsMaskViewClipped);
    ASSERT_TRUE(bool(frame));
    EXPECT_TRUE(frame->planes.empty());

    NodePtr opposed = createChain(eRawHighlightsModeOpposed, scene, metadata);
    ASSERT_TRUE(bool(opposed));
    frame = showView(opposed, eRawHighlightsMaskViewClipped);
    ASSERT_TRUE(bool(frame));
    EXPECT_FLOAT_EQ(1.f, planeValue(*frame, kRawHighlightsMaskPlaneWhite, 75, 60));
    EXPECT_FLOAT_EQ(0.f, anyPlaneValue(*frame, 10, 10));
}

TEST_F(NativeRawHighlightsMaskViewsTest, ClippedOnLinearDataMarksEachChannel)
{
    const RawTestSource::ValueFn scene = [](int x, int, int c) {
        if (c == 0) {
            return (x < 40) ? 1.5f : 0.5f;
        }
        if (c == 1) {
            return (x >= 110) ? 1.5f : 0.5f;
        }

        return 0.5f;
    };
    NodePtr node = createChain(eRawHighlightsModeClip, scene, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared), 3);
    ASSERT_TRUE(bool(node));

    const OverlayImageFrameConstPtr frame = showView(node, eRawHighlightsMaskViewClipped);
    ASSERT_TRUE(bool(frame));
    EXPECT_FLOAT_EQ(1.f, planeValue(*frame, kRawHighlightsMaskPlaneRed, 20, 60));
    EXPECT_FLOAT_EQ(1.f, planeValue(*frame, kRawHighlightsMaskPlaneGreen, 130, 60));
    EXPECT_FLOAT_EQ(0.f, anyPlaneValue(*frame, 75, 60));
}

TEST_F(NativeRawHighlightsMaskViewsTest, NothingClippedAddsNoPlanes)
{
    const ImageMetadata metadata = whiteBalancedMetadata("RGGB");
    const RawLayout layout = *RawLayout::fromMetadata(metadata);
    NodePtr node = createChain(eRawHighlightsModeClip, [layout](int x, int y, int) { return 0.3f * kCoeffs[(int)layout.colourAt(x, y)]; }, metadata);
    ASSERT_TRUE(bool(node));

    const OverlayImageFrameConstPtr frame = showView(node, eRawHighlightsMaskViewClipped);
    ASSERT_TRUE(bool(frame));
    EXPECT_TRUE(frame->planes.empty());
}

TEST_F(NativeRawHighlightsMaskViewsTest, TilesMatchTheFullFrame)
{
    const RectI windows[] = {
        RectI(30, 40, 70, 85),
        RectI(0, 0, 9, 7),
        RectI(45, 60, 46, 61),
        RectI(100, 50, 151, 70),
    };
    const RawHighlightsMaskViewEnum views[] = { eRawHighlightsMaskViewClipped, eRawHighlightsMaskViewCombine, eRawHighlightsMaskViewCandidating };

    NodePtr node = createChain(eRawHighlightsModeSegments);
    ASSERT_TRUE(bool(node));
    for (const RawHighlightsMaskViewEnum view : views) {
        const OverlayImageFrameConstPtr full = showView(node, view);
        ASSERT_TRUE(bool(full));
        for (const RectI& window : windows) {
            const OverlayImageFrameConstPtr tile = pass(node, window);
            ASSERT_TRUE(bool(tile));
            const RectI inside = window.intersect(kRod);
            for (int y = inside.y1; y < inside.y2; ++y) {
                for (int x = inside.x1; x < inside.x2; ++x) {
                    for (int i = 1; i < 8; ++i) {
                        ASSERT_EQ(planeValue(*full, kPlanes[i], x, y), planeValue(*tile, kPlanes[i], x, y)) << "view " << (int)view << " " << kPlanes[i] << " at " << x << "," << y;
                    }
                }
            }
        }
    }
}

TEST_F(NativeRawHighlightsMaskViewsTest, CombineShowsTheSegmentsOfEachColour)
{
    NodePtr node = createChain(eRawHighlightsModeSegments);
    ASSERT_TRUE(bool(node));

    const OverlayImageFrameConstPtr frame = showView(node, eRawHighlightsMaskViewCombine);
    ASSERT_TRUE(bool(frame));
    EXPECT_GT(planeValue(*frame, kRawHighlightsMaskPlaneWhite, 45, 60), 0.f);
    EXPECT_GT(planeValue(*frame, kRawHighlightsMaskPlaneRed, 110, 60), 0.f);
    EXPECT_FLOAT_EQ(0.f, anyPlaneValue(*frame, 140, 10));
    EXPECT_FLOAT_EQ(0.f, anyPlaneValue(*frame, kRod.x1, kRod.y1));
}

TEST_F(NativeRawHighlightsMaskViewsTest, CandidatingIsWithinTheSegments)
{
    NodePtr node = createChain(eRawHighlightsModeSegments);
    ASSERT_TRUE(bool(node));

    const OverlayImageFrameConstPtr combine = showView(node, eRawHighlightsMaskViewCombine);
    const OverlayImageFrameConstPtr candidating = showView(node, eRawHighlightsMaskViewCandidating);
    ASSERT_TRUE(combine && candidating);
    bool any = false;
    for (int y = kRod.y1; y < kRod.y2; ++y) {
        for (int x = kRod.x1; x < kRod.x2; ++x) {
            if (anyPlaneValue(*candidating, x, y) > 0.f) {
                any = true;
                ASSERT_GT(anyPlaneValue(*combine, x, y), 0.f) << x << "," << y;
            }
        }
    }
    EXPECT_TRUE(any);
}

// Only all-clipped segments have a strength, so the disc where red alone clipped shows nothing.
TEST_F(NativeRawHighlightsMaskViewsTest, StrengthShowsOnTheAllClippedSegments)
{
    NodePtr node = createChain(eRawHighlightsModeSegments);
    ASSERT_TRUE(bool(node));
    choice(node, kRawHighlightsParamRecovery)->setValue(1);
    node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamStrength)->setValue(0.6);

    const OverlayImageFrameConstPtr frame = showView(node, eRawHighlightsMaskViewStrength);
    ASSERT_TRUE(bool(frame));
    float inside = 0.f;
    for (int y = 50; y < 70; ++y) {
        for (int x = 35; x < 55; ++x) {
            const float value = planeValue(*frame, kRawHighlightsMaskPlaneStrength, x, y);
            EXPECT_LE(value, 1.f);
            inside = std::max(inside, value);
        }
    }
    EXPECT_GT(inside, 0.f);
    EXPECT_FLOAT_EQ(0.f, planeValue(*frame, kRawHighlightsMaskPlaneStrength, 110, 60));
    EXPECT_FLOAT_EQ(0.f, planeValue(*frame, kRawHighlightsMaskPlaneStrength, 140, 10));
}

TEST_F(NativeRawHighlightsMaskViewsTest, SegmentViewsNeedSegmentationBased)
{
    NodePtr node = createChain(eRawHighlightsModeOpposed);
    ASSERT_TRUE(bool(node));

    for (const RawHighlightsMaskViewEnum view : { eRawHighlightsMaskViewCombine, eRawHighlightsMaskViewCandidating, eRawHighlightsMaskViewStrength }) {
        const OverlayImageFrameConstPtr frame = showView(node, view);
        ASSERT_TRUE(bool(frame));
        EXPECT_TRUE(frame->planes.empty());
    }
}

// Unclipped is an identity, and an identity's render action never runs.
TEST_F(NativeRawHighlightsMaskViewsTest, UnclippedHasNoMasks)
{
    NodePtr node = createChain(eRawHighlightsModeUnclipped);
    ASSERT_TRUE(bool(node));

    const OverlayImageFrameConstPtr frame = showView(node, eRawHighlightsMaskViewClipped);
    ASSERT_TRUE(bool(frame));
    EXPECT_TRUE(frame->planes.empty());
}

TEST_F(NativeRawHighlightsMaskViewsTest, ChangingTheMethodTurnsTheViewOff)
{
    NodePtr node = createChain(eRawHighlightsModeOpposed);
    ASSERT_TRUE(bool(node));
    const KnobChoicePtr view = choice(node, kRawHighlightsParamMaskView);

    view->setValue((int)eRawHighlightsMaskViewClipped);
    choice(node, kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeClip);
    EXPECT_EQ((int)eRawHighlightsMaskViewOff, view->getValue());
}

TEST_F(NativeRawHighlightsMaskViewsTest, TurningRebuildOffLeavesTheStrengthView)
{
    NodePtr node = createChain(eRawHighlightsModeSegments);
    ASSERT_TRUE(bool(node));
    const KnobChoicePtr view = choice(node, kRawHighlightsParamMaskView);
    const KnobChoicePtr recovery = choice(node, kRawHighlightsParamRecovery);

    recovery->setValue(1);
    view->setValue((int)eRawHighlightsMaskViewStrength);
    recovery->setValue(2);
    EXPECT_EQ((int)eRawHighlightsMaskViewStrength, view->getValue());
    recovery->setValue(0);
    EXPECT_EQ((int)eRawHighlightsMaskViewOff, view->getValue());
}

// The view stays out of the hash and of the pixels, also when the pass fills the cache.
TEST_F(NativeRawHighlightsMaskViewsTest, TheViewLeavesTheRenderAlone)
{
    for (const RawHighlightsModeEnum mode : { eRawHighlightsModeClip, eRawHighlightsModeSegments }) {
        NodePtr node = createChain(mode);
        ASSERT_TRUE(bool(node));
        RenderedPlane before;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, kRod, &before, &error)) << error;
        const U64 hash = node->getHashValue();

        // Otherwise the render below reads the image from before the view went on.
        appPTR->clearAllCaches();
        const OverlayImageFrameConstPtr frame = showView(node, eRawHighlightsMaskViewClipped);
        ASSERT_TRUE(bool(frame));
        EXPECT_FALSE(frame->planes.empty());
        EXPECT_EQ(hash, node->getHashValue());

        RenderedPlane afterPass;
        ASSERT_TRUE(renderRawTestPlane(node, kRod, &afterPass, &error, false /*byPassCache*/)) << error;
        EXPECT_EQ(before.pixels, afterPass.pixels);

        RenderedPlane rendered;
        ASSERT_TRUE(renderRawTestPlane(node, kRod, &rendered, &error, true /*byPassCache*/)) << error;
        EXPECT_EQ(before.pixels, rendered.pixels);
    }
}

TEST_F(NativeRawHighlightsMaskViewsTest, TheMaskGoesWithTheView)
{
    NodePtr node = createChain(eRawHighlightsModeSegments);
    ASSERT_TRUE(bool(node));
    FrameComputedCounter counter;

    ASSERT_TRUE(bool(showView(node, eRawHighlightsMaskViewCandidating)));
    const int computed = counter.count(kRawHighlightsFrameSegments);
    ASSERT_GT(computed, 0);
    ASSERT_TRUE(bool(pass(node)));
    EXPECT_EQ(computed, counter.count(kRawHighlightsFrameSegments));

    choice(node, kRawHighlightsParamMaskView)->setValue((int)eRawHighlightsMaskViewOff);
    node->getEffectInstance()->refreshOverlayImages();

    ASSERT_TRUE(bool(showView(node, eRawHighlightsMaskViewCandidating)));
    EXPECT_EQ(computed + 1, counter.count(kRawHighlightsFrameSegments));
}

// A display aid must not change what a main render asks of its source.
TEST_F(NativeRawHighlightsMaskViewsTest, TheClippedViewLeavesTheMainRenderRoIAlone)
{
    NodePtr node = createChain(eRawHighlightsModeClip);
    ASSERT_TRUE(bool(node));
    const EffectInstancePtr effect = node->getEffectInstance();
    const RectD window(10., 10., 20., 20.);
    const auto regionOfInput = [&]() {
        RoIMap regions;
        effect->getRegionsOfInterest_public(1., RenderScale::identity, RectD(kRod.x1, kRod.y1, kRod.x2, kRod.y2), window, ViewIdx(0), &regions);

        return regions;
    };

    const RoIMap off = regionOfInput();
    choice(node, kRawHighlightsParamMaskView)->setValue((int)eRawHighlightsMaskViewClipped);
    const RoIMap on = regionOfInput();
    EXPECT_EQ(off.size(), on.size());
    for (const auto& [input, region] : off) {
        const RoIMap::const_iterator found = on.find(input);
        ASSERT_TRUE(found != on.end());
        EXPECT_EQ(region.x1, found->second.x1);
        EXPECT_EQ(region.x2, found->second.x2);
        EXPECT_EQ(region.y1, found->second.y1);
        EXPECT_EQ(region.y2, found->second.y2);
    }
}

TEST_F(NativeRawHighlightsMaskViewsTest, KernelMasksLeaveTheReconstructionAlone)
{
    const int width = kRod.width();
    const int height = kRod.height();
    const std::optional<RawLayout> layout = RawLayout::fromPattern("RGGB");
    ASSERT_TRUE(bool(layout));
    const std::uint32_t filters = RawNodeSupport::bayerFiltersAt(*layout, kRod);
    const std::uint8_t xtrans[6][6] = {};
    const RawKernels::ParallelFor serial(&RawKernels::serialFor);
    const RawTestSource::ValueFn scene = clippedScene(*layout);
    const auto fewClipped = [layout](int x, int y, int) {
        const float raw = ((x == 40) && (y >= 40) && (y < 43)) ? 1.05f : 0.2f;

        return raw * kCoeffs[(int)layout->colourAt(x, y)];
    };

    RawKernels::SegmentsParams params;
    params.recovery = RawKernels::eSegmentsRecoverySmall;
    params.strength = 0.6f;
    params.noiseLevel = 0.1f;
    std::copy(kCoeffs, kCoeffs + 3, params.coeffs);

    for (const RawTestSource::ValueFn& fn : { scene, RawTestSource::ValueFn(fewClipped) }) {
        std::vector<float> in((std::size_t)width * height);
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                in[(std::size_t)row * width + col] = fn(kRod.x1 + col, kRod.y2 - 1 - row, 0);
            }
        }
        std::vector<float> plain = in;
        RawKernels::segmentsHighlights(in.data(), width, 1, plain.data(), width, height, filters, xtrans, params, serial);
        for (const RawKernels::SegmentsMaskEnum view : { RawKernels::eSegmentsMaskCombine, RawKernels::eSegmentsMaskCandidating, RawKernels::eSegmentsMaskStrength }) {
            std::vector<float> out = in;
            std::vector<float> mask((std::size_t)width * height, 7.f);
            RawKernels::segmentsHighlights(in.data(), width, 1, out.data(), width, height, filters, xtrans, params, serial, view, mask.data());
            EXPECT_EQ(plain, out) << "mask " << (int)view;
            for (int col = 0; col < width; ++col) {
                ASSERT_EQ(0.f, mask[col]);
                ASSERT_EQ(0.f, mask[(std::size_t)(height - 1) * width + col]);
            }
            for (int row = 0; row < height; ++row) {
                ASSERT_EQ(0.f, mask[(std::size_t)row * width]);
                ASSERT_EQ(0.f, mask[(std::size_t)row * width + width - 1]);
            }
        }
    }
}
