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
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorMatrix.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/Premult.h"
#include "Engine/Nodes/Color/Threshold.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

const char* const kOutputKnobs[4] = {
    kColorMatrixParamOutputRed, kColorMatrixParamOutputGreen, kColorMatrixParamOutputBlue, kColorMatrixParamOutputAlpha
};

struct MatrixSetting {
    double m[4][4];
    bool clampBlack;
    bool clampWhite;
};

const MatrixSetting kMatrixSettings[] = {
    { { { 1., 0., 0., 0. }, { 0., 1., 0., 0. }, { 0., 0., 1., 0. }, { 0., 0., 0., 1. } }, true, false },
    { { { 1.75, -0.4, 0.25, 0.5 }, { 0.3, 2.5, -0.8, 0. }, { -1.2, 0.1, 3., -0.25 }, { 0.2, 0.3, 0.1, 0.6 } }, false, false },
    { { { 0.6, 0.3, 0.1, 0. }, { -0.5, 1.5, 0., 0.2 }, { 0., 0., 0.5, 0. }, { 1., 1., 1., -1. } }, true, true },
    { { { 2., 0., 0., 0. }, { 0., 0., 1., 0. }, { 0., 1., 0., 0. }, { 0.25, 0.25, 0.25, 0.25 } }, false, true },
};

struct ThresholdSetting {
    double level[4];
    double softness[4];
};

// Softness 0 at levels the test values hit exactly, then ramps of several widths.
const ThresholdSetting kThresholdSettings[] = {
    { { 0.5, 1., 0., 0.5 }, { 0., 0., 0., 0. } },
    { { 0.5, 0.25, 1.2, 0.5 }, { 0.25, 0.5, 0.4, 0.1 } },
    { { -0.5, 2., 0.3333, 0. }, { 1., 0.75, 0.05, 2. } },
};

void
applyMatrix(const NodePtr& node,
            const MatrixSetting& s)
{
    for (int r = 0; r < 4; ++r) {
        ASSERT_TRUE(setKnobValues(node, kOutputKnobs[r], std::vector<double>(s.m[r], s.m[r] + 4)));
    }
    ASSERT_TRUE(setKnobValues(node, kColorMatrixParamClampBlack, std::vector<double>(1, s.clampBlack ? 1. : 0.)));
    ASSERT_TRUE(setKnobValues(node, kColorMatrixParamClampWhite, std::vector<double>(1, s.clampWhite ? 1. : 0.)));
}

void
applyThreshold(const NodePtr& node,
               const ThresholdSetting& s)
{
    ASSERT_TRUE(setKnobValues(node, kThresholdParamLevel, std::vector<double>(s.level, s.level + 4)));
    ASSERT_TRUE(setKnobValues(node, kThresholdParamSoftness, std::vector<double>(s.softness, s.softness + 4)));
}

bool
renderRgba(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

#ifdef NATRON_HAVE_GPU
GpuParityImage
imageOf(const RenderedPlane& plane)
{
    GpuParityImage image;

    image.bounds = plane.window;
    image.nComps = (int)plane.channels.size();
    image.pixels = plane.pixels;

    return image;
}

GpuQuery
queryFor(const NodePtr& op)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get());
    const ImageLayerDesc plane = ImageLayerDesc::mapNCompsToColorLayer(4);
    GpuQuery q;

    q.time = kTime;
    q.view = ViewIdx(0);
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;
    const int maskInput = effect->getMaskInput();
    q.hasMask = (maskInput >= 0) && effect->getInput(maskInput) && effect->isMaskEnabled(maskInput);
    if (q.hasMask) {
        ImageLayerDesc maskLayer;
        int maskChannel = -1;
        q.maskInPlane = !effect->resolveInputPlaneForRender(maskInput, kTime, ViewIdx(0), &maskLayer, &maskChannel) || (maskChannel < 0) || (Node::getUnPremultSkipChannel(plane, maskLayer, maskChannel) >= 0);
    }
    KnobDoublePtr mix = std::dynamic_pointer_cast<KnobDouble>(effect->getKnobByName(kOfxMixParamName));
    q.mix = mix ? mix->getValueAtTime(kTime, 0, ViewIdx(0)) : 1.;
    const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), plane);
    for (int i = 0; i < 4; ++i) {
        q.processChannels[i] = channels[i];
    }
    KnobChannelSelectPtr unPremultBy = op->getUnPremultBySelector();
    if (unPremultBy && !unPremultBy->isNone() && effect->getInput(0)) {
        std::list<ImageLayerDesc> available;
        effect->getAvailableLayers(kTime, ViewIdx(0), 0, &available);
        ImageLayerDesc divisorLayer;
        const int divisorChannel = op->getUnPremultChannel(available, &divisorLayer);
        if (divisorLayer.getNumComponents() > 0) {
            q.unPremultChannel = divisorChannel;
            q.unPremultInPlane = (divisorChannel >= 0) && (Node::getUnPremultSkipChannel(plane, divisorLayer, divisorChannel) >= 0);
        }
    }

    return q;
}

