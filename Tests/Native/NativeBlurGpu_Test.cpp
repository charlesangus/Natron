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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

} // namespace

class NativeBlurGpuTest
    : public BaseTest {
protected:
    // A 64x48 project format, as in NativeBlurTest, so cropToFormat has a format to crop to.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeBlurGpu64x48", 1.));
    }

    NodePtr makeBlur()
    {
        NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
        EXPECT_TRUE(bool(blur));

        return blur;
    }

    static Blur* blurOf(const NodePtr& node)
    {
        return node ? dynamic_cast<Blur*>(node->getEffectInstance().get()) : NULL;
    }
};

TEST_F(NativeBlurGpuTest, EligibilityFollowsTheSupportedSettings)
{
    resetProject();
    NodePtr node = makeBlur();
    Blur* blur = blurOf(node);
    ASSERT_TRUE(blur != NULL);
    ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 10., 10. }));

#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, blur->canRenderOnGpu(q)) << nComps << " components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.processChannels[1] = false;
        q.processChannels[2] = false;
        EXPECT_EQ(supported, blur->canRenderOnGpu(q)) << "a channel subset";
    }
    {
        GpuQuery q = eligibleQuery();
        q.hasMask = true;
        EXPECT_EQ(supported, blur->canRenderOnGpu(q)) << "a mask channel outside the plane, read in its own layer";
        q.maskInPlane = true;
        EXPECT_EQ(supported, blur->canRenderOnGpu(q)) << "mask";
    }
    for (double mix : { 0., 0.5, 0.999 }) {
        GpuQuery q = eligibleQuery();
        q.mix = mix;
        EXPECT_EQ(supported, blur->canRenderOnGpu(q)) << "mix " << mix;
    }
    {
        GpuQuery q = eligibleQuery();
        q.unPremultChannel = 3;
        EXPECT_FALSE(blur->canRenderOnGpu(q)) << "a divisor outside the plane";
        q.unPremultInPlane = true;
        EXPECT_EQ(supported, blur->canRenderOnGpu(q)) << "unpremult";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(blur->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }

    for (const char* filter : { kBlurParamFilterQuasiGaussian, kBlurParamFilterGaussian, kBlurParamFilterBox, kBlurParamFilterTriangle, kBlurParamFilterQuadratic }) {
        ASSERT_TRUE(setKnobValue(node, kBlurParamFilter, std::string(filter)));
        EXPECT_EQ(supported, blur->canRenderOnGpu(eligibleQuery())) << filter;
        // Any size: the recursive filters have no tile to outgrow.
        ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 1000., 1000. }));
        EXPECT_EQ(supported, blur->canRenderOnGpu(eligibleQuery())) << filter << " at size 1000";
        ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 10., 10. }));
    }
    ASSERT_TRUE(setKnobValue(node, kBlurParamFilter, std::string(kBlurParamFilterFIRGaussian)));
    EXPECT_EQ(supported, blur->canRenderOnGpu(eligibleQuery())) << "FIR Gaussian";

    // Size 1000 is sigma 416.7, a radius of about 1250 at full scale and 313 at a quarter.
    ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 1000., 10. }));
    EXPECT_FALSE(blur->canRenderOnGpu(eligibleQuery())) << "radius beyond the kernel's tile";
    {
        GpuQuery q = eligibleQuery();
        q.mappedScale = RenderScale::fromMipmapLevel(2);
        EXPECT_EQ(supported, blur->canRenderOnGpu(q)) << "the same size at a quarter scale";
    }
    // Sigma 213.3 gives radius 640, past the tile.
    ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 10., 512. }));
    EXPECT_FALSE(blur->canRenderOnGpu(eligibleQuery())) << "vertical radius beyond the kernel's tile";
}

TEST_F(NativeBlurGpuTest, InputRegionIsTheSourceRegionOfInterest)
{
    resetProject();
    NodePtr node = makeBlur();
    Blur* blur = blurOf(node);
    ASSERT_TRUE(blur != NULL);
    ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 12., 30. }));

    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        BlurParams params;
        blur->getParams(kTime, ViewIdx(0), &params);
        EXPECT_EQ(Blur::getSourceRoI(rect, scale, params), blur->getGpuInputRoI(0, blur->getCurrentTime(), blur->getCurrentView(), rect, scale)) << "mipmap " << mipmapLevel;
    }
}

