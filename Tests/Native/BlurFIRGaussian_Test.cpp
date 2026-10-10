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
#include <cstddef>
#include <iostream>
#include <list>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/BlurKernels.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

using BlurKernels::LineFilter;
using BlurKernels::LineScratch;

namespace {

const double kTime = 1.;
const int kFIRChoiceIndex = 5;
const int kIIRChoiceIndex = 1;

// Deterministic values in [0, 1) that differ from one sample to the next.
float
pseudoRandom(unsigned index)
{
    unsigned h = index * 2654435761u + 12345u;

    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;

    return static_cast<float>(h & 0xFFFFFu) / static_cast<float>(0x100000u);
}

// The truncated, normalised Gaussian of the spec, in double.
std::vector<double>
referenceWeights(double sigma)
{
    const int radius = LineFilter::firGaussianRadius(sigma);
    std::vector<double> w(2 * radius + 1);
    double sum = 0.;

    for (int k = -radius; k <= radius; ++k) {
        w[k + radius] = std::exp(-static_cast<double>(k) * k / (2. * sigma * sigma));
        sum += w[k + radius];
    }
    for (std::size_t i = 0; i < w.size(); ++i) {
        w[i] /= sum;
    }

    return w;
}

double
referenceSample(const std::vector<float>& line,
                int i,
                bool neumann)
{
    const int n = static_cast<int>(line.size());

    if ((i >= 0) && (i < n)) {
        return line[i];
    }
    if (!neumann) {
        return 0.;
    }

    return line[(i < 0) ? 0 : n - 1];
}

std::vector<double>
referenceFilter(const std::vector<float>& line,
                double sigma,
                bool neumann)
{
    const std::vector<double> w = referenceWeights(sigma);
    const int radius = static_cast<int>(w.size() / 2);
    std::vector<double> out(line.size());

    for (int i = 0; i < static_cast<int>(line.size()); ++i) {
        double acc = 0.;
        for (int k = -radius; k <= radius; ++k) {
            acc += w[k + radius] * referenceSample(line, i + k, neumann);
        }
        out[i] = acc;
    }

    return out;
}

double
maxAbsDiff(const std::vector<float>& a,
           const std::vector<float>& b)
{
    double m = 0.;

    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
    }

    return m;
}

} // namespace

TEST(BlurFIRGaussianKernel, RadiusRule)
{
    EXPECT_EQ(0, LineFilter::firGaussianRadius(0.));
    EXPECT_EQ(0, LineFilter::firGaussianRadius(0.0999));
    EXPECT_EQ(1, LineFilter::firGaussianRadius(0.1));
    EXPECT_EQ(2, LineFilter::firGaussianRadius(0.5));
    EXPECT_EQ(3, LineFilter::firGaussianRadius(1.));
    EXPECT_EQ(9, LineFilter::firGaussianRadius(3.));
    EXPECT_EQ(75, LineFilter::firGaussianRadius(25.));
    EXPECT_TRUE(LineFilter::firGaussian(0.05, 0, true).isIdentity());
    EXPECT_FALSE(LineFilter::firGaussian(0.5, 0, true).isIdentity());
}

TEST(BlurFIRGaussianKernel, ImpulseResponseMatchesTruncatedGaussian)
{
    const double sigmas[] = { 0.5, 3., 25. };

    for (std::size_t s = 0; s < sizeof(sigmas) / sizeof(sigmas[0]); ++s) {
        const double sigma = sigmas[s];
        const int radius = LineFilter::firGaussianRadius(sigma);
        const std::vector<double> w = referenceWeights(sigma);
        const int n = 2 * (3 * radius + 5) + 1;
        const int center = n / 2;

        for (int boundary = 0; boundary < 2; ++boundary) {
            const bool neumann = (boundary == 1);
            std::vector<float> line(n, 0.f);
            line[center] = 1.f;
            LineFilter::firGaussian(sigma, 0, neumann).apply(line.data(), n, 1);

            double sum = 0.;
            for (int i = 0; i < n; ++i) {
                const int k = i - center;
                const double expected = (std::abs(k) <= radius) ? w[k + radius] : 0.;
                EXPECT_NEAR(expected, line[i], 1e-6) << "sigma " << sigma << ", neumann " << neumann << ", offset " << k;
                if (std::abs(k) > radius) {
                    EXPECT_EQ(0.f, line[i]) << "sigma " << sigma << ", neumann " << neumann << ", offset " << k;
                }
                sum += line[i];
            }
            EXPECT_NEAR(1., sum, 1e-6) << "sigma " << sigma << ", neumann " << neumann;
        }
    }
}

