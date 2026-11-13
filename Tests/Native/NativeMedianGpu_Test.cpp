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
#include <cmath>
#include <cstdint>
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
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Median.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

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

class NativeMedianGpuTest
    : public BaseTest {
protected:
    // A 64x48 project format, the parity source's size.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeMedianGpu64x48", 1.));
    }

    NodePtr makeNode()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_MEDIAN), PLUGIN_MAJOR_NATRON_MEDIAN);
        EXPECT_TRUE(bool(node));

        return node;
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }
};

TEST_F(NativeMedianGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr node = makeNode();
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    for (int size : { 1, 2, 3, 9, Median::kMedianGpuMaxRadius }) {
        ASSERT_TRUE(setKnobValues(node, kMedianParamSize, { (double)size }));
        for (int nComps : { 1, 2, 3, 4 }) {
            GpuQuery q = eligibleQuery();
            q.nComps = nComps;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "size " << size << ", " << nComps << " components";
        }
        {
            GpuQuery q = eligibleQuery();
            q.hasMask = true;
            q.maskInPlane = true;
            q.mix = 0.5;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "size " << size << " mask and mix";
        }
        for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
            GpuQuery q = eligibleQuery();
            q.bitDepth = depth;
            EXPECT_FALSE(effect->canRenderOnGpu(q)) << "size " << size << " bit depth " << (int)depth;
        }
    }
    // Beyond the tiled kernel's largest window the CPU renders, but at half scale the radius fits.
    ASSERT_TRUE(setKnobValues(node, kMedianParamSize, { (double)(Median::kMedianGpuMaxRadius + 1) }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery()));
    GpuQuery half = eligibleQuery();
    half.mappedScale = RenderScale::fromMipmapLevel(1);
    EXPECT_EQ(supported, effect->canRenderOnGpu(half));
}

TEST_F(NativeMedianGpuTest, InputRegionIsTheSourceRegionOfInterest)
{
    resetProject();
    NodePtr node = makeNode();
    Median* effect = node ? dynamic_cast<Median*>(node->getEffectInstance().get()) : NULL;
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setKnobValues(node, kMedianParamSize, { 7. }));

    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        MedianParams params;
        effect->getParams(kTime, ViewIdx(0), &params);
        const RectI roi = Median::getSourceRoI(rect, scale, params);
        const int r = Median::pixelRadius(scale, params);
        EXPECT_EQ(RectI(rect.x1 - r, rect.y1 - r, rect.x2 + r, rect.y2 + r), roi) << "mipmap " << mipmapLevel;
        EXPECT_EQ(roi, effect->getGpuInputRoI(0, effect->getCurrentTime(), effect->getCurrentView(), rect, scale)) << "mipmap " << mipmapLevel;
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

GpuViewLayout
outputLayout()
{
    GpuViewLayout layout;

    layout.padFloats = 4;
    layout.rowPadFloats = 4;
    layout.marginLeft = 2;
    layout.marginBottom = 1;
    layout.marginRight = 3;
    layout.marginTop = 2;

    return layout;
}

// The source over getGpuInputRoI(roi), inside a larger buffer at an offset and with padded rows.
bool
sourceImage(const NodePtr& source,
            StagedImageEffect& effect,
            unsigned mipmapLevel,
            const RectI& roi,
            GpuParityImage* image)
{
    const RectI inputRoI = effect.getGpuInputRoI(0, effect.getCurrentTime(), effect.getCurrentView(), roi, RenderScale::fromMipmapLevel(mipmapLevel));
    RenderedPlane in;
    if (!renderColor(source, mipmapLevel, inputRoI, &in)) {
        return false;
    }
    EXPECT_EQ(inputRoI, in.window);
    image->bounds = inputRoI;
    image->nComps = 4;
    image->pixels = in.pixels;
    image->layout.padFloats = 6;
    image->layout.rowPadFloats = 8;

    return true;
}

std::string
describeRoI(const RectI& roi)
{
    std::ostringstream s;
    s << "roi (" << roi.x1 << "," << roi.y1 << ")-(" << roi.x2 << "," << roi.y2 << ")";

    return s.str();
}

// The whole region of definition, an odd-sized window inside it, and a window two pixels wide at
// its right edge.
std::vector<RectI>
testWindows(const RectI& rod)
{
    std::vector<RectI> rois;

    rois.push_back(rod);
    rois.push_back(RectI(rod.x1 + 5, rod.y1 + 3, std::min(rod.x2 - 2, rod.x1 + 5 + 23), std::min(rod.y2 - 1, rod.y1 + 3 + 17)));
    rois.push_back(RectI(std::max(rod.x1, rod.x2 - 2), rod.y1 + 1, rod.x2, std::min(rod.y2, rod.y1 + 30)));

    return rois;
}

