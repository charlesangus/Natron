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

#include "RawReferenceSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {
// Sets the Zoomed-Out Demosaic preference for its lifetime, Full when full, and restores it.
class ZoomedOutDemosaicScope {
public:
    explicit ZoomedOutDemosaicScope(bool full)
        : _knob(appPTR->getCurrentSettings()->getKnobByNameAndType<KnobChoice>("zoomedOutDemosaic"))
        , _saved(_knob ? _knob->getValue() : 0)
    {
        EXPECT_TRUE(bool(_knob));
        if (_knob) {
            _knob->setValue(full ? 1 : 0);
        }
    }

    ~ZoomedOutDemosaicScope()
    {
        if (_knob) {
            _knob->setValue(_saved);
        }
    }

private:
    KnobChoicePtr _knob;
    int _saved;
};

struct PlaneDiff {
    double level = 0.;
    double relMean = 0.;
    double relMax = 0.;
};
} // namespace

/**
 * The Develop RAW chain below 100% zoom: the mosaic stages and RawDemosaic read full-scale
 * photosites, so RawInputColor at level 1 is in colour, rather than a demosaic of photosites the
 * Read averaged across colour filters. With Zoomed-Out Demosaic at Full it is the level 0 output
 * box-downscaled; at Fast, RawDemosaic's superpixel, which previews Full.
 **/