TEST(BlurFIRGaussianKernel, MatchesDoublePrecisionReferenceOnAnyLine)
{
    const double sigmas[] = { 0.5, 3., 25. };
    const int lengths[] = { 5, 40, 700 };

    for (std::size_t s = 0; s < sizeof(sigmas) / sizeof(sigmas[0]); ++s) {
        for (std::size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); ++l) {
            const int n = lengths[l];
            std::vector<float> src(n);
            for (int i = 0; i < n; ++i) {
                src[i] = pseudoRandom(static_cast<unsigned>(i + 31 * n));
            }
            for (int boundary = 0; boundary < 2; ++boundary) {
                const bool neumann = (boundary == 1);
                std::vector<float> line = src;
                LineFilter::firGaussian(sigmas[s], 0, neumann).apply(line.data(), n, 1);
                const std::vector<double> expected = referenceFilter(src, sigmas[s], neumann);
                for (int i = 0; i < n; ++i) {
                    ASSERT_NEAR(expected[i], line[i], 1e-6) << "sigma " << sigmas[s] << ", n " << n << ", neumann " << neumann << ", i " << i;
                }
            }
        }
    }
}

// The radius of a size-500 blur, which float accumulation over 1251 taps must still serve:
// measured 9e-7 on values in [0, 1).
TEST(BlurFIRGaussianKernel, LargeRadiusMatchesDoublePrecisionReference)
{
    const double sigma = 625. / 3.;
    const int n = 4000;
    std::vector<float> src(n);

    ASSERT_EQ(625, LineFilter::firGaussianRadius(sigma));
    for (int i = 0; i < n; ++i) {
        src[i] = pseudoRandom(static_cast<unsigned>(i));
    }
    for (int boundary = 0; boundary < 2; ++boundary) {
        const bool neumann = (boundary == 1);
        std::vector<float> line = src;
        LineFilter::firGaussian(sigma, 0, neumann).apply(line.data(), n, 1);
        const std::vector<double> expected = referenceFilter(src, sigma, neumann);
        double worst = 0.;
        for (int i = 0; i < n; ++i) {
            worst = std::max(worst, std::fabs(expected[i] - line[i]));
        }
        std::cout << "[ report ] FIR Gaussian, radius 625, neumann " << neumann << ": max abs error " << worst << std::endl;
        EXPECT_LE(worst, 2e-6) << "neumann " << neumann;
    }

    std::vector<float> ones(n, 1.f);
    LineFilter::firGaussian(sigma, 0, true).apply(ones.data(), n, 1);
    double worstOnes = 0.;
    for (int i = 0; i < n; ++i) {
        worstOnes = std::max(worstOnes, std::fabs(1. - ones[i]));
    }
    std::cout << "[ report ] FIR Gaussian, radius 625, constant 1: max abs error " << worstOnes << std::endl;
    EXPECT_LE(worstOnes, 2e-6);
}

TEST(BlurFIRGaussianKernel, BoundaryModesOnAConstantLine)
{
    const int n = 200;
    const double sigma = 3.;
    const int radius = LineFilter::firGaussianRadius(sigma);
    const std::vector<double> w = referenceWeights(sigma);

    std::vector<float> nearest(n, 1.f);
    LineFilter::firGaussian(sigma, 0, true).apply(nearest.data(), n, 1);
    for (int i = 0; i < n; ++i) {
        EXPECT_NEAR(1., nearest[i], 1e-6) << i;
    }

    std::vector<float> black(n, 1.f);
    LineFilter::firGaussian(sigma, 0, false).apply(black.data(), n, 1);
    double edge = 0.;
    for (int k = 0; k <= radius; ++k) {
        edge += w[radius + k];
    }
    EXPECT_NEAR(edge, black[0], 1e-6);
    EXPECT_NEAR(edge, black[n - 1], 1e-6);
    EXPECT_NEAR(1., black[n / 2], 1e-6);
}