#ifdef NATRON_HAVE_GPU

namespace {

ImageLayerDesc
colorLayer(const NodePtr& node)
{
    std::list<ImageLayerDesc> layers;

    node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            return *it;
        }
    }

    return ImageLayerDesc();
}

bool
renderColor(const NodePtr& node,
            unsigned mipmapLevel,
            const RectI& window,
            RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(colorLayer(node));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

RectI
pixelRegionOfDefinition(const NodePtr& node,
                        unsigned mipmapLevel)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(mipmapLevel, 1.);
}

std::string
describeCase(double sigma,
             unsigned mipmapLevel,
             const RectI& roi)
{
    std::ostringstream s;
    s << "sigma " << sigma
      << " mipmap " << mipmapLevel << " roi (" << roi.x1 << "," << roi.y1 << ")-(" << roi.x2 << "," << roi.y2 << ")";

    return s.str();
}

} // namespace

// The CPU render of the node against renderGpu() fed with the source over getGpuInputRoI(), on
// the parity source moved off the format's origin so the halo, the format crop and the input's
// edges all fall inside the filtered extent.
TEST_F(NativeBlurGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    const ParityTolerance kTolerance = ParityTolerance::make(2e-6, 0.);
    int nCompared = 0;
    for (double sigma : { 0.5, 3., 25., 100. }) {
        // A fresh node per setting, so no region of definition cached under an earlier
        // setting is reused.
        NodePtr node = makeBlur();
        Blur* blur = blurOf(node);
        ASSERT_TRUE(blur != NULL);
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 2.4 * sigma, 2.4 * sigma }));

        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            if ((mipmapLevel > 0) && (sigma != 3.) && (sigma != 25.)) {
                continue;
            }
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
            ASSERT_FALSE(rod.isNull());
            // The whole region of definition, and an odd-sized window inside it.
            std::vector<RectI> rois;
            rois.push_back(rod);
            rois.push_back(RectI(rod.x1 + 5, rod.y1 + 3, std::min(rod.x2 - 2, rod.x1 + 5 + 23), std::min(rod.y2 - 1, rod.y1 + 3 + 17)));

            for (const RectI& roi : rois) {
                const std::string what = describeCase(sigma, mipmapLevel, roi);
                RenderedPlane cpu;
                ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
                ASSERT_EQ(4u, cpu.channels.size()) << what;
                ASSERT_EQ(roi, cpu.window) << what;

                const RectI inputRoI = blur->getGpuInputRoI(0, blur->getCurrentTime(), blur->getCurrentView(), roi, scale);
                RenderedPlane in;
                ASSERT_TRUE(renderColor(source, mipmapLevel, inputRoI, &in)) << what;
                ASSERT_EQ(inputRoI, in.window) << what;

                const int nComps = 4;
                // Both views sit inside larger buffers, at an offset and with padded rows.
                GpuParityImage image;
                image.bounds = inputRoI;
                image.nComps = nComps;
                image.pixels = in.pixels;
                image.layout.padFloats = 6;
                image.layout.rowPadFloats = 8;
                GpuViewLayout layout;
                layout.padFloats = 4;
                layout.rowPadFloats = 4;
                layout.marginLeft = 2;
                layout.marginBottom = 1;
                layout.marginRight = 3;
                layout.marginTop = 2;

                GpuQuery query = eligibleQuery();
                query.mappedScale = scale;
                const GpuRunResult run = runRenderGpu(*runtime, *blur, query, std::vector<GpuParityImage>(1, image), nullptr, roi, layout);
                ASSERT_TRUE(run.ok) << what;
                EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";

                ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
                const ImageDiff diff = compareImages(cpu.pixels, run.pixels, kTolerance);
                nCompared += (int)diff.compared;
                const std::size_t at = diff.worstIndex / nComps;
                EXPECT_EQ(0u, diff.mismatches) << what << ", worst at (" << (roi.x1 + (int)(at % roi.width())) << "," << (roi.y1 + (int)(at / roi.width())) << ")";
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

namespace {

// What the placement asks renderGpu() for when rendering the colour plane of node.
GpuQuery
queryFor(const NodePtr& node,
         unsigned mipmapLevel)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    const ImageLayerDesc plane = colorLayer(node);
    GpuQuery q = eligibleQuery();

    q.mappedScale = RenderScale::fromMipmapLevel(mipmapLevel);
    q.nComps = plane.getNumComponents();
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

    return q;
}

enum MaskMode {
    eMaskNone,
    eMaskOn,
    eMaskInverted
};

struct GpuWrappedBlurCase {
    const char* name;
    double sigma;
    MaskMode mask;
    double mix;
    bool redAndAlpha;
    unsigned maxMipmapLevel;
    const char* filter = kBlurParamFilterFIRGaussian;
};

} // namespace