// Runs `row` over every row of `src` the way the CPU stages hand rows to an op, with `divisor`
// (one value per pixel, or empty) as the divisor row.
std::vector<float>
cpuRows(const std::vector<float>& src,
        const std::vector<float>& divisor,
        int width,
        int height,
        int nComps,
        unsigned channelMask,
        const std::function<void(const RowIO&)>& row)
{
    const std::bitset<4> channels(channelMask);
    std::vector<float> dst(src);

    for (int y = 0; y < height; ++y) {
        RowIO io;
        io.nSrc = 1;
        io.src[0] = src.data() + (size_t)y * width * nComps;
        io.dst = dst.data() + (size_t)y * width * nComps;
        io.divisor = divisor.empty() ? NULL : divisor.data() + (size_t)y * width;
        io.y = y;
        io.width = width;
        io.nComps = nComps;
        io.channels = channels;
        row(io);
    }

    return dst;
}

// Values on both sides of 0 and 1 and far beyond, in a fixed pseudo-random order so that the
// channels of a pixel differ.
std::vector<float>
testValues()
{
    std::vector<float> values = { 0.f, 1.f, -0.25f, 0.5f, 1.5f, 0.1f, 0.9f, -1.f, 2.f, 0.3333f, 0.7f, 0.05f, 2.75f, 0.2f, -0.6f, 0.45f, 1.1f };

    while (((values.size() % 2) == 0) || ((values.size() % 3) == 0)) {
        values.push_back(0.8f);
    }

    return values;
}

std::vector<float>
testImage(int width,
          int height,
          int nComps,
          int seed)
{
    const std::vector<float> values = testValues();
    std::vector<float> src((size_t)width * height * nComps);

    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = values[(i * 7 + (size_t)seed) % values.size()];
    }

    return src;
}

// The channel bits an image of nComps channels holds.
unsigned
presentBits(int nComps)
{
    switch (nComps) {
    case 1:
        return 0x8u;
    case 3:
        return 0x7u;
    default:
        return 0xFu;
    }
}