TEST(BlurFIRGaussianKernel, StrideAndInterleavedLinesGiveTheSameResult)
{
    const int n = 90;
    const int comps = 4;
    std::vector<float> planar(n);
    std::vector<float> interleaved(static_cast<std::size_t>(n) * comps, -7.f);

    for (int i = 0; i < n; ++i) {
        planar[i] = pseudoRandom(static_cast<unsigned>(i));
        interleaved[static_cast<std::size_t>(i) * comps + 2] = planar[i];
    }
    const LineFilter f = LineFilter::firGaussian(2., 0, true);
    f.apply(planar.data(), n, 1);
    f.apply(interleaved.data() + 2, n, comps);
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(planar[i], interleaved[static_cast<std::size_t>(i) * comps + 2]) << i;
        EXPECT_EQ(-7.f, interleaved[static_cast<std::size_t>(i) * comps + 1]) << i;
    }
}

TEST(BlurFIRGaussianKernel, ApplyColumnsEqualsColumnByColumn)
{
    // More columns than two blocks, with a ragged last block, and rows both shorter and longer
    // than the radius.
    const int widths[] = { 1, 7, 2 * LineFilter::kColumnBlock + 23 };
    const int heights[] = { 5, 61 };
    const double sigmas[] = { 0.5, 3. };

    for (std::size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); ++wi) {
        for (std::size_t hi = 0; hi < sizeof(heights) / sizeof(heights[0]); ++hi) {
            for (std::size_t si = 0; si < sizeof(sigmas) / sizeof(sigmas[0]); ++si) {
                for (int boundary = 0; boundary < 2; ++boundary) {
                    const int w = widths[wi];
                    const int h = heights[hi];
                    const bool neumann = (boundary == 1);
                    std::vector<float> a(static_cast<std::size_t>(w) * h);
                    for (std::size_t i = 0; i < a.size(); ++i) {
                        a[i] = pseudoRandom(static_cast<unsigned>(i));
                    }
                    std::vector<float> b = a;
                    const LineFilter f = LineFilter::firGaussian(sigmas[si], 0, neumann);

                    LineScratch scratch;
                    f.applyColumns(a.data(), h, w, w, scratch);
                    for (int x = 0; x < w; ++x) {
                        f.apply(b.data() + x, h, w, scratch);
                    }
                    EXPECT_LE(maxAbsDiff(a, b), 1e-6) << "w " << w << ", h " << h << ", sigma " << sigmas[si] << ", neumann " << neumann;
                }
            }
        }
    }
}

TEST(BlurFIRGaussianKernel, ForFilterBuildsTheFIRGaussianFromSigma)
{
    const int n = 101;
    std::vector<float> viaFilter(n, 0.f);
    std::vector<float> direct(n, 0.f);

    viaFilter[n / 2] = direct[n / 2] = 1.f;
    LineFilter::forFilter(BlurKernels::eFilterFIRGaussian, 3.f, 0, true).apply(viaFilter.data(), n, 1);
    LineFilter::firGaussian(3., 0, true).apply(direct.data(), n, 1);
    EXPECT_EQ(direct, viaFilter);
    EXPECT_EQ(5, static_cast<int>(BlurKernels::eFilterFIRGaussian));
}

// The figures are context for ports of the filter to other back ends, which have to choose
// between the two Gaussians; the bounds only catch a filter that is no longer a Gaussian of the
// same sigma (measured: about 5e-3 and 1e-2 at sigma 3, 2e-4 and 5e-3 at sigma 25).
TEST(BlurFIRGaussianKernel, ReportDifferenceFromTheIIRGaussian)
{
    const float sizes[] = { 7.2f, 60.f };

    for (std::size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        const float sigma = sizes[s] / 2.4f;
        const int n = 1201;
        std::vector<float> impulse(n, 0.f);
        std::vector<float> step(n, 0.f);
        impulse[n / 2] = 1.f;
        std::fill(step.begin() + n / 2, step.end(), 1.f);
        std::vector<float> impulseIIR = impulse;
        std::vector<float> stepIIR = step;

        LineFilter::firGaussian(sigma, 0, true).apply(impulse.data(), n, 1);
        LineFilter::firGaussian(sigma, 0, true).apply(step.data(), n, 1);
        LineFilter::forFilter(BlurKernels::eFilterGaussian, sigma, 0, true).apply(impulseIIR.data(), n, 1);
        LineFilter::forFilter(BlurKernels::eFilterGaussian, sigma, 0, true).apply(stepIIR.data(), n, 1);
        const double impulseDiff = maxAbsDiff(impulse, impulseIIR);
        const double stepDiff = maxAbsDiff(step, stepIIR);
        std::cout << "[ report ] FIR - IIR, sigma " << sigma << ": impulse " << impulseDiff << ", step " << stepDiff << std::endl;
        EXPECT_LT(impulseDiff, 0.01) << "sigma " << sigma;
        EXPECT_LT(stepDiff, 0.05) << "sigma " << sigma;
    }
}

class BlurFIRGaussianTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "blurFIR64x48", 1.));
    }

    static int filterIndexOf(const NodePtr& blur)
    {
        KnobChoice* filter = blur ? dynamic_cast<KnobChoice*>(blur->getKnobByName(kBlurParamFilter).get()) : NULL;

        EXPECT_TRUE(filter != NULL);

        return filter ? filter->getValue() : -1;
    }

    static RectI renderWindow(const NodePtr& node,
                              unsigned mipmapLevel = 0)
    {
        RectD rod;
        EffectInstancePtr effect = node->getEffectInstance();
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, scale, ViewIdx(0), &rod);

        EXPECT_NE(eStatusFailed, stat);

        return rod.toPixelEnclosing(mipmapLevel, 1.);
    }

    static bool renderRGBA(const NodePtr& node,
                           const RectI& window,
                           RenderedPlane* plane,
                           unsigned mipmapLevel = 0)
    {
        std::list<ImageLayerDesc> layers;
        std::vector<RenderedPlane> planes;
        std::string error;

        layers.push_back(ImageLayerDesc::getRGBAComponents());
        const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);
        EXPECT_TRUE(ok) << error;
        if (!ok || (planes.size() != 1)) {
            return false;
        }
        *plane = planes[0];

        return true;
    }

    // Renders `blur` over its whole region of definition and over `sub`, and requires the second
    // to equal the matching crop of the first.
    void expectSubRectEqualsCrop(const NodePtr& blur,
                                 const RectI& sub,
                                 const std::string& caseName)
    {
        const RectI full = renderWindow(blur);
        RenderedPlane whole, part;

        ASSERT_TRUE(renderRGBA(blur, full, &whole)) << caseName;
        ASSERT_TRUE(renderRGBA(blur, sub, &part)) << caseName;
        ASSERT_EQ(whole.channels.size(), part.channels.size());
        ASSERT_TRUE(full.contains(sub)) << caseName;

        const std::size_t nc = whole.channels.size();
        const int fullW = full.x2 - full.x1;
        const int subW = sub.x2 - sub.x1;
        const int subH = sub.y2 - sub.y1;
        ASSERT_EQ(static_cast<std::size_t>(subW) * subH * nc, part.pixels.size()) << caseName;
        double worst = 0.;
        for (int y = 0; y < subH; ++y) {
            for (int x = 0; x < subW; ++x) {
                for (std::size_t c = 0; c < nc; ++c) {
                    const float a = whole.pixels[(static_cast<std::size_t>(sub.y1 - full.y1 + y) * fullW + (sub.x1 - full.x1 + x)) * nc + c];
                    const float b = part.pixels[(static_cast<std::size_t>(y) * subW + x) * nc + c];
                    worst = std::max(worst, std::fabs(static_cast<double>(a) - b));
                }
            }
        }
        EXPECT_LE(worst, 1e-6) << caseName;
    }

    NodePtr makeBlurOnSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);

        EXPECT_TRUE(bool(source));
        EXPECT_TRUE(bool(blur));
        if (source && blur) {
            setParitySourceOrigin(source, 0, 0);
            connectNodes(source, blur, 0, true);
            EXPECT_TRUE(setKnobValues(blur, kBlurParamCropToFormat, { 0. }));
        }

        return blur;
    }

    // A Blur, uncropped, of a black image of the project format with a white, opaque rectangle
    // `box` over it.
    NodePtr makeBoxOverBlack(const RectI& box)
    {
        NodePtr black = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        NodePtr white = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE), PLUGIN_MAJOR_NATRON_MERGE);

        EXPECT_TRUE(bool(black) && bool(white) && bool(merge));
        if (!black || !white || !merge) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(black, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
        EXPECT_TRUE(setKnobValues(black, kConstantParamColor, { 0., 0., 0., 0. }));
        EXPECT_TRUE(setKnobValue(white, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
        EXPECT_TRUE(setKnobValues(white, kNativeGeneratorParamBottomLeft, { (double)box.x1, (double)box.y1 }));
        EXPECT_TRUE(setKnobValues(white, kNativeGeneratorParamSize, { (double)box.width(), (double)box.height() }));
        EXPECT_TRUE(setKnobValues(white, kConstantParamColor, { 1., 1., 1., 1. }));
        EXPECT_TRUE(setKnobValue(merge, kMergeParamOperation, std::string("over")));
        connectNodes(black, merge, 0, true);
        connectNodes(white, merge, 1, true);

        NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
        EXPECT_TRUE(bool(blur));
        if (blur) {
            connectNodes(merge, blur, 0, true);
            EXPECT_TRUE(setKnobValues(blur, kBlurParamCropToFormat, { 0. }));
        }

        return blur;
    }

    // Renders `blur` over its region of definition at `mipmapLevel` and compares its alpha with
    // the separable double-precision truncated Gaussian of `source` (1 inside `box`, 0 elsewhere
    // in `format`, extended outside `format` by the boundary condition). `format`, `box` and
    // `sigma` are in pixels at that level.
    void expectMatchesReference(const NodePtr& blur,
                                const RectI& format,
                                const RectI& box,
                                double sigma,
                                bool neumann,
                                const std::string& caseName,
                                unsigned mipmapLevel = 0)
    {
        const RectI window = renderWindow(blur, mipmapLevel);
        RenderedPlane plane;

        ASSERT_TRUE(renderRGBA(blur, window, &plane, mipmapLevel)) << caseName;
        const std::size_t nc = plane.channels.size();
        ASSERT_EQ(4u, nc) << caseName;
        const int W = window.width();
        const int H = window.height();
        ASSERT_EQ(static_cast<std::size_t>(W) * H * nc, plane.pixels.size()) << caseName;

        const std::vector<double> w = referenceWeights(sigma);
        const int radius = static_cast<int>(w.size() / 2);
        const auto source = [&](int x, int y) -> double {
            if (neumann) {
                x = std::min(std::max(x, format.x1), format.x2 - 1);
                y = std::min(std::max(y, format.y1), format.y2 - 1);
            } else if ((x < format.x1) || (x >= format.x2) || (y < format.y1) || (y >= format.y2)) {
                return 0.;
            }

            return ((x >= box.x1) && (x < box.x2) && (y >= box.y1) && (y < box.y2)) ? 1. : 0.;
        };

        const int rowsY1 = window.y1 - radius;
        const int nRows = H + 2 * radius;
        std::vector<double> rows(static_cast<std::size_t>(nRows) * W, 0.);
        for (int r = 0; r < nRows; ++r) {
            for (int x = window.x1; x < window.x2; ++x) {
                double acc = 0.;
                for (int i = -radius; i <= radius; ++i) {
                    acc += w[i + radius] * source(x + i, rowsY1 + r);
                }
                rows[static_cast<std::size_t>(r) * W + (x - window.x1)] = acc;
            }
        }

        double worst = 0.;
        double sum = 0.;
        int nonZeroOutsideSupport = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                double expected = 0.;
                for (int j = -radius; j <= radius; ++j) {
                    expected += w[j + radius] * rows[static_cast<std::size_t>(y + j - rowsY1) * W + (x - window.x1)];
                }
                const float got = plane.pixels[(static_cast<std::size_t>(y - window.y1) * W + (x - window.x1)) * nc + 3];
                worst = std::max(worst, std::fabs(expected - got));
                sum += got;
                if ((expected == 0.) && (got != 0.f)) {
                    ++nonZeroOutsideSupport;
                }
            }
        }
        EXPECT_LE(worst, 1e-6) << caseName;
        EXPECT_EQ(0, nonZeroOutsideSupport) << caseName;
        if ((box.width() == 1) && (box.height() == 1)) {
            EXPECT_NEAR(1., sum, 1e-5) << caseName;
        }
    }

    bool saveResetLoad(const QTemporaryDir& tmp)
    {
        ProjectPtr project = getApp()->getProject();
        const QString dirPath = tmp.path() + QLatin1Char('/');
        const QString fileName = QString::fromUtf8("blur-fir-gaussian.ntp");
        QString savedFilePath;

        if (!project->saveProject(dirPath, fileName, &savedFilePath) || !QFile::exists(savedFilePath)) {
            return false;
        }
        project->reset(false, true);

        return project->loadProject(dirPath, fileName);
    }
};

