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
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Gpu/GradeFixture.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/GpuRender.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

namespace {

struct GradeSetting {
    double multiply[4];
    double offset[4];
    double gamma[4];
    bool reverse;
    bool clampBlack;
    bool clampWhite;
};

class NativeGradeGpuTest
    : public BaseTest {
protected:
    Grade* makeGrade(NodePtr& node)
    {
        node = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        EXPECT_TRUE(bool(node));
        Grade* effect = node ? dynamic_cast<Grade*>(node->getEffectInstance().get()) : NULL;
        EXPECT_TRUE(effect != NULL);

        return effect;
    }

    // Leaves the points and the lift/gain at their defaults, so multiply is the gain of every
    // channel and offset its offset.
    void applySetting(const NodePtr& node,
                      const GradeSetting& s)
    {
        ASSERT_TRUE(setKnobValues(node, "multiply", std::vector<double>(s.multiply, s.multiply + 4)));
        ASSERT_TRUE(setKnobValues(node, "offset", std::vector<double>(s.offset, s.offset + 4)));
        ASSERT_TRUE(setKnobValues(node, "gamma", std::vector<double>(s.gamma, s.gamma + 4)));
        ASSERT_TRUE(setKnobValues(node, "reverse", { s.reverse ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, "clampBlack", { s.clampBlack ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, "clampWhite", { s.clampWhite ? 1. : 0. }));
    }

    // The native CPU path: the op's own row kernel over packed rows. Channels outside the mask
    // keep their input value.
    static std::vector<float> cpuRender(Grade* effect,
                                        const std::vector<float>& src,
                                        int width,
                                        int height,
                                        int nComps,
                                        unsigned channelMask)
    {
        const std::bitset<4> channels(channelMask);
        const GradeOp::Params params = effect->bakeGrade(0., ViewIdx(0));
        std::vector<float> dst(src);
        for (int y = 0; y < height; ++y) {
            RowIO io;
            io.nSrc = 1;
            io.src[0] = src.data() + (size_t)y * width * nComps;
            io.dst = dst.data() + (size_t)y * width * nComps;
            io.y = y;
            io.width = width;
            io.nComps = nComps;
            io.channels = channels;
            effect->gradeRow(params, io);
        }

        return dst;
    }

    static GpuQuery eligibleQuery()
    {
        GpuQuery q;
        q.nComps = 4;
        q.bitDepth = eImageBitDepthFloat;

        return q;
    }
};

// Every value of the fixture sweep in each of the four channels, NaN, infinities, signed zeros and
// subnormals included.
std::vector<float>
sweepImage(int* width)
{
    const std::vector<float> sweep = gradefix::makeSweep();
    const size_t n = sweep.size();
    std::vector<float> px(n * 4);

    for (size_t i = 0; i < px.size(); ++i) {
        px[i] = sweep[(i / 4 + (i % 4) * 37) % n];
    }
    *width = (int)n;

    return px;
}

// Gamma knob values at and below zero, and one between zero and the floor, one per channel.
const GradeSetting kGammaBelowFloor = { { 1., 1., 1., 1. }, { 0., 0., 0., 0. }, { 0., -1., -0.25, 0.004 }, false, false, false };
const GradeSetting kGammaAtFloor = { { 1., 1., 1., 1. }, { 0., 0., 0., 0. }, { gradefix::kMinGamma, gradefix::kMinGamma, gradefix::kMinGamma, gradefix::kMinGamma }, false, false, false };

} // namespace

TEST_F(NativeGradeGpuTest, EligibilityFollowsTheSupportedSettings)
{
    NodePtr node;
    Grade* grade = makeGrade(node);
    ASSERT_TRUE(grade != NULL);

#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, grade->canRenderOnGpu(q)) << nComps << " components";
    }
    for (int nComps : { 0, 5 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_FALSE(grade->canRenderOnGpu(q)) << nComps << " components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.processChannels[0] = false;
        q.processChannels[3] = false;
        EXPECT_EQ(supported, grade->canRenderOnGpu(q)) << "a channel subset";
    }

    {
        GpuQuery q = eligibleQuery();
        q.hasMask = true;
        q.maskInPlane = true;
        q.mix = 0.5;
        q.unPremultChannel = 3;
        q.unPremultInPlane = true;
        EXPECT_EQ(supported, grade->canRenderOnGpu(q)) << "mask, mix and unpremult by alpha";
        q.maskInPlane = false;
        EXPECT_EQ(supported, grade->canRenderOnGpu(q)) << "a mask channel outside the plane, read in its own layer";
        q.maskInPlane = true;
        q.unPremultInPlane = false;
        EXPECT_FALSE(grade->canRenderOnGpu(q)) << "an unpremult channel outside the plane";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf, eImageBitDepthNone }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(grade->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }
}

TEST_F(NativeGradeGpuTest, DefaultKernelPassesEveryValueThrough)
{
    NodePtr node;
    Grade* grade = makeGrade(node);
    ASSERT_TRUE(grade != NULL);

    int width = 0;
    const std::vector<float> src = sweepImage(&width);
    const std::vector<float> got = cpuRender(grade, src, width, 1, 4, 0xF);
    const ImageDiff diff = compareImages(src, got, ParityTolerance::exact());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << " in " << src[diff.firstMismatchIndex] << " out " << got[diff.firstMismatchIndex];
}

// A gamma at or below the floor renders as the floor, forward and reversed: pow(x, 1 / floor) for
// a positive x, x itself otherwise.
TEST_F(NativeGradeGpuTest, GammaAtOrBelowZeroIsRaisedToTheFloor)
{
    NodePtr node;
    Grade* grade = makeGrade(node);
    ASSERT_TRUE(grade != NULL);
    NodePtr floorNode;
    Grade* floorGrade = makeGrade(floorNode);
    ASSERT_TRUE(floorGrade != NULL);

    int width = 0;
    const std::vector<float> src = sweepImage(&width);
    for (int reverse = 0; reverse < 2; ++reverse) {
        GradeSetting below = kGammaBelowFloor;
        GradeSetting atFloor = kGammaAtFloor;
        below.reverse = atFloor.reverse = reverse != 0;
        applySetting(node, below);
        applySetting(floorNode, atFloor);
        if (HasFatalFailure()) {
            return;
        }

        const std::vector<float> got = cpuRender(grade, src, width, 1, 4, 0xF);
        const std::vector<float> floorGot = cpuRender(floorGrade, src, width, 1, 4, 0xF);
        const ImageDiff same = compareImages(floorGot, got, ParityTolerance::exact());
        EXPECT_EQ(0u, same.mismatches) << "reverse " << reverse << ": first at float " << same.firstMismatchIndex << " in " << src[same.firstMismatchIndex];

        std::vector<float> want(src.size());
        for (size_t i = 0; i < src.size(); ++i) {
            const double v = src[i];
            want[i] = (float)((v > 0) ? std::pow(v, reverse ? gradefix::kMinGamma : 1. / gradefix::kMinGamma) : v);
        }
        const ImageDiff analytic = compareImages(want, got, ParityTolerance::transcendental());
        EXPECT_EQ(0u, analytic.mismatches) << "reverse " << reverse << ": first at float " << analytic.firstMismatchIndex << " in " << src[analytic.firstMismatchIndex]
                                           << " got " << got[analytic.firstMismatchIndex] << " want " << want[analytic.firstMismatchIndex];
    }
}

#ifdef NATRON_HAVE_GPU

TEST_F(NativeGradeGpuTest, MatchesTheCpuKernelOnTheGradeFixtures)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node;
    Grade* grade = makeGrade(node);
    ASSERT_TRUE(grade != NULL);

    const std::vector<gradefix::GradeCase> cases = gradefix::makeCases();
    ASSERT_FALSE(cases.empty());
    const gradefix::Bounds bounds { gradefix::kUlpBound, true };
    uint64_t compared = 0;
    uint64_t passthrough = 0;
    uint64_t subnormal = 0;
    uint64_t violations = 0;
    std::vector<std::string> failures;

    for (size_t k = 0; k < cases.size(); ++k) {
        const gradefix::GradeCase& c = cases[k];
        const gradefix::GradeParams& p = c.p;
        GradeSetting setting;
        for (int i = 0; i < 4; ++i) {
            setting.multiply[i] = p.a[i];
            setting.offset[i] = p.b[i];
            setting.gamma[i] = c.knobGamma[i];
        }
        setting.reverse = (p.flags & gradefix::kReverse) != 0;
        setting.clampBlack = (p.flags & gradefix::kClampBlack) != 0;
        setting.clampWhite = (p.flags & gradefix::kClampWhite) != 0;
        applySetting(node, setting);
        if (HasFatalFailure()) {
            return;
        }

        const int width = (int)p.width;
        const int height = (int)p.height;
        const int nComps = (int)p.nComps;
        const std::vector<float> want = cpuRender(grade, c.src, width, height, nComps, p.channelMask);

        GpuQuery query = eligibleQuery();
        query.nComps = nComps;
        for (int i = 0; i < 4; ++i) {
            query.processChannels[i] = (p.channelMask >> i) & 1u;
        }
        GpuParityImage image;
        image.bounds = RectI(0, 0, width, height);
        image.nComps = nComps;
        image.pixels = c.src;
        const GpuRunResult run = runRenderGpu(*runtime, *grade, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
        ASSERT_TRUE(run.ok);
        EXPECT_EQ(0u, run.writtenOutside);
        const std::vector<float>& got = run.pixels;

        for (size_t i = 0; i < c.src.size(); ++i) {
            const int channel = (int)(i % nComps);
            const int bit = nComps == 1 ? 3 : channel;
            const bool processed = (p.channelMask >> bit) & 1u;
            if (!processed) {
                ++passthrough;
                if (std::memcmp(&got[i], &c.src[i], sizeof(float)) != 0) {
                    ++violations;
                    if (failures.size() < 10) {
                        failures.push_back("case " + std::to_string(k) + " element " + std::to_string(i) + " changed outside the channel mask");
                    }
                }
                continue;
            }
            const double in = c.src[i];
            if (in != 0 && std::fabs(in) < std::numeric_limits<float>::min()) {
                ++subnormal;
                continue;
            }
            ++compared;
            const graderef::Channel chan { p.a[bit], p.b[bit], p.gamma[bit] };
            const double conditioned = gradefix::conditionedTolerance(in, chan, setting.reverse, want[i], true);
            if (!gradefix::matches(got[i], want[i], conditioned, bounds)) {
                ++violations;
                if (failures.size() < 10) {
                    failures.push_back("case " + std::to_string(k) + " element " + std::to_string(i) + " in=" + std::to_string(in) + " gpu=" + std::to_string(got[i]) + " cpu=" + std::to_string(want[i]));
                }
            }
        }
    }

    std::printf("grade node gpu vs cpu: compared=%llu passthrough=%llu subnormal inputs skipped=%llu\n",
                (unsigned long long)compared, (unsigned long long)passthrough, (unsigned long long)subnormal);
    for (const std::string& f : failures) {
        ADD_FAILURE() << f;
    }
    EXPECT_GT(compared, 1000000u);
    EXPECT_EQ(0u, violations);
}

// Subnormal inputs are skipped: a device may flush them to zero.
TEST_F(NativeGradeGpuTest, DefaultKnobsPassEveryValueThroughOnTheGpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node;
    Grade* grade = makeGrade(node);
    ASSERT_TRUE(grade != NULL);

    int width = 0;
    const std::vector<float> src = sweepImage(&width);
    GpuParityImage image;
    image.bounds = RectI(0, 0, width, 1);
    image.nComps = 4;
    image.pixels = src;
    const GpuRunResult run = runRenderGpu(*runtime, *grade, eligibleQuery(), std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
    ASSERT_TRUE(run.ok);
    EXPECT_EQ(0u, run.writtenOutside);
    ASSERT_EQ(src.size(), run.pixels.size());

    const ParityTolerance exact = ParityTolerance::exact();
    size_t compared = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        if ((src[i] != 0) && (std::fabs(src[i]) < std::numeric_limits<float>::min())) {
            continue;
        }
        ++compared;
        EXPECT_TRUE(exact.accepts(src[i], run.pixels[i])) << "float " << i << " in " << src[i] << " gpu " << run.pixels[i];
    }
    EXPECT_GT(compared, src.size() / 2);
}

TEST_F(NativeGradeGpuTest, GammaAtOrBelowZeroMatchesTheCpuKernel)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node;
    Grade* grade = makeGrade(node);
    ASSERT_TRUE(grade != NULL);

    int width = 0;
    const std::vector<float> src = sweepImage(&width);
    const gradefix::Bounds bounds { gradefix::kUlpBound, true };
    const graderef::Channel chan { 1., 0., gradefix::hostGamma(0.f) };
    for (int variant = 0; variant < 4; ++variant) {
        GradeSetting setting = kGammaBelowFloor;
        setting.reverse = (variant & 1) != 0;
        setting.clampBlack = setting.clampWhite = (variant & 2) != 0;
        applySetting(node, setting);
        if (HasFatalFailure()) {
            return;
        }
        const std::vector<float> want = cpuRender(grade, src, width, 1, 4, 0xF);

        GpuParityImage image;
        image.bounds = RectI(0, 0, width, 1);
        image.nComps = 4;
        image.pixels = src;
        const GpuRunResult run = runRenderGpu(*runtime, *grade, eligibleQuery(), std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
        ASSERT_TRUE(run.ok);
        EXPECT_EQ(0u, run.writtenOutside);
        ASSERT_EQ(src.size(), run.pixels.size());

        size_t failures = 0;
        for (size_t i = 0; i < src.size(); ++i) {
            const double in = src[i];
            if ((in != 0) && (std::fabs(in) < std::numeric_limits<float>::min())) {
                continue;
            }
            const double conditioned = gradefix::conditionedTolerance(in, chan, setting.reverse, want[i], true);
            if (!gradefix::matches(run.pixels[i], want[i], conditioned, bounds) && (++failures < 10)) {
                ADD_FAILURE() << "variant " << variant << " float " << i << " in " << in << " gpu " << run.pixels[i] << " cpu " << want[i];
            }
        }
        EXPECT_EQ(0u, failures) << "variant " << variant;
    }
}

TEST_F(NativeGradeGpuTest, HonoursOffsetsAndStridesOfTheViews)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node;
    Grade* grade = makeGrade(node);
    ASSERT_TRUE(grade != NULL);

    GradeSetting setting = { { 1.5, 0.75, 2., 1.25 }, { 0.1, -0.05, 0.2, 0. }, { 2.2, 1.8, 0.8, 1. }, false, true, true };
    applySetting(node, setting);
    if (HasFatalFailure()) {
        return;
    }

    const int nComps = 4;
    const RectI srcBounds(10, 20, 10 + 21, 20 + 13);
    const int dstWidth = 25;
    const int dstHeight = 17;
    const RectI rect(13, 22, 13 + 11, 22 + 7);

    std::vector<float> packedSrc((size_t)srcBounds.width() * srcBounds.height() * nComps);
    for (size_t i = 0; i < packedSrc.size(); ++i) {
        packedSrc[i] = -0.5f + (float)((i * 37) % 211) / 100.f;
    }
    std::vector<float> packed((size_t)rect.width() * rect.height() * nComps);
    for (int y = 0; y < rect.height(); ++y) {
        for (int x = 0; x < rect.width(); ++x) {
            for (int c = 0; c < nComps; ++c) {
                packed[((size_t)y * rect.width() + x) * nComps + c] = packedSrc[(size_t)(rect.y1 - srcBounds.y1 + y) * srcBounds.width() * nComps + (size_t)(rect.x1 - srcBounds.x1 + x) * nComps + c];
            }
        }
    }
    const std::vector<float> want = cpuRender(grade, packed, rect.width(), rect.height(), nComps, 0xF);

    GpuParityImage in;
    in.bounds = srcBounds;
    in.nComps = nComps;
    in.layout.padFloats = 16;
    in.layout.rowPadFloats = 3;
    in.pixels = packedSrc;

    GpuViewLayout layout;
    layout.padFloats = 8;
    layout.rowPadFloats = 5;
    // The window sits 7 pixels and 4 rows into the destination.
    layout.marginLeft = 7;
    layout.marginBottom = 4;
    layout.marginRight = dstWidth - 7 - rect.width();
    layout.marginTop = dstHeight - 4 - rect.height();
    GpuQuery query;
    query.nComps = nComps;
    const GpuRunResult run = runRenderGpu(*runtime, *grade, query, std::vector<GpuParityImage>(1, in), nullptr, rect, layout);
    ASSERT_TRUE(run.ok);
    EXPECT_EQ(0u, run.writtenOutside) << "floats outside the window were written";

    const std::vector<float>& got = run.pixels;
    ASSERT_EQ(packed.size(), got.size());
    const gradefix::Bounds bounds { gradefix::kUlpBound, true };
    for (size_t i = 0; i < got.size(); ++i) {
        const int bit = (int)(i % nComps);
        const graderef::Channel chan { setting.multiply[bit], setting.offset[bit], setting.gamma[bit] };
        const double conditioned = gradefix::conditionedTolerance(packed[i], chan, false, want[i], true);
        EXPECT_TRUE(gradefix::matches(got[i], want[i], conditioned, bounds))
            << "window float " << i << " gpu=" << got[i] << " cpu=" << want[i];
    }
}

namespace {

struct WrapCase {
    const char* name;
    // 0 none, 1 an RGBA mask image, 2 an alpha-only mask image
    int maskKind;
    bool invert;
    double mix;
    bool unPremult;
    unsigned channelMask;
    GradeSetting setting;
};

const GradeSetting kLinear = { { 1.5, 0.75, 2., 1.25 }, { 0.1, -0.05, 0.2, 0.05 }, { 1., 1., 1., 1. }, false, false, false };
const GradeSetting kGamma = { { 1.5, 0.75, 2., 1.25 }, { 0.1, -0.05, 0.2, 0. }, { 2.2, 1.8, 0.8, 1.4 }, false, true, true };
const GradeSetting kReverse = { { 1.5, 0.75, 2., 1.25 }, { 0.1, -0.05, 0.2, 0. }, { 2.2, 1.8, 0.8, 1.4 }, true, true, false };

float
sampleValue(size_t i,
            float scale)
{
    if (i % 29 == 0) {
        return 0.f;
    }

    return scale * (-0.4f + (float)((i * 37) % 211) / 100.f);
}

// The pixels of a window of `bounds`, with alpha ramps that include exact zero and values the
// "(Un)premult by" division refuses.
std::vector<float>
makeImage(const RectI& bounds,
          int nComps,
          bool alphaRamp)
{
    std::vector<float> px((size_t)bounds.area() * nComps);
    for (size_t i = 0; i < px.size(); ++i) {
        const int c = (int)(i % nComps);
        const size_t pixel = i / nComps;
        if (alphaRamp && (c == nComps - 1)) {
            px[i] = (pixel % 17 == 0) ? 0.f : ((pixel % 13 == 0) ? 1e-8f : 0.1f + (float)((pixel * 7) % 90) / 100.f);
        } else {
            px[i] = sampleValue(i, 1.f);
        }
    }

    return px;
}

} // namespace

TEST_F(NativeGradeGpuTest, WrapsMaskMixPremultAndChannelsAroundTheGrade)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }

    const WrapCase cases[] = {
        { "rgba mask", 1, false, 1., false, 0xF, kLinear },
        { "rgba mask inverted", 1, true, 1., false, 0xF, kGamma },
        { "alpha mask", 2, false, 1., false, 0xF, kGamma },
        { "alpha mask inverted", 2, true, 1., false, 0xF, kReverse },
        { "mask and mix", 1, false, 0.5, false, 0xF, kGamma },
        { "mix 0.5", 0, false, 0.5, false, 0xF, kGamma },
        { "mix 0.5 reverse", 0, false, 0.5, false, 0xF, kReverse },
        { "mix 0", 0, false, 0., false, 0xF, kGamma },
        { "unpremult by alpha", 0, false, 1., true, 0xF, kLinear },
        { "unpremult by alpha, mix 0.5", 0, false, 0.5, true, 0xF, kLinear },
        { "unpremult, mask and mix", 2, true, 0.5, true, 0x7, kLinear },
        { "channel subset rg", 0, false, 1., false, 0x3, kGamma },
        { "channel subset a with mask", 1, false, 1., false, 0x8, kGamma },
        { "channel subset rb mix", 0, false, 0.75, false, 0x5, kReverse },
    };

    const int nComps = 4;
    // The source covers less than the window, and the mask less than the source.
    const RectI window(0, 0, 30, 20);
    const RectI srcBounds(4, 3, 4 + 21, 3 + 11);
    const RectI maskBounds(10, 6, 10 + 14, 6 + 9);
    const std::vector<float> srcPixels = makeImage(srcBounds, nComps, true);
    const std::vector<float> maskRgba = makeImage(maskBounds, 4, false);
    std::vector<float> maskAlpha((size_t)maskBounds.area());
    for (size_t i = 0; i < maskAlpha.size(); ++i) {
        maskAlpha[i] = (i % 5 == 0) ? 1.f : (float)((i * 11) % 101) / 100.f;
    }
    std::vector<float> maskRgbaClamped = maskRgba;
    for (size_t i = 3; i < maskRgbaClamped.size(); i += 4) {
        maskRgbaClamped[i] = (i % 6 == 3) ? 1.f : (float)((i * 11) % 101) / 100.f;
    }

    const gradefix::Bounds bounds { gradefix::kUlpBound, true };
    NodePtr rgbaSource = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(rgbaSource));
    setParitySourceComponents(rgbaSource, "rgba");
    uint64_t compared = 0;

    for (const WrapCase& c : cases) {
        NodePtr node;
        Grade* grade = makeGrade(node);
        ASSERT_TRUE(grade != NULL);
        connectNodes(rgbaSource, node, 0, true);
        applySetting(node, c.setting);
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { c.mix }));
        ASSERT_TRUE(setKnobValues(node, kOfxMaskInvertParamName, { c.invert ? 1. : 0. }));
        if (c.maskKind) {
            NodePtr maskSource = createNode(QString::fromUtf8(kTestPluginIDParitySource));
            ASSERT_TRUE(bool(maskSource));
            setParitySourceComponents(maskSource, c.maskKind == 1 ? "rgba" : "alpha");
            const int maskInput = grade->getMaskInput();
            ASSERT_GE(maskInput, 0);
            connectNodes(maskSource, node, maskInput, true);
            ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
            KnobChannelSelect* channel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
            ASSERT_TRUE(channel != NULL);
            channel->set("rgba.A");
        }
        if (c.unPremult) {
            KnobChannelSelect* by = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("hostUnPremultBy").get());
            ASSERT_TRUE(by != NULL);
            by->set("rgba.A");
        }
        if (HasFatalFailure()) {
            return;
        }

        GpuQuery query = eligibleQuery();
        query.mix = c.mix;
        query.hasMask = c.maskKind != 0;
        query.maskInPlane = true;
        for (int i = 0; i < 4; ++i) {
            query.processChannels[i] = (c.channelMask >> i) & 1u;
        }
        if (c.unPremult) {
            query.unPremultChannel = 3;
            query.unPremultInPlane = true;
        }
        ASSERT_TRUE(grade->canRenderOnGpu(query)) << c.name;

        GpuParityImage src;
        src.bounds = srcBounds;
        src.nComps = nComps;
        src.pixels = srcPixels;
        src.layout.padFloats = 4;
        src.layout.rowPadFloats = 2;
        GpuParityImage mask;
        mask.bounds = maskBounds;
        mask.nComps = c.maskKind == 2 ? 1 : 4;
        mask.pixels = c.maskKind == 2 ? maskAlpha : maskRgbaClamped;
        const GpuViewLayout outLayout;
        std::vector<GpuParityImage> inputs(1, src);
        const GpuRunResult run = runRenderGpu(*runtime, *grade, query, inputs, c.maskKind ? &mask : nullptr, window, outLayout);
        ASSERT_TRUE(run.ok) << c.name;
        EXPECT_EQ(0u, run.writtenOutside) << c.name;
        ASSERT_EQ((size_t)window.area() * nComps, run.pixels.size()) << c.name;

        size_t failures = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                float s[4] = { 0, 0, 0, 0 };
                if (srcBounds.contains(x, y)) {
                    const size_t at = ((size_t)(y - srcBounds.y1) * srcBounds.width() + (x - srcBounds.x1)) * nComps;
                    for (int k = 0; k < 4; ++k) {
                        s[k] = srcPixels[at + k];
                    }
                }
                float m = 0.f;
                if (c.maskKind && maskBounds.contains(x, y)) {
                    const size_t at = (size_t)(y - maskBounds.y1) * maskBounds.width() + (x - maskBounds.x1);
                    m = c.maskKind == 2 ? maskAlpha[at] : maskRgbaClamped[at * 4 + 3];
                }
                // render() reads the divisor as 1 outside the source, and multiplies back by
                // max(0, d) even where d was too small to divide by.
                const float d = srcBounds.contains(x, y) ? s[3] : 1.f;
                const bool divide = c.unPremult && d > FLT_EPSILON;
                std::vector<float> in(s, s + 4);
                if (divide) {
                    for (int k = 0; k < 3; ++k) {
                        in[k] = s[k] / d;
                    }
                }
                const std::vector<float> g = cpuRender(grade, in, 1, 1, 4, c.channelMask);
                double alpha = c.mix;
                if (c.maskKind) {
                    alpha = (c.invert ? 1.0 - m : m) * c.mix;
                }
                const size_t outAt = ((size_t)(y - window.y1) * window.width() + (x - window.x1)) * nComps;
                for (int k = 0; k < 4; ++k) {
                    const float got = run.pixels[outAt + k];
                    if (!((c.channelMask >> k) & 1u)) {
                        if (std::memcmp(&got, &s[k], sizeof(float)) != 0) {
                            ++failures;
                            ADD_FAILURE() << c.name << " (" << x << "," << y << ") channel " << k << " changed outside the channel mask";
                        }
                        continue;
                    }
                    const bool scaled = divide && k != 3;
                    const double back = (c.unPremult && k != 3) ? std::max(0.f, d) : 1.;
                    double v = g[k] * back;
                    const graderef::Channel chan { c.setting.multiply[k], c.setting.offset[k], c.setting.gamma[k] };
                    double tol = gradefix::conditionedTolerance(in[k], chan, c.setting.reverse, g[k], true) * back;
                    if (scaled) {
                        tol += 3. * FLT_EPSILON * (std::fabs(in[k]) * std::fabs(chan.a) + std::fabs(g[k])) * back;
                    }
                    double want;
                    if (alpha == 0.) {
                        want = s[k];
                        tol = 0.;
                    } else if (alpha == 1.) {
                        want = v;
                    } else {
                        want = v * alpha + (1. - alpha) * s[k];
                        tol = tol * alpha + 4. * FLT_EPSILON * std::max(std::fabs(v * alpha), std::fabs((1. - alpha) * s[k]));
                    }
                    ++compared;
                    if (!gradefix::matches(got, want, tol, bounds)) {
                        ++failures;
                        if (failures < 10) {
                            ADD_FAILURE() << c.name << " (" << x << "," << y << ") channel " << k << " gpu=" << got << " want=" << want;
                        }
                    }
                }
            }
        }
        EXPECT_EQ(0u, failures) << c.name;
    }
    EXPECT_GT(compared, 0u);
}

#endif // NATRON_HAVE_GPU