// The settings renderGpu() wraps around the blur: the mask (partly covering the source, plain and
// inverted), mix and an R+A channel subset, alone and combined, against the CPU render.
TEST_F(NativeBlurGpuTest, MatchesTheCpuRenderWithMaskMixAndChannels)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);
    NodePtr maskSource = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(maskSource));
    setParitySourceOrigin(maskSource, 8, 4);

    const GpuWrappedBlurCase cases[] = {
        { "mask", 3., eMaskOn, 1., false, 1 },
        { "inverted mask", 3., eMaskInverted, 1., false, 0 },
        { "mix", 25., eMaskNone, 0.3, false, 1 },
        { "R+A", 3., eMaskNone, 1., true, 1 },
        { "everything", 25., eMaskOn, 0.3, true, 0 },
        { "everything", 100., eMaskInverted, 0.3, true, 0 },
        // The recursive filters depend on whole lines, which a mask must leave as they are.
        { "IIR mask", 3., eMaskOn, 1., false, 1, kBlurParamFilterGaussian },
        { "IIR inverted mask, mix", 10., eMaskInverted, 0.3, false, 0, kBlurParamFilterGaussian },
        { "IIR R+A", 10., eMaskNone, 1., true, 0, kBlurParamFilterGaussian },
    };

    const ParityTolerance kTolerance = ParityTolerance::make(2e-6, 0.);
    int nCompared = 0;
    for (const GpuWrappedBlurCase& c : cases) {
        NodePtr node = makeBlur();
        Blur* blur = blurOf(node);
        ASSERT_TRUE(blur != NULL);
        connectNodes(source, node, 0, true);
        const int maskInput = blur->getMaskInput();
        ASSERT_GE(maskInput, 0);
        connectNodes(maskSource, node, maskInput, true);
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
        ASSERT_TRUE(maskChannel != NULL);
        maskChannel->set("rgba.A");
        ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { (c.mask != eMaskNone) ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMaskInvertParamName, { (c.mask == eMaskInverted) ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { c.mix }));
        if (c.redAndAlpha) {
            KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
            ASSERT_TRUE(bool(channelSet));
            channelSet->setChannels(0, std::vector<std::string> { "R", "A" });
        }
        ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 2.4 * c.sigma, 2.4 * c.sigma }));
        ASSERT_TRUE(setKnobValue(node, kBlurParamFilter, std::string(c.filter)));
        const bool recursive = std::string(c.filter) != kBlurParamFilterFIRGaussian;

        for (unsigned mipmapLevel = 0; mipmapLevel <= c.maxMipmapLevel; ++mipmapLevel) {
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
            ASSERT_FALSE(rod.isNull());
            const GpuQuery query = queryFor(node, mipmapLevel);
            const std::string base = std::string(c.name) + " " + describeCase(c.sigma, mipmapLevel, rod);
            ASSERT_EQ(4, query.nComps) << base;
            EXPECT_EQ(c.mask != eMaskNone, query.hasMask) << base;
            EXPECT_LT(query.unPremultChannel, 0) << base;
            EXPECT_EQ(!c.redAndAlpha, query.processChannels[1] && query.processChannels[2]) << base;
            ASSERT_TRUE(blur->canRenderOnGpu(query)) << base;

            RenderedPlane maskRendered;
            ASSERT_TRUE(renderColor(maskSource, mipmapLevel, paritySourceWindow(maskSource, kTime, mipmapLevel), &maskRendered)) << base;
            GpuParityImage mask;
            mask.bounds = maskRendered.window;
            mask.nComps = (int)maskRendered.channels.size();
            mask.pixels = maskRendered.pixels;
            mask.layout.padFloats = 3;
            mask.layout.rowPadFloats = 5;

            std::vector<RectI> rois;
            rois.push_back(rod);
            rois.push_back(RectI(rod.x1 + 5, rod.y1 + 3, std::min(rod.x2 - 2, rod.x1 + 5 + 23), std::min(rod.y2 - 1, rod.y1 + 3 + 17)));
            for (const RectI& roi : rois) {
                const std::string what = std::string(c.name) + " " + describeCase(c.sigma, mipmapLevel, roi);
                RenderedPlane cpu;
                ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
                ASSERT_EQ(4u, cpu.channels.size()) << what;
                ASSERT_EQ(roi, cpu.window) << what;

                const RectI inputRoI = blur->getGpuInputRoI(0, blur->getCurrentTime(), blur->getCurrentView(), roi, scale);
                RenderedPlane in;
                ASSERT_TRUE(renderColor(source, mipmapLevel, inputRoI, &in)) << what;
                ASSERT_EQ(inputRoI, in.window) << what;

                const int nComps = 4;
                GpuParityImage image;
                image.bounds = inputRoI;
                image.nComps = nComps;
                image.pixels = in.pixels;
                image.layout.padFloats = 6;
                image.layout.rowPadFloats = 8;
                GpuViewLayout layout;
                layout.padFloats = 4;
                layout.rowPadFloats = 4;
                layout.marginLeft = 2;
                layout.marginBottom = 1;
                layout.marginRight = 3;
                layout.marginTop = 2;

                const GpuRunResult run = runRenderGpu(*runtime, *blur, query, std::vector<GpuParityImage>(1, image), query.hasMask ? &mask : nullptr, roi, layout);
                ASSERT_TRUE(run.ok) << what;
                EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";

                ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
                const ImageDiff diff = compareImages(cpu.pixels, run.pixels, recursive ? ParityTolerance::iir() : kTolerance);
                nCompared += (int)diff.compared;
                const std::size_t at = diff.worstIndex / nComps;
                EXPECT_EQ(0u, diff.mismatches) << what << ", worst " << diff.worstAbsolute << " at (" << (roi.x1 + (int)(at % roi.width())) << "," << (roi.y1 + (int)(at / roi.width())) << ") channel " << (diff.worstIndex % nComps) << " gpu " << run.pixels[diff.worstIndex] << " cpu " << cpu.pixels[diff.worstIndex];
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

namespace {

const char* const kRecursiveFilters[] = {
    kBlurParamFilterQuasiGaussian,
    kBlurParamFilterGaussian,
    kBlurParamFilterBox,
    kBlurParamFilterTriangle,
    kBlurParamFilterQuadratic,
};

// One channel of a packed image of nComps channels.
std::vector<float>
channelOf(const std::vector<float>& pixels,
          int nComps,
          int channel)
{
    std::vector<float> out(pixels.size() / nComps);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = pixels[i * nComps + channel];
    }

    return out;
}

} // namespace

// The IIR Gaussians and the box, triangle and quadratic filters against the CPU render, at sizes
// 3, 25 and 100, over the whole region of definition, and over an odd window at size 25, whose
// lines the halo cuts short. Then each on a one-channel image: the alpha of the source, against the
// alpha of the CPU's RGBA render, which filters each channel alone.
TEST_F(NativeBlurGpuTest, RecursiveAndBoxFiltersMatchTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    int nCompared = 0;
    for (const char* filter : kRecursiveFilters) {
        for (double size : { 3., 25., 100. }) {
            NodePtr node = makeBlur();
            Blur* blur = blurOf(node);
            ASSERT_TRUE(blur != NULL);
            connectNodes(source, node, 0, true);
            ASSERT_TRUE(setKnobValue(node, kBlurParamFilter, std::string(filter)));
            ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { size, size }));

            const RectI rod = pixelRegionOfDefinition(node, 0);
            ASSERT_FALSE(rod.isNull());
            std::vector<RectI> rois;
            rois.push_back(rod);
            if (size == 25.) {
                rois.push_back(RectI(rod.x1 + 41, rod.y1 + 37, std::min(rod.x2 - 2, rod.x1 + 41 + 23), std::min(rod.y2 - 1, rod.y1 + 37 + 17)));
            }
            for (const RectI& roi : rois) {
                std::ostringstream what;
                what << filter << " size " << size << " roi (" << roi.x1 << "," << roi.y1 << ")-(" << roi.x2 << "," << roi.y2 << ")";
                RenderedPlane cpu;
                ASSERT_TRUE(renderColor(node, 0, roi, &cpu)) << what.str();
                ASSERT_EQ(4u, cpu.channels.size()) << what.str();

                const RectI inputRoI = blur->getGpuInputRoI(0, blur->getCurrentTime(), blur->getCurrentView(), roi, RenderScale::identity);
                RenderedPlane in;
                ASSERT_TRUE(renderColor(source, 0, inputRoI, &in)) << what.str();

                GpuParityImage image;
                image.bounds = inputRoI;
                image.nComps = 4;
                image.pixels = in.pixels;
                image.layout.padFloats = 6;
                image.layout.rowPadFloats = 8;
                GpuViewLayout layout;
                layout.padFloats = 4;
                layout.rowPadFloats = 4;
                layout.marginLeft = 2;
                layout.marginBottom = 1;
                layout.marginRight = 3;
                layout.marginTop = 2;

                const GpuRunResult run = runRenderGpu(*runtime, *blur, eligibleQuery(), std::vector<GpuParityImage>(1, image), nullptr, roi, layout);
                ASSERT_TRUE(run.ok) << what.str();
                EXPECT_EQ(0u, run.writtenOutside) << what.str() << ": floats outside the window were written";
                ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what.str();
                const ImageDiff diff = compareImages(cpu.pixels, run.pixels, ParityTolerance::iir());
                nCompared += (int)diff.compared;
                EXPECT_EQ(0u, diff.mismatches) << what.str() << ", worst " << diff.worstAbsolute << " at float " << diff.worstIndex;

                if ((size != 25.) || (roi != rod)) {
                    continue;
                }
                GpuParityImage alpha;
                alpha.bounds = inputRoI;
                alpha.nComps = 1;
                alpha.pixels = channelOf(in.pixels, 4, 3);
                GpuQuery query = eligibleQuery();
                query.nComps = 1;
                ASSERT_TRUE(blur->canRenderOnGpu(query)) << what.str();
                const GpuRunResult runAlpha = runRenderGpu(*runtime, *blur, query, std::vector<GpuParityImage>(1, alpha), nullptr, roi);
                ASSERT_TRUE(runAlpha.ok) << what.str() << " alpha";
                const std::vector<float> cpuAlpha = channelOf(cpu.pixels, 4, 3);
                ASSERT_EQ(cpuAlpha.size(), runAlpha.pixels.size()) << what.str() << " alpha";
                const ImageDiff alphaDiff = compareImages(cpuAlpha, runAlpha.pixels, ParityTolerance::iir());
                nCompared += (int)alphaDiff.compared;
                EXPECT_EQ(0u, alphaDiff.mismatches) << what.str() << " alpha, worst " << alphaDiff.worstAbsolute << " at pixel " << alphaDiff.worstIndex;
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

namespace {

bool
moveToDevice(gpu::GpuRuntime& runtime,
             gpu::GpuBuffer& buffer,
             std::vector<float>& data)
{
    gpu::TimelineValue done;
    gpu::GpuStatus s = runtime.upload(gpu::HostRange { data.data(), data.size() * sizeof(float) }, buffer, 0, done);
    if (s) {
        s = runtime.wait(done);
    }
    EXPECT_TRUE(bool(s)) << s.message;

    return bool(s);
}

} // namespace

// The device time of a UHD (3840x2160) RGBA Blur with each recursive and box filter at sizes 10 and
// 100, its source and output resident: the wall time of submitting renderGpu()'s work alone and
// waiting for it, the fastest of three after a warm-up. Only runs with NATRON_GPU_BENCH=1, and is
// meant for a discrete device such as RADV; the times go to stdout and to the test's properties.
TEST_F(NativeBlurGpuTest, RecursiveFiltersUhdKernelTime)
{
    const char* bench = std::getenv("NATRON_GPU_BENCH");
    if (!bench || (std::string(bench) != "1")) {
        std::printf("skipped: set NATRON_GPU_BENCH=1 to time the UHD recursive blurs\n");

        return;
    }
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const int width = 3840;
    const int height = 2160;
    ProjectPtr project = getApp()->getProject();
    project->reset(false, true);
    project->setAutoSetProjectFormatEnabled(false);
    project->setOrAddProjectFormat(Format(0, 0, width, height, "nativeBlurGpuUhd", 1.));

    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr node = makeBlur();
    Blur* blur = blurOf(node);
    ASSERT_TRUE(bool(source) && (blur != NULL));
    connectNodes(source, node, 0, true);

    const RectI rect(0, 0, width, height);
    gpu::GpuRuntime& vk = runtime->runtime();
    gpu::GpuBuffer* dstBuffer = runtime->buffer(1, (std::size_t)rect.area() * 4);
    ASSERT_TRUE(dstBuffer != NULL);
    const double megapixels = (double)rect.area() / 1e6;
    for (const char* filter : kRecursiveFilters) {
        for (double size : { 10., 100. }) {
            ASSERT_TRUE(setKnobValue(node, kBlurParamFilter, std::string(filter)));
            ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { size, size }));
            ASSERT_TRUE(blur->canRenderOnGpu(eligibleQuery())) << filter;
            const RectI inputRoI = blur->getGpuInputRoI(0, blur->getCurrentTime(), blur->getCurrentView(), rect, RenderScale::identity);
            ASSERT_FALSE(inputRoI.isNull());

            // Edges in every direction, so no line is constant.
            std::vector<float> pixels((std::size_t)inputRoI.area() * 4);
            for (int y = 0; y < inputRoI.height(); ++y) {
                for (int x = 0; x < inputRoI.width(); ++x) {
                    float* p = &pixels[((std::size_t)y * inputRoI.width() + x) * 4];
                    const bool cell = (((x / 37) + (y / 23)) & 1) != 0;
                    p[0] = cell ? 0.9f : 0.1f;
                    p[1] = (float)(x % 256) / 255.f;
                    p[2] = (float)(y % 512) / 511.f;
                    p[3] = 1.f;
                }
            }
            gpu::GpuBuffer* srcBuffer = runtime->buffer(0, pixels.size());
            ASSERT_TRUE(srcBuffer != NULL);
            ASSERT_TRUE(moveToDevice(vk, *srcBuffer, pixels));

            GpuImageView input;
            input.buffer = srcBuffer;
            input.bounds = inputRoI;
            input.rowStrideFloats = inputRoI.width() * 4;
            input.nComps = 4;
            double best = 0.;
            for (int run = 0; run < 4; ++run) {
                std::unique_ptr<gpu::GpuRecordContext> context;
                const gpu::GpuStatus created = vk.newRecordContext(context);
                ASSERT_TRUE(bool(created)) << created.message;
                GpuRenderArgs args(*context);
                args.query = eligibleQuery();
                args.outputRect = rect;
                args.inputs.push_back(input);
                args.output.buffer = dstBuffer;
                args.output.bounds = rect;
                args.output.rowStrideFloats = rect.width() * 4;
                args.output.nComps = 4;
                ASSERT_EQ(eStatusOK, blur->renderGpu(args)) << filter << " size " << size;
                const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
                const gpu::GpuStatus submitted = context->submitAndWait(600'000'000'000ull);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                ASSERT_TRUE(bool(submitted)) << submitted.message;
                if ((run == 1) || ((run > 1) && (ms < best))) {
                    best = ms;
                }
            }
            std::printf("blur %s size %g, UHD: %.1f ms, %.3f ms/Mpx\n", filter, size, best, best / megapixels);
            RecordProperty(std::string(filter) + "_" + std::to_string((int)size) + "_uhd_ms", std::to_string(best));
        }
    }
}

#endif // NATRON_HAVE_GPU