TEST_F(BlurFIRGaussianTest, NewBlurDefaultsToTheFIRGaussian)
{
    resetProject();
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);

    ASSERT_TRUE(bool(blur));
    EXPECT_EQ(kFIRChoiceIndex, filterIndexOf(blur));
    KnobChoice* filter = dynamic_cast<KnobChoice*>(blur->getKnobByName(kBlurParamFilter).get());
    ASSERT_TRUE(filter != NULL);
    EXPECT_EQ(std::string(kBlurParamFilterFIRGaussian), filter->getActiveEntry().id);
    ASSERT_TRUE(setKnobValue(blur, kBlurParamFilter, std::string(kBlurParamFilterGaussian)));
    EXPECT_EQ(kIIRChoiceIndex, filterIndexOf(blur));
}

TEST_F(BlurFIRGaussianTest, ImpulseAndStepThroughTheNodeMatchTheReference)
{
    const RectI format(0, 0, kParitySourceWidth, kParitySourceHeight);
    const RectI impulse(30, 20, 31, 21);
    const RectI step(32, 0, kParitySourceWidth, kParitySourceHeight);
    const double sigmas[] = { 0.5, 3., 25. };

    for (int shape = 0; shape < 2; ++shape) {
        const RectI& box = shape ? step : impulse;
        resetProject();
        NodePtr blur = makeBoxOverBlack(box);
        ASSERT_TRUE(bool(blur));
        ASSERT_EQ(kFIRChoiceIndex, filterIndexOf(blur));
        for (std::size_t s = 0; s < sizeof(sigmas) / sizeof(sigmas[0]); ++s) {
            ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 2.4 * sigmas[s], 2.4 * sigmas[s] }));
            for (int boundary = 0; boundary < 2; ++boundary) {
                const bool neumann = (boundary == 1);
                ASSERT_TRUE(setKnobValue(blur, kBlurParamBoundary, std::string(neumann ? kBlurParamBoundaryNearest : kBlurParamBoundaryBlack)));
                std::ostringstream caseName;
                caseName << (shape ? "step" : "impulse") << ", sigma " << sigmas[s] << (neumann ? ", nearest" : ", black");
                expectMatchesReference(blur, format, box, sigmas[s], neumann, caseName.str());
            }
        }
    }
}