GpuQuery
plainQuery(int nComps,
           unsigned channelMask)
{
    GpuQuery query;

    query.time = kTime;
    query.nComps = nComps;
    for (int i = 0; i < 4; ++i) {
        query.processChannels[i] = ((channelMask >> i) & 1u) != 0;
    }

    return query;
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeColourUtilGpuTest
    : public BaseTest {
#ifdef NATRON_HAVE_GPU
protected:
    // Runs the node's kernel over `src` and compares it with `want`.
    void expectKernel(GpuParityRuntime& runtime,
                      StagedImageEffect& effect,
                      const GpuQuery& query,
                      const std::vector<float>& src,
                      int width,
                      int height,
                      const std::vector<float>& want,
                      const std::string& what)
    {
        ASSERT_TRUE(effect.canRenderOnGpu(query)) << what;
        GpuParityImage image;
        image.bounds = RectI(0, 0, width, height);
        image.nComps = query.nComps;
        image.pixels = src;
        const GpuRunResult run = runRenderGpu(runtime, effect, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
        ASSERT_TRUE(run.ok) << what;
        EXPECT_EQ(0u, run.writtenOutside) << what;
        ASSERT_EQ(want.size(), run.pixels.size()) << what;
        const ImageDiff diff = compareImages(want, run.pixels, ParityTolerance::arithmetic());
        EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << " in " << src[diff.firstMismatchIndex] << " gpu "
                                       << run.pixels[diff.firstMismatchIndex] << " cpu " << want[diff.firstMismatchIndex];
    }

    NodePtr makeMaskedOp(const char* id,
                         NodePtr* source,
                         NodePtr* mask)
    {
        *source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        *mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr op = createNode(QString::fromUtf8(id), 1);
        EXPECT_TRUE(bool(*source) && bool(*mask) && bool(op));
        if (!*source || !*mask || !op) {
            return NodePtr();
        }
        setParitySourceOrigin(*mask, 8, 4);
        connectNodes(*source, op, 0, true);
        int maskInput = -1;
        for (int i = 0; i < op->getNInputs(); ++i) {
            if (op->getInputLabel(i) == "Mask") {
                maskInput = i;
            }
        }
        EXPECT_GE(maskInput, 0);
        if (maskInput < 0) {
            return NodePtr();
        }
        connectNodes(*mask, op, maskInput, true);
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(op->getKnobByName("maskChannel_Mask").get());
        EXPECT_TRUE(maskChannel != NULL);
        if (maskChannel) {
            maskChannel->set("rgba.A");
        }

        return op;
    }
#endif // NATRON_HAVE_GPU
};

#ifdef NATRON_HAVE_GPU

TEST_F(NativeColourUtilGpuTest, ColorMatrixKernelMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORMATRIX), PLUGIN_MAJOR_NATRON_COLORMATRIX);
    ASSERT_TRUE(bool(node));
    ColorMatrix* effect = dynamic_cast<ColorMatrix*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    const unsigned channelMasks[] = { 0xF, 0x7, 0x5, 0x8 };
    const int width = 32;
    const int height = 9;
    int nCase = 0;
    for (const MatrixSetting& setting : kMatrixSettings) {
        applyMatrix(node, setting);
        if (HasFatalFailure()) {
            return;
        }
        const ColorMatrixOp::Params params = effect->bakeColorMatrix(kTime, ViewIdx(0));
        for (int nComps : { 1, 3, 4 }) {
            for (unsigned channelMask : channelMasks) {
                if ((channelMask & presentBits(nComps)) == 0) {
                    continue;
                }
                const std::vector<float> src = testImage(width, height, nComps, nCase);
                const std::vector<float> want = cpuRows(src, std::vector<float>(), width, height, nComps, channelMask, [effect, &params](const RowIO& io) { effect->colorMatrixRow(params, io); });
                char what[96];
                std::snprintf(what, sizeof(what), "matrix %d, components %d, channels %x", nCase, nComps, channelMask);
                expectKernel(*runtime, *effect, plainQuery(nComps, channelMask), src, width, height, want, what);
            }
        }
        ++nCase;
    }
}

TEST_F(NativeColourUtilGpuTest, ThresholdKernelMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_THRESHOLD), PLUGIN_MAJOR_NATRON_THRESHOLD);
    ASSERT_TRUE(bool(node));
    Threshold* effect = dynamic_cast<Threshold*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    const unsigned channelMasks[] = { 0xF, 0x7, 0x5, 0x8 };
    const int width = 32;
    const int height = 9;
    int nCase = 0;
    for (const ThresholdSetting& setting : kThresholdSettings) {
        applyThreshold(node, setting);
        if (HasFatalFailure()) {
            return;
        }
        const ThresholdOp::Params params = effect->bakeThreshold(kTime, ViewIdx(0));
        for (int nComps : { 1, 3, 4 }) {
            for (unsigned channelMask : channelMasks) {
                if ((channelMask & presentBits(nComps)) == 0) {
                    continue;
                }
                const std::vector<float> src = testImage(width, height, nComps, nCase);
                const std::vector<float> want = cpuRows(src, std::vector<float>(), width, height, nComps, channelMask, [effect, &params](const RowIO& io) { effect->thresholdRow(params, io); });
                char what[96];
                std::snprintf(what, sizeof(what), "threshold %d, components %d, channels %x", nCase, nComps, channelMask);
                expectKernel(*runtime, *effect, plainQuery(nComps, channelMask), src, width, height, want, what);
            }
        }
        ++nCase;
    }
}