class RawReducedZoomTest
    : public RawReferenceFixture {
protected:
    // A 128 x 128 window at level 0 inside the site's darktable crop, on even pixels so that level 1
    // pixel (x, y) covers level 0 pixels (2x, 2y) to (2x + 1, 2y + 1).
    static RectI fullScaleWindow(const RawReference::CropSite& site,
                                 const RectD& rod)
    {
        const int x1 = ((int)rod.x1 + site.left) & ~1;
        const int y1 = ((int)rod.y2 - site.top - RawReference::kCropSize) & ~1;

        return RectI(x1, y1, x1 + 128, y1 + 128);
    }

    static RectI halfOf(const RectI& full)
    {
        return RectI(full.x1 / 2, full.y1 / 2, full.x2 / 2, full.y2 / 2);
    }

    static bool renderColour(const NodePtr& node,
                             unsigned mipmapLevel,
                             const RectI& window,
                             RenderSchedulerModeEnum mode,
                             RenderedPlane* plane,
                             std::string* error)
    {
        std::list<ImageLayerDesc> layers;
        node->getEffectInstance()->getPresentLayers(1., ViewIdx(0), -1, &layers);
        std::list<ImageLayerDesc> colour;
        for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
            if (it->isColorLayer()) {
                colour.push_back(*it);
                break;
            }
        }
        if (colour.empty()) {
            *error = "no colour layer";

            return false;
        }
        std::vector<RenderedPlane> planes;
        if (!renderNodePlanesInMode(node, 1., ViewIdx(0), mipmapLevel, window, colour, mode, true /*byPassCache*/, &planes, error)) {
            return false;
        }
        for (std::vector<RenderedPlane>::const_iterator it = planes.begin(); it != planes.end(); ++it) {
            if (it->layer.isColorLayer()) {
                *plane = *it;

                return true;
            }
        }
        *error = "no colour plane rendered";

        return false;
    }

    // The mean over the RGB of every pixel of max - min, how far the plane is from grey.
    static double meanChroma(const RenderedPlane& plane)
    {
        double sum = 0.;
        const RectI& w = plane.window;
        for (int y = w.y1; y < w.y2; ++y) {
            for (int x = w.x1; x < w.x2; ++x) {
                const float r = rawTestPlaneValue(plane, x, y, 0);
                const float g = rawTestPlaneValue(plane, x, y, 1);
                const float b = rawTestPlaneValue(plane, x, y, 2);
                sum += std::max(r, std::max(g, b)) - std::min(r, std::min(g, b));
            }
        }

        return sum / ((double)w.width() * w.height());
    }

    // fine over twice the window of coarse, 2x2 box-downscaled to it.
    static RenderedPlane boxDownscaled(const RenderedPlane& fine,
                                       const RenderedPlane& coarse)
    {
        const RectI& half = coarse.window;
        RenderedPlane expected;
        expected.layer = coarse.layer;
        expected.window = half;
        expected.channels = coarse.channels;
        expected.pixels.resize((std::size_t)half.width() * half.height() * coarse.channels.size(), 0.f);
        const std::size_t nComps = coarse.channels.size();
        for (int y = half.y1; y < half.y2; ++y) {
            for (int x = half.x1; x < half.x2; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const double box = 0.25 * ((double)rawTestPlaneValue(fine, 2 * x, 2 * y, c) + rawTestPlaneValue(fine, 2 * x + 1, 2 * y, c) + rawTestPlaneValue(fine, 2 * x, 2 * y + 1, c) + rawTestPlaneValue(fine, 2 * x + 1, 2 * y + 1, c));
                    expected.pixels[((std::size_t)(y - half.y1) * half.width() + (x - half.x1)) * nComps + c] = (float)box;
                }
            }
        }

        return expected;
    }

    // The RGB error of got against want over want's window, relative to want's mean level.
    static PlaneDiff compare(const RenderedPlane& want,
                             const RenderedPlane& got,
                             const char* name)
    {
        PlaneDiff diff;
        double sum = 0.;
        double max = 0.;
        const RectI& w = want.window;
        for (int y = w.y1; y < w.y2; ++y) {
            for (int x = w.x1; x < w.x2; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const double a = rawTestPlaneValue(want, x, y, c);
                    const double b = rawTestPlaneValue(got, x, y, c);
                    EXPECT_TRUE(std::isfinite(b)) << name << " at " << x << "," << y;
                    if (!std::isfinite(b)) {
                        return diff;
                    }
                    diff.level += std::fabs(a);
                    sum += std::fabs(b - a);
                    max = std::max(max, std::fabs(b - a));
                }
            }
        }
        const double count = 3. * w.width() * w.height();
        diff.level /= count;
        EXPECT_GT(diff.level, 0.) << name;
        if (diff.level > 0.) {
            diff.relMean = sum / count / diff.level;
            diff.relMax = max / diff.level;
        }

        return diff;
    }

    void compareFullWithDownscaledFullScale(RenderSchedulerModeEnum mode,
                                            const char* name)
    {
        ZoomedOutDemosaicScope full(true);
        const RawReference::CropSite site = RawReference::cr2Site();
        const RawDevelop::Chain chain = createDefaultChain(site);
        ASSERT_TRUE(chain.complete()) << name;
        const NodePtr output = chain.inputColor;

        const RectI window = fullScaleWindow(site, regionOfDefinition(output));
        const RectI half = halfOf(window);

        std::string error;
        RenderedPlane fine;
        ASSERT_TRUE(renderColour(output, 0, window, mode, &fine, &error)) << name << ": " << error;
        ASSERT_GE(fine.channels.size(), 3u) << name;
        RenderedPlane coarse;
        ASSERT_TRUE(renderColour(output, 1, half, mode, &coarse, &error)) << name << ": " << error;
        ASSERT_GE(coarse.channels.size(), 3u) << name;
        ASSERT_TRUE(half == coarse.window) << name;

        const RenderedPlane expected = boxDownscaled(fine, coarse);
        const PlaneDiff diff = compare(expected, coarse, name);
        const double wantChroma = meanChroma(expected);
        const double gotChroma = meanChroma(coarse);
        std::printf("%s: level 1 against level 0 box-downscaled: mean error %.4f%%, max error %.3f%% of the mean level %g; chroma %g, expected %g\n",
                    name, 100. * diff.relMean, 100. * diff.relMax, diff.level, gotChroma, wantChroma);

        ASSERT_GT(wantChroma, 1e-3 * diff.level) << name << ": the crop has no colour to tell a grey demosaic by";
        EXPECT_GE(gotChroma, 0.9 * wantChroma) << name;
        EXPECT_LE(diff.relMean, 2e-3) << name;
        EXPECT_LE(diff.relMax, 5e-2) << name;
    }

    // Fast's level 1 against Full's: the superpixel is a preview, close on average and as
    // colourful, its edges softer and its fine detail aliased where the method's are not.
    void compareFastWithFull(const RawReference::CropSite& site,
                             RenderSchedulerModeEnum mode,
                             const char* name)
    {
        RenderedPlane fast;
        RenderedPlane full;
        std::string error;
        for (int pass = 0; pass < 2; ++pass) {
            ZoomedOutDemosaicScope scope(pass == 1);
            const RawDevelop::Chain chain = createDefaultChain(site);
            ASSERT_TRUE(chain.complete()) << name;
            const RectI half = halfOf(fullScaleWindow(site, regionOfDefinition(chain.inputColor)));
            RenderedPlane* plane = (pass == 0) ? &fast : &full;
            ASSERT_TRUE(renderColour(chain.inputColor, 1, half, mode, plane, &error)) << name << ": " << error;
            ASSERT_GE(plane->channels.size(), 3u) << name;
            ASSERT_TRUE(half == plane->window) << name;
        }
        ASSERT_TRUE(fast.window == full.window) << name;

        const PlaneDiff diff = compare(full, fast, name);
        const double fullChroma = meanChroma(full);
        const double fastChroma = meanChroma(fast);
        std::printf("%s: Fast against Full at level 1: mean error %.4f%%, max error %.3f%% of the mean level %g; chroma %g, Full %g\n",
                    name, 100. * diff.relMean, 100. * diff.relMax, diff.level, fastChroma, fullChroma);

        ASSERT_GT(fullChroma, 1e-3 * diff.level) << name << ": the crop has no colour to tell a grey demosaic by";
        EXPECT_GE(fastChroma, 0.75 * fullChroma) << name;
        EXPECT_LE(fastChroma, 1.25 * fullChroma) << name;
        EXPECT_LE(diff.relMean, 5e-2) << name;
    }
};