TEST_F(BlurFIRGaussianTest, ReducedRenderScaleScalesSigma)
{
    // At mipmap level 1, a size of 14.4 (sigma 6) is a sigma of 3 over a half-resolution image.
    const RectI box(20, 10, 40, 30);
    const RectI halfFormat(0, 0, kParitySourceWidth / 2, kParitySourceHeight / 2);
    const RectI halfBox(box.x1 / 2, box.y1 / 2, box.x2 / 2, box.y2 / 2);

    resetProject();
    NodePtr blur = makeBoxOverBlack(box);
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(blur->getEffectInstance()->supportsRenderScale());
    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 14.4, 14.4 }));
    expectMatchesReference(blur, halfFormat, halfBox, 3., false, "size 14.4 at mipmap level 1", 1);

    BlurParams params;
    params.filter = BlurKernels::eFilterFIRGaussian;
    params.sizeX = params.sizeY = 14.4;
    const RenderScale half = RenderScale::fromMipmapLevel(1);
    EXPECT_EQ(RectI(10 - 9, 20 - 9, 30 + 9, 40 + 9), Blur::getSourceRoI(RectI(10, 20, 30, 40), half, params));
    params.sizeX = params.sizeY = 0.4;
    EXPECT_FALSE(Blur::paramsAreIdentity(RenderScale::identity, params));
    EXPECT_TRUE(Blur::paramsAreIdentity(half, params));
}