// The divisor is the image's own alpha, as the GPU reads it from the processed plane. Alphas of 0,
// below and at FLT_EPSILON, negative and above 1 cover the pass-through and the plain product.
TEST_F(NativeColourUtilGpuTest, PremultAndUnpremultKernelsMatchTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const float alphas[] = { 0.f, 1e-9f, FLT_EPSILON, 2.f * FLT_EPSILON, 0.25f, 0.5f, 1.f, 2.f, -0.5f, 0.0625f, 0.9f };
    const int nAlphas = (int)(sizeof(alphas) / sizeof(alphas[0]));
    const int width = 32;
    const int height = 9;
    const int nComps = 4;
    const unsigned channelMasks[] = { 0x7, 0xF, 0x5, 0x8 };

    for (int divide = 0; divide < 2; ++divide) {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr node = createNode(QString::fromUtf8(divide ? PLUGINID_NATRON_UNPREMULT : PLUGINID_NATRON_PREMULT), PLUGIN_MAJOR_NATRON_PREMULT);
        ASSERT_TRUE(bool(source) && bool(node));
        // The divisor is resolved against the connected input's layers.
        connectNodes(source, node, 0, true);
        StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL);

        std::vector<float> src = testImage(width, height, nComps, divide);
        std::vector<float> divisor((size_t)width * height);
        for (size_t p = 0; p < divisor.size(); ++p) {
            src[p * nComps + 3] = alphas[(p * 5) % nAlphas];
            divisor[p] = src[p * nComps + 3];
        }
        for (unsigned channelMask : channelMasks) {
            std::vector<float> want;
            if (divide) {
                Unpremult* op = dynamic_cast<Unpremult*>(effect);
                ASSERT_TRUE(op != NULL);
                want = cpuRows(src, divisor, width, height, nComps, channelMask, [op](const RowIO& io) { op->premultRow(io); });
            } else {
                Premult* op = dynamic_cast<Premult*>(effect);
                ASSERT_TRUE(op != NULL);
                want = cpuRows(src, divisor, width, height, nComps, channelMask, [op](const RowIO& io) { op->premultRow(io); });
            }
            GpuQuery query = plainQuery(nComps, channelMask);
            query.unPremultChannel = 3;
            query.unPremultInPlane = true;
            char what[96];
            std::snprintf(what, sizeof(what), "%s, channels %x", divide ? "Unpremult" : "Premult", channelMask);
            expectKernel(*runtime, *effect, query, src, width, height, want, what);
        }
    }
}