std::string
describeCase(int size,
             double threshold,
             unsigned mipmapLevel)
{
    std::ostringstream s;
    s << "size " << size << " threshold " << threshold << " mipmap " << mipmapLevel;

    return s.str();
}

void
expectSame(const RenderedPlane& cpu,
           const GpuRunResult& run,
           const RectI& roi,
           const std::string& what,
           int* nCompared,
           const ParityTolerance& tolerance = ParityTolerance::exact())
{
    ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
    const ImageDiff diff = compareImages(cpu.pixels, run.pixels, tolerance);
    *nCompared += (int)diff.compared;
    const std::size_t at = diff.firstMismatchIndex / 4;
    EXPECT_EQ(0u, diff.mismatches) << what << ", first at (" << (roi.x1 + (int)(at % roi.width())) << "," << (roi.y1 + (int)(at / roi.width())) << ") channel " << (diff.firstMismatchIndex % 4)
                                   << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
}

} // namespace

// The CPU render of the node against renderGpu() fed with the source over getGpuInputRoI(), on the
// parity source moved off the format's origin so the halo, the format and the input's edges all
// fall inside the filtered extent. Both kernels (radius up to 2, and the tiled one up to its
// largest window) select an input value, so the match is exact.
TEST_F(NativeMedianGpuTest, MatchesTheCpuRender)
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
    for (int size : { 1, 2, 3, 6, Median::kMedianGpuMaxRadius }) {
        for (double threshold : { 0., 0.2 }) {
            // A fresh node per setting, so no region of definition cached under an earlier
            // setting is reused.
            NodePtr node = makeNode();
            StagedImageEffect* effect = effectOf(node);
            ASSERT_TRUE(effect != NULL);
            connectNodes(source, node, 0, true);
            ASSERT_TRUE(setKnobValues(node, kMedianParamSize, { (double)size }));
            ASSERT_TRUE(setKnobValues(node, kMedianParamThreshold, { threshold }));

            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
                ASSERT_FALSE(rod.isNull());
                const GpuQuery query = queryFor(node, mipmapLevel);
                ASSERT_TRUE(effect->canRenderOnGpu(query)) << describeCase(size, threshold, mipmapLevel);

                for (const RectI& roi : testWindows(rod)) {
                    const std::string what = describeCase(size, threshold, mipmapLevel) + " " + describeRoI(roi);
                    RenderedPlane cpu;
                    ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
                    ASSERT_EQ(4u, cpu.channels.size()) << what;
                    ASSERT_EQ(roi, cpu.window) << what;

                    GpuParityImage image;
                    ASSERT_TRUE(sourceImage(source, *effect, mipmapLevel, roi, &image)) << what;
                    const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, roi, outputLayout());
                    ASSERT_TRUE(run.ok) << what;
                    EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";
                    ASSERT_NO_FATAL_FAILURE(expectSame(cpu, run, roi, what, &nCompared));
                }
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

namespace {

struct WrappedCase {
    const char* name;
    int size;
    bool maskOn;
    bool maskInverted;
    double mix;
    bool allChannels;
};

} // namespace

// The settings renderGpu() wraps around the filter: the mask (plain and inverted), mix and alpha
// processed too, against the CPU render. The selection is exact, but the mask and mix lerp rounds
// differently on the device, by an ulp, so this one compares within the arithmetic tolerance.
TEST_F(NativeMedianGpuTest, MatchesTheCpuRenderWithMaskMixAndChannels)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    const WrappedCase cases[] = {
        { "mask", 1, true, false, 1., false },
        { "inverted mask", 5, true, true, 1., false },
        { "mix", 2, false, false, 0.5, false },
        { "RGBA", 4, false, false, 1., true },
        { "everything", 3, true, false, 0.5, true },
    };

    int nCompared = 0;
    for (const WrappedCase& c : cases) {
        NodePtr maskSource = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        ASSERT_TRUE(bool(maskSource));
        setParitySourceOrigin(maskSource, 8, 4);
        NodePtr node = makeNode();
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        connectNodes(source, node, 0, true);
        const int maskInput = effect->getMaskInput();
        ASSERT_GE(maskInput, 0);
        connectNodes(maskSource, node, maskInput, true);
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
        ASSERT_TRUE(maskChannel != NULL);
        maskChannel->set("rgba.A");
        ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { c.maskOn ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMaskInvertParamName, { c.maskInverted ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { c.mix }));
        if (c.allChannels) {
            KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
            ASSERT_TRUE(bool(channelSet));
            channelSet->setChannels(0, std::vector<std::string> { "R", "G", "B", "A" });
        }
        ASSERT_TRUE(setKnobValues(node, kMedianParamSize, { (double)c.size }));
        ASSERT_TRUE(setKnobValues(node, kMedianParamThreshold, { 0.1 }));

        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
            ASSERT_FALSE(rod.isNull());
            const GpuQuery query = queryFor(node, mipmapLevel);
            const std::string base = std::string(c.name) + " " + describeCase(c.size, 0.1, mipmapLevel);
            ASSERT_EQ(4, query.nComps) << base;
            EXPECT_EQ(c.maskOn, query.hasMask) << base;
            EXPECT_EQ(c.allChannels, (bool)query.processChannels[3]) << base;
            ASSERT_TRUE(effect->canRenderOnGpu(query)) << base;

            RenderedPlane maskRendered;
            ASSERT_TRUE(renderColor(maskSource, mipmapLevel, paritySourceWindow(maskSource, kTime, mipmapLevel), &maskRendered)) << base;
            GpuParityImage mask;
            mask.bounds = maskRendered.window;
            mask.nComps = (int)maskRendered.channels.size();
            mask.pixels = maskRendered.pixels;
            mask.layout.padFloats = 3;
            mask.layout.rowPadFloats = 5;

            const RectI sourceRoD = pixelRegionOfDefinition(source, mipmapLevel);
            for (const RectI& roi : testWindows(rod)) {
                if (!roi.intersects(sourceRoD)) {
                    continue;
                }
                const std::string what = base + " " + describeRoI(roi);
                RenderedPlane cpu;
                ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
                ASSERT_EQ(4u, cpu.channels.size()) << what;
                ASSERT_EQ(roi, cpu.window) << what;

                GpuParityImage image;
                ASSERT_TRUE(sourceImage(source, *effect, mipmapLevel, roi, &image)) << what;
                const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), query.hasMask ? &mask : nullptr, roi, outputLayout());
                ASSERT_TRUE(run.ok) << what;
                EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";
                ASSERT_NO_FATAL_FAILURE(expectSame(cpu, run, roi, what, &nCompared, ParityTolerance::arithmetic()));
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

namespace {

uint32_t
bitsOf(float f)
{
    uint32_t u;

    std::memcpy(&u, &f, sizeof(u));

    return u;
}

float
fromBits(uint32_t u)
{
    float f;

    std::memcpy(&f, &u, sizeof(f));

    return f;
}

// Values the GPU must order and compare as the CPU does: NaNs (two payloads), both zeros,
// denormals of both signs, the infinities, and ordinary values in between.
float
specialValue(int x,
             int y,
             int c)
{
    static const uint32_t kBits[] = {
        0x7fc00000u,
        0xffc00001u,
        0x80000000u,
        0x00000000u,
        0x00000001u,
        0x80000001u,
        0x007fffffu,
        0x807fffffu,
        0x7f800000u,
        0xff800000u,
        0x3f000000u,
        0xbf000000u,
        0x3f800000u,
        0x40000000u,
    };
    const int n = (int)(sizeof(kBits) / sizeof(kBits[0]));
    const int k = ((x * 7 + y * 13 + c * 5) % 23 + 23) % 23;
    if (k < n) {
        return fromBits(kBits[k]);
    }

    return 0.01f * (float)((x * 3 + y * 5 + c) % 17) - 0.08f;
}

} // namespace

// renderGpu() on NaNs, signed zeros, denormals and infinities against the median computed here as
// the CPU defines it, bit for bit: the buffer over the source region of interest clipped to the
// region of definition, zero outside the source, windows clamped to the buffer, samples through
// Median::windowSample() and the result through Median::applyThreshold(). All four channels are
// processed so that alpha is filtered too.
TEST_F(NativeMedianGpuTest, SpecialValuesMatchBitForBit)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);
    const RectI sourceWindow = paritySourceWindow(source, kTime, 0);

    for (int size : { 1, 2, 3, 7 }) {
        for (double threshold : { 0., 0.3 }) {
            NodePtr node = makeNode();
            StagedImageEffect* effect = effectOf(node);
            ASSERT_TRUE(effect != NULL);
            connectNodes(source, node, 0, true);
            KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
            ASSERT_TRUE(bool(channelSet));
            channelSet->setChannels(0, std::vector<std::string> { "R", "G", "B", "A" });
            ASSERT_TRUE(setKnobValues(node, kMedianParamSize, { (double)size }));
            ASSERT_TRUE(setKnobValues(node, kMedianParamThreshold, { threshold }));
            const RectI rod = pixelRegionOfDefinition(node, 0);
            GpuQuery query = eligibleQuery();
            ASSERT_TRUE(effect->canRenderOnGpu(query));

            for (const RectI& roi : testWindows(rod)) {
                const std::string what = describeCase(size, threshold, 0) + " " + describeRoI(roi);
                const RectI inputRoI = effect->getGpuInputRoI(0, effect->getCurrentTime(), effect->getCurrentView(), roi, RenderScale::identity);
                GpuParityImage image;
                image.bounds = inputRoI;
                image.nComps = 4;
                image.pixels.resize((std::size_t)inputRoI.area() * 4);
                for (int y = inputRoI.y1; y < inputRoI.y2; ++y) {
                    for (int x = inputRoI.x1; x < inputRoI.x2; ++x) {
                        for (int c = 0; c < 4; ++c) {
                            image.pixels[((std::size_t)(y - inputRoI.y1) * inputRoI.width() + (x - inputRoI.x1)) * 4 + c] = specialValue(x, y, c);
                        }
                    }
                }
                image.layout.padFloats = 2;
                image.layout.rowPadFloats = 4;

                const RectI buffer = inputRoI.intersect(rod);
                ASSERT_FALSE(buffer.isNull()) << what;
                const auto bufferValue = [&](int x, int y, int c) -> float {
                    const bool inside = (x >= sourceWindow.x1) && (x < sourceWindow.x2) && (y >= sourceWindow.y1) && (y < sourceWindow.y2);

                    return inside ? specialValue(x, y, c) : 0.f;
                };

                const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, roi, outputLayout());
                ASSERT_TRUE(run.ok) << what;
                ASSERT_EQ((std::size_t)roi.area() * 4, run.pixels.size()) << what;
                std::size_t mismatches = 0;
                std::vector<float> window;
                for (int y = roi.y1; y < roi.y2; ++y) {
                    for (int x = roi.x1; x < roi.x2; ++x) {
                        const bool inBuffer = (x >= buffer.x1) && (x < buffer.x2) && (y >= buffer.y1) && (y < buffer.y2);
                        for (int c = 0; c < 4; ++c) {
                            float want = 0.f;
                            if (inBuffer) {
                                window.clear();
                                for (int dy = -size; dy <= size; ++dy) {
                                    for (int dx = -size; dx <= size; ++dx) {
                                        const int sx = std::min(std::max(x + dx, buffer.x1), buffer.x2 - 1);
                                        const int sy = std::min(std::max(y + dy, buffer.y1), buffer.y2 - 1);
                                        window.push_back(Median::windowSample(bufferValue(sx, sy, c)));
                                    }
                                }
                                std::sort(window.begin(), window.end());
                                want = Median::applyThreshold(bufferValue(x, y, c), window[window.size() / 2], (float)threshold);
                            }
                            const float got = run.pixels[((std::size_t)(y - roi.y1) * roi.width() + (x - roi.x1)) * 4 + c];
                            if (bitsOf(want) != bitsOf(got)) {
                                if (mismatches == 0) {
                                    ADD_FAILURE() << what << ": first at (" << x << "," << y << ") channel " << c << std::hex << " gpu 0x" << bitsOf(got) << " cpu 0x" << bitsOf(want);
                                }
                                ++mismatches;
                            }
                        }
                    }
                }
                EXPECT_EQ(0u, mismatches) << what;
            }
        }
    }
}

#endif // NATRON_HAVE_GPU

// A Grade, the median and a Grade render as one GPU region, and match the CPU render.
TEST_F(NativeMedianGpuTest, GradeMedianGradeChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr filter = makeNode();
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(first) && bool(filter) && bool(second));
    connectNodes(source, first, 0, true);
    connectNodes(first, filter, 0, true);
    connectNodes(filter, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValues(filter, kMedianParamSize, { 4. }));
    ASSERT_TRUE(setKnobValues(filter, kMedianParamThreshold, { 0.1 }));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, filter, second }), second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    bool together = true;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
        const bool hasFilter = std::find(nodes.begin(), nodes.end(), filter) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
        if (hasFirst || hasFilter || hasSecond) {
            ++holding;
            together = together && hasFirst && hasFilter && hasSecond;
        }
    }
    EXPECT_EQ(1, holding);
    EXPECT_TRUE(together);
    EXPECT_TRUE(result.ranInRegion(filter));

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}