TEST_F(RawReducedZoomTest, MosaicStagesPullFullScaleInputs)
{
    const RawDevelop::Chain chain = createDefaultChain(RawReference::cr2Site());
    ASSERT_TRUE(chain.complete());

    EXPECT_TRUE(chain.prepare->getEffectInstance()->pullsInputsAtScaleOne());
    EXPECT_TRUE(chain.temperature->getEffectInstance()->pullsInputsAtScaleOne());
    EXPECT_TRUE(chain.highlights->getEffectInstance()->pullsInputsAtScaleOne());
    EXPECT_TRUE(chain.demosaic->getEffectInstance()->pullsInputsAtScaleOne());
    EXPECT_FALSE(chain.inputColor->getEffectInstance()->pullsInputsAtScaleOne());
    EXPECT_FALSE(chain.colorCalibration->getEffectInstance()->pullsInputsAtScaleOne());
}

TEST_F(RawReducedZoomTest, PreferenceSetsDemosaicRenderScaleSupport)
{
    ZoomedOutDemosaicScope fast(false);
    const RawDevelop::Chain chain = createDefaultChain(RawReference::cr2Site());
    ASSERT_TRUE(chain.complete());
    const EffectInstancePtr demosaic = chain.demosaic->getEffectInstance();

    EXPECT_EQ(EffectInstance::eSupportsYes, demosaic->supportsRenderScaleMaybe());
    const U64 fastHash = chain.demosaic->getHashValue();
    {
        ZoomedOutDemosaicScope full(true);
        EXPECT_EQ(EffectInstance::eSupportsNo, demosaic->supportsRenderScaleMaybe());
        EXPECT_NE(fastHash, chain.demosaic->getHashValue());
    }
    EXPECT_EQ(EffectInstance::eSupportsYes, demosaic->supportsRenderScaleMaybe());
    EXPECT_EQ(fastHash, chain.demosaic->getHashValue());
}

TEST_F(RawReducedZoomTest, Cr2FullLevelOneMatchesDownscaledLevelZeroLegacy)
{
    compareFullWithDownscaledFullScale(eRenderSchedulerModeLegacy, "CR2 Full Legacy");
}

TEST_F(RawReducedZoomTest, Cr2FullLevelOneMatchesDownscaledLevelZeroTaskGraph)
{
    compareFullWithDownscaledFullScale(eRenderSchedulerModeTaskGraph, "CR2 Full Task graph");
}

TEST_F(RawReducedZoomTest, Cr2FastMatchesFullAtLevelOneLegacy)
{
    compareFastWithFull(RawReference::cr2Site(), eRenderSchedulerModeLegacy, "CR2 Legacy");
}

TEST_F(RawReducedZoomTest, Cr2FastMatchesFullAtLevelOneTaskGraph)
{
    compareFastWithFull(RawReference::cr2Site(), eRenderSchedulerModeTaskGraph, "CR2 Task graph");
}

TEST_F(RawReducedZoomTest, RafFastMatchesFullAtLevelOneLegacy)
{
    compareFastWithFull(RawReference::rafSite(), eRenderSchedulerModeLegacy, "RAF Legacy");
}

TEST_F(RawReducedZoomTest, RafFastMatchesFullAtLevelOneTaskGraph)
{
    compareFastWithFull(RawReference::rafSite(), eRenderSchedulerModeTaskGraph, "RAF Task graph");
}