TEST_F(BlurFIRGaussianTest, RegionOfDefinitionGrowsByTheRadius)
{
    const RectI format(0, 0, kParitySourceWidth, kParitySourceHeight);

    resetProject();
    NodePtr blur = makeBoxOverBlack(RectI(20, 10, 40, 30));
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 7.2, 2.4 }));
    EXPECT_EQ(RectI(-9, -3, kParitySourceWidth + 9, kParitySourceHeight + 3), renderWindow(blur));
    EXPECT_EQ(RectI(-5, -2, kParitySourceWidth / 2 + 5, kParitySourceHeight / 2 + 2), renderWindow(blur, 1));

    ASSERT_TRUE(setKnobValues(blur, kBlurParamOrderX, { 1. }));
    EXPECT_EQ(RectI(-10, -3, kParitySourceWidth + 10, kParitySourceHeight + 3), renderWindow(blur));

    ASSERT_TRUE(setKnobValues(blur, kBlurParamOrderX, { 0. }));
    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 0.2, 0.2 }));
    EXPECT_EQ(format, renderWindow(blur));

    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 7.2, 7.2 }));
    ASSERT_TRUE(setKnobValues(blur, kBlurParamExpandRoD, { 0. }));
    EXPECT_EQ(format, renderWindow(blur));
}

TEST_F(BlurFIRGaussianTest, SubRectRenderEqualsCropOfFullRender)
{
    resetProject();
    NodePtr blur = makeBlurOnSource();
    ASSERT_TRUE(bool(blur));

    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 9., 4. }));
    expectSubRectEqualsCrop(blur, RectI(20, 10, 45, 30), "size 9x4, black");

    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 30., 30. }));
    expectSubRectEqualsCrop(blur, RectI(0, 0, 20, 17), "size 30, black, bottom-left corner");
    expectSubRectEqualsCrop(blur, RectI(40, 30, 64, 48), "size 30, black, top-right corner");

    ASSERT_TRUE(setKnobValue(blur, kBlurParamBoundary, std::string(kBlurParamBoundaryNearest)));
    expectSubRectEqualsCrop(blur, RectI(0, 0, 20, 17), "size 30, nearest, bottom-left corner");
    expectSubRectEqualsCrop(blur, RectI(25, 20, 50, 40), "size 30, nearest");

    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 12., 6. }));
    ASSERT_TRUE(setKnobValues(blur, kBlurParamOrderX, { 1. }));
    expectSubRectEqualsCrop(blur, RectI(25, 20, 50, 40), "size 12x6, nearest, x derivative");
}

TEST_F(BlurFIRGaussianTest, RegionOfInterestPaddingIsTheRadius)
{
    BlurParams params;
    const RectI rect(10, 20, 30, 40);

    params.filter = BlurKernels::eFilterFIRGaussian;
    params.sizeX = params.sizeY = 7.2;
    RectI roi = Blur::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(RectI(10 - 9, 20 - 9, 30 + 9, 40 + 9), roi);

    params.orderY = 1;
    roi = Blur::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(RectI(10 - 9, 20 - 10, 30 + 9, 40 + 10), roi);

    params.orderY = 0;
    params.sizeX = params.sizeY = 0.2;
    EXPECT_TRUE(Blur::paramsAreIdentity(RenderScale::identity, params));
}

TEST_F(BlurFIRGaussianTest, SavedDefaultFilterReloadsAsFIR)
{
    QTemporaryDir tmp;

    ASSERT_TRUE(tmp.isValid());
    resetProject();
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    ASSERT_TRUE(bool(blur));
    const std::string name = blur->getScriptName();
    ASSERT_EQ(kFIRChoiceIndex, filterIndexOf(blur));

    ASSERT_TRUE(saveResetLoad(tmp));
    NodePtr loaded = getApp()->getProject()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_EQ(kFIRChoiceIndex, filterIndexOf(loaded));
}