// The wrapped render of each node against its own render() over the parity sources: a plain
// render, a mask, an inverted mask, mix 0.5, and for the nodes with a Premult stage a divide by
// alpha. Premult and Unpremult always have their divisor, the alpha of the processed plane.
TEST_F(NativeColourUtilGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    struct Case {
        const char* name;
        bool masked;
        bool inverted;
        double mix;
        bool divide;
    };
    const Case cases[] = {
        { "plain", false, false, 1., false },
        { "mask", true, false, 1., false },
        { "inverted mask", true, true, 1., false },
        { "mix 0.5", false, false, 0.5, false },
        { "divide by A", false, false, 1., true },
        { "masked, mix 0.5, divide by A", true, false, 0.5, true },
    };
    const char* const ids[] = { PLUGINID_NATRON_PREMULT, PLUGINID_NATRON_UNPREMULT, PLUGINID_NATRON_COLORMATRIX, PLUGINID_NATRON_THRESHOLD };
    int nCase = 0;

    for (const char* id : ids) {
        const bool divisorStage = (std::string(id) == PLUGINID_NATRON_PREMULT) || (std::string(id) == PLUGINID_NATRON_UNPREMULT);
        NodePtr source;
        NodePtr mask;
        NodePtr op = makeMaskedOp(id, &source, &mask);
        ASSERT_TRUE(bool(op)) << id;
        StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL) << id;
        KnobChannelSelect* unPremultBy = dynamic_cast<KnobChannelSelect*>(op->getKnobByName(kUnPremultByKnobName).get());
        ASSERT_TRUE(unPremultBy != NULL) << id;
        if (std::string(id) == PLUGINID_NATRON_COLORMATRIX) {
            applyMatrix(op, kMatrixSettings[1]);
        } else if (std::string(id) == PLUGINID_NATRON_THRESHOLD) {
            applyThreshold(op, kThresholdSettings[1]);
        } else {
            // Alpha too, so the divisor channel is itself processed.
            ASSERT_TRUE(setKnobValues(op, kNatronOfxParamProcessA, std::vector<double>(1, 1.)));
        }
        if (HasFatalFailure()) {
            return;
        }

        const RectI window = paritySourceWindow(source, kTime, 0);
        RenderedPlane sourcePlane;
        RenderedPlane maskPlane;
        ASSERT_TRUE(renderRgba(source, window, &sourcePlane));
        ASSERT_TRUE(renderRgba(mask, paritySourceWindow(mask, kTime, 0), &maskPlane));

        for (const Case& c : cases) {
            ASSERT_TRUE(setKnobValues(op, "enableMask_Mask", std::vector<double>(1, c.masked ? 1. : 0.)));
            ASSERT_TRUE(setKnobValues(op, kOfxMaskInvertParamName, std::vector<double>(1, c.inverted ? 1. : 0.)));
            ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, std::vector<double>(1, c.mix)));
            if (c.divide || divisorStage) {
                unPremultBy->set("rgba.A");
            } else {
                unPremultBy->setNone();
            }
            const GpuQuery query = queryFor(op);
            ASSERT_TRUE(effect->canRenderOnGpu(query)) << id << " " << c.name;

            RenderedPlane cpu;
            ASSERT_TRUE(renderRgba(op, window, &cpu)) << id << " " << c.name;

            std::vector<GpuParityImage> inputs(1, imageOf(sourcePlane));
            GpuParityImage maskImage = imageOf(maskPlane);
            GpuViewLayout layout;
            if (nCase++ % 2) {
                inputs[0].layout.padFloats = 16;
                inputs[0].layout.rowPadFloats = 3;
                maskImage.layout.padFloats = 5;
                maskImage.layout.rowPadFloats = 7;
                layout.padFloats = 8;
                layout.rowPadFloats = 5;
                layout.marginLeft = 3;
                layout.marginBottom = 2;
                layout.marginRight = 4;
                layout.marginTop = 1;
            }
            const GpuRunResult run = runRenderGpu(*runtime, *effect, query, inputs, query.hasMask ? &maskImage : nullptr, window, layout);
            ASSERT_TRUE(run.ok) << id << " " << c.name;
            EXPECT_EQ(0u, run.writtenOutside) << id << " " << c.name;
            ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << id << " " << c.name;

            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, ParityTolerance::arithmetic());
            EXPECT_EQ(0u, diff.mismatches) << id << " " << c.name << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex]
                                           << " cpu " << cpu.pixels[diff.firstMismatchIndex];
        }
    }
}

#endif // NATRON_HAVE_GPU

// A Grade, the four nodes and another Grade run as one GPU region and match the CPU frame.
TEST_F(NativeColourUtilGpuTest, ChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr unpremult = createNode(QString::fromUtf8(PLUGINID_NATRON_UNPREMULT), PLUGIN_MAJOR_NATRON_PREMULT);
    NodePtr matrix = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORMATRIX), PLUGIN_MAJOR_NATRON_COLORMATRIX);
    NodePtr threshold = createNode(QString::fromUtf8(PLUGINID_NATRON_THRESHOLD), PLUGIN_MAJOR_NATRON_THRESHOLD);
    NodePtr premult = createNode(QString::fromUtf8(PLUGINID_NATRON_PREMULT), PLUGIN_MAJOR_NATRON_PREMULT);
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(first) && bool(unpremult) && bool(matrix) && bool(threshold) && bool(premult) && bool(second));
    const std::vector<NodePtr> chain({ first, unpremult, matrix, threshold, premult, second });
    connectNodes(source, first, 0, true);
    for (std::size_t i = 1; i < chain.size(); ++i) {
        connectNodes(chain[i - 1], chain[i], 0, true);
    }
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    applyMatrix(matrix, kMatrixSettings[1]);
    applyThreshold(threshold, kThresholdSettings[2]);
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));
    if (HasFatalFailure()) {
        return;
    }

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(chain, second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        int held = 0;
        for (const NodePtr& node : chain) {
            if (std::find(nodes.begin(), nodes.end(), node) != nodes.end()) {
                ++held;
            }
        }
        if (held > 0) {
            ++holding;
            EXPECT_EQ((int)chain.size(), held);
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::arithmetic());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}
