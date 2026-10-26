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
#include <limits>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/GpuRegionStrips.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderScheduler.h"
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

class NativeErodeDilateGpuTest
    : public BaseTest {
protected:
    // A 64x48 project format, the parity source's size.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeErodeDilateGpu64x48", 1.));
    }

    NodePtr makeNode(bool dilate)
    {
        NodePtr node = dilate ? createNode(QString::fromUtf8(PLUGINID_NATRON_DILATE), PLUGIN_MAJOR_NATRON_DILATE)
                              : createNode(QString::fromUtf8(PLUGINID_NATRON_ERODE), PLUGIN_MAJOR_NATRON_ERODE);
        EXPECT_TRUE(bool(node));

        return node;
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }
};

TEST_F(NativeErodeDilateGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    for (bool dilate : { false, true }) {
        NodePtr node = makeNode(dilate);
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        ASSERT_TRUE(setKnobValues(node, kErodeDilateParamSize, { 6., -40. }));
        const std::string name = dilate ? "dilate" : "erode";

        for (int nComps : { 1, 2, 3, 4 }) {
            GpuQuery q = eligibleQuery();
            q.nComps = nComps;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << name << " " << nComps << " components";
        }
        {
            GpuQuery q = eligibleQuery();
            q.processChannels[1] = false;
            q.processChannels[2] = false;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << name << " a channel subset";
        }
        {
            GpuQuery q = eligibleQuery();
            q.hasMask = true;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << name << " a mask channel outside the plane, read in its own layer";
            q.maskInPlane = true;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << name << " mask";
        }
        for (double mix : { 0., 0.5 }) {
            GpuQuery q = eligibleQuery();
            q.mix = mix;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << name << " mix " << mix;
        }
        for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
            GpuQuery q = eligibleQuery();
            q.bitDepth = depth;
            EXPECT_FALSE(effect->canRenderOnGpu(q)) << name << " bit depth " << (int)depth;
        }
        // The knob's range, 1000 pixels, fits the kernel's tile.
        ASSERT_TRUE(setKnobValues(node, kErodeDilateParamSize, { -1000., 1000. }));
        EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << name << " the widest size";
    }
}

TEST_F(NativeErodeDilateGpuTest, InputRegionIsTheSourceRegionOfInterest)
{
    resetProject();
    NodePtr node = makeNode(false);
    ErodeDilate* effect = node ? dynamic_cast<ErodeDilate*>(node->getEffectInstance().get()) : NULL;
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setKnobValues(node, kErodeDilateParamSize, { 12., -30. }));

    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        ErodeDilateParams params;
        effect->getParams(kTime, ViewIdx(0), &params);
        EXPECT_EQ(ErodeDilate::getSourceRoI(rect, scale, params), effect->getGpuInputRoI(0, effect->getCurrentTime(), effect->getCurrentView(), rect, scale)) << "mipmap " << mipmapLevel;
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

// The whole region of definition, an odd-sized window inside it, and a window two pixels wide
// at its right edge.
std::vector<RectI>
testWindows(const RectI& rod)
{
    std::vector<RectI> rois;

    rois.push_back(rod);
    rois.push_back(RectI(rod.x1 + 5, rod.y1 + 3, std::min(rod.x2 - 2, rod.x1 + 5 + 23), std::min(rod.y2 - 1, rod.y1 + 3 + 17)));
    rois.push_back(RectI(std::max(rod.x1, rod.x2 - 2), rod.y1 + 1, rod.x2, std::min(rod.y2, rod.y1 + 30)));

    return rois;
}

struct SizeCase {
    double x;
    double y;
};

// Sizes 1, 2, 6 and 40 alone, of both signs on one axis or across the two, which also orders a
// vertical pass before a horizontal one; 46 and 63 make lines no longer than halfWidth + 2 on the
// 64x48 source, and at a half scale size -1 is no pass at all without being identity.
const SizeCase kSizes[] = {
    { 1., 1. },
    { 2., 2. },
    { 6., 6. },
    { 40., 40. },
    { -1., -1. },
    { -2., 6. },
    { 6., -40. },
    { -40., -2. },
    { 0., 6. },
    { -6., 0. },
    { 46., 46. },
    { 63., -1. },
};

std::string
describeSize(bool dilate,
             const SizeCase& size,
             bool expandRoD,
             unsigned mipmapLevel)
{
    std::ostringstream s;
    s << (dilate ? "dilate" : "erode") << " size (" << size.x << "," << size.y << ")" << (expandRoD ? " expandRoD" : "")
      << " mipmap " << mipmapLevel;

    return s.str();
}

} // namespace

// The CPU render of the node against renderGpu() fed with the source over getGpuInputRoI(), on the
// parity source moved off the format's origin so the halo, the format and the input's edges all
// fall inside the filtered extent. Min and max pick an input value, so the match is exact.
TEST_F(NativeErodeDilateGpuTest, MatchesTheCpuRender)
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
    for (bool dilate : { false, true }) {
        for (const SizeCase& size : kSizes) {
            for (bool expandRoD : { false, true }) {
                // A fresh node per setting, so no region of definition cached under an earlier
                // setting is reused.
                NodePtr node = makeNode(dilate);
                StagedImageEffect* effect = effectOf(node);
                ASSERT_TRUE(effect != NULL);
                connectNodes(source, node, 0, true);
                ASSERT_TRUE(setKnobValues(node, kErodeDilateParamSize, { size.x, size.y }));
                ASSERT_TRUE(setKnobValues(node, kErodeDilateParamExpandRoD, { expandRoD ? 1. : 0. }));

                for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                    const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
                    ASSERT_FALSE(rod.isNull());
                    GpuQuery query = eligibleQuery();
                    query.mappedScale = RenderScale::fromMipmapLevel(mipmapLevel);
                    ASSERT_TRUE(effect->canRenderOnGpu(query));

                    for (const RectI& roi : testWindows(rod)) {
                        const std::string what = describeSize(dilate, size, expandRoD, mipmapLevel) + " " + describeRoI(roi);
                        RenderedPlane cpu;
                        ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
                        ASSERT_EQ(4u, cpu.channels.size()) << what;
                        ASSERT_EQ(roi, cpu.window) << what;

                        GpuParityImage image;
                        ASSERT_TRUE(sourceImage(source, *effect, mipmapLevel, roi, &image)) << what;
                        const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, roi, outputLayout());
                        ASSERT_TRUE(run.ok) << what;
                        EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";

                        ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
                        const ImageDiff diff = compareImages(cpu.pixels, run.pixels, ParityTolerance::exact());
                        nCompared += (int)diff.compared;
                        const std::size_t at = diff.firstMismatchIndex / 4;
                        EXPECT_EQ(0u, diff.mismatches) << what << ", first at (" << (roi.x1 + (int)(at % roi.width())) << "," << (roi.y1 + (int)(at / roi.width())) << ") channel " << (diff.firstMismatchIndex % 4)
                                                       << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
                    }
                }
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

namespace {

enum MaskMode {
    eMaskNone,
    eMaskOn,
    eMaskInverted
};

struct WrappedCase {
    const char* name;
    bool dilate;
    SizeCase size;
    MaskMode mask;
    // Where the mask's parity source sits.
    int maskX;
    int maskY;
    double mix;
    bool redAndAlpha;
};

} // namespace

// The settings renderGpu() wraps around the filter: the mask (plain and inverted, and one two
// columns wide at the source's right edge, mostly zero over the window), mix and an R+A channel
// subset, alone and combined, against the CPU render.
TEST_F(NativeErodeDilateGpuTest, MatchesTheCpuRenderWithMaskMixAndChannels)
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
        { "mask", false, { 6., 6. }, eMaskOn, 8, 4, 1., false },
        { "mask", true, { 1., -40. }, eMaskOn, 8, 6, 1., false },
        { "inverted mask", false, { 2., -6. }, eMaskInverted, 8, 4, 1., false },
        { "inverted mask", true, { 40., 2. }, eMaskInverted, 0, 2, 1., false },
        { "narrow mask", false, { 40., 1. }, eMaskOn, 65, 4, 1., false },
        { "narrow mask", true, { -40., 2. }, eMaskOn, 65, 4, 1., false },
        { "mix", false, { 40., 6. }, eMaskNone, 0, 0, 0.5, false },
        { "mix 0", true, { 6., 6. }, eMaskNone, 0, 0, 0., false },
        { "R+A", false, { 2., -2. }, eMaskNone, 0, 0, 1., true },
        { "everything", false, { 6., -2. }, eMaskOn, 8, 4, 0.5, true },
        { "everything", true, { -1., 40. }, eMaskInverted, 8, 4, 0.5, true },
    };

    int nCompared = 0;
    for (const WrappedCase& c : cases) {
        NodePtr maskSource = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        ASSERT_TRUE(bool(maskSource));
        setParitySourceOrigin(maskSource, c.maskX, c.maskY);
        NodePtr node = makeNode(c.dilate);
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        connectNodes(source, node, 0, true);
        const int maskInput = effect->getMaskInput();
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
        ASSERT_TRUE(setKnobValues(node, kErodeDilateParamSize, { c.size.x, c.size.y }));

        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
            ASSERT_FALSE(rod.isNull());
            const GpuQuery query = queryFor(node, mipmapLevel);
            const std::string base = std::string(c.name) + " " + describeSize(c.dilate, c.size, true, mipmapLevel);
            ASSERT_EQ(4, query.nComps) << base;
            EXPECT_EQ(c.mask != eMaskNone, query.hasMask) << base;
            EXPECT_LT(query.unPremultChannel, 0) << base;
            EXPECT_EQ(!c.redAndAlpha, query.processChannels[1] && query.processChannels[2]) << base;
            ASSERT_TRUE(effect->canRenderOnGpu(query)) << base;

            RenderedPlane maskRendered;
            ASSERT_TRUE(renderColor(maskSource, mipmapLevel, paritySourceWindow(maskSource, kTime, mipmapLevel), &maskRendered)) << base;
            GpuParityImage mask;
            mask.bounds = maskRendered.window;
            mask.nComps = (int)maskRendered.channels.size();
            mask.pixels = maskRendered.pixels;
            mask.layout.padFloats = 3;
            mask.layout.rowPadFloats = 5;
            // The same mask as an alpha-only view, the layout a caller may hand an rgba.A mask over in.
            ASSERT_EQ(4, mask.nComps) << base;
            GpuParityImage alphaMask;
            alphaMask.bounds = mask.bounds;
            alphaMask.nComps = 1;
            for (std::size_t i = 3; i < mask.pixels.size(); i += 4) {
                alphaMask.pixels.push_back(mask.pixels[i]);
            }

            const RectI sourceRoD = pixelRegionOfDefinition(source, mipmapLevel);
            for (const RectI& roi : testWindows(rod)) {
                const std::string what = base + " " + describeRoI(roi);
                // At mix 0 the node is an identity, which renders nothing outside its input's region
                // of definition, and the region engine never runs it.
                if ((c.mix == 0.) && !roi.intersects(sourceRoD)) {
                    continue;
                }
                RenderedPlane cpu;
                ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
                ASSERT_EQ(4u, cpu.channels.size()) << what;
                ASSERT_EQ(roi, cpu.window) << what;

                GpuParityImage image;
                ASSERT_TRUE(sourceImage(source, *effect, mipmapLevel, roi, &image)) << what;
                const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), query.hasMask ? &mask : nullptr, roi, outputLayout());
                ASSERT_TRUE(run.ok) << what;
                EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";

                ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
                const ImageDiff diff = compareImages(cpu.pixels, run.pixels, ParityTolerance::exact());
                nCompared += (int)diff.compared;
                const std::size_t at = diff.firstMismatchIndex / 4;
                EXPECT_EQ(0u, diff.mismatches) << what << ", first at (" << (roi.x1 + (int)(at % roi.width())) << "," << (roi.y1 + (int)(at / roi.width())) << ") channel " << (diff.firstMismatchIndex % 4)
                                               << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];

                if (query.hasMask) {
                    const GpuRunResult alphaRun = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), &alphaMask, roi, outputLayout());
                    ASSERT_TRUE(alphaRun.ok) << what << " alpha-only mask";
                    EXPECT_TRUE(alphaRun.pixels == run.pixels) << what << ": an alpha-only mask view renders differently";
                }
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

// Values whose order the GPU must take as the CPU's comparisons do: NaNs (two payloads), both
// zeros, denormals of both signs, the infinities, and ordinary values in between.
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

// renderGpu() on NaNs, signed zeros, denormals and infinities against ErodeDilateKernels::filterLine
// run as render() runs it, compared bit for bit: render()'s buffer over the source region of
// interest clipped to the region of definition, zero outside the source, the passes in render()'s
// order, then the window read back unchanged.
TEST_F(NativeErodeDilateGpuTest, SpecialValuesMatchFilterLineBitForBit)
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

    const SizeCase sizes[] = { { 1., 1. }, { 2., -2. }, { -6., 6. }, { 40., -40. }, { 6., 63. } };
    for (bool dilate : { false, true }) {
        for (const SizeCase& size : sizes) {
            NodePtr node = makeNode(dilate);
            StagedImageEffect* effect = effectOf(node);
            ASSERT_TRUE(effect != NULL);
            connectNodes(source, node, 0, true);
            ASSERT_TRUE(setKnobValues(node, kErodeDilateParamSize, { size.x, size.y }));
            const RectI rod = pixelRegionOfDefinition(node, 0);

            for (const RectI& roi : testWindows(rod)) {
                const std::string what = describeSize(dilate, size, true, 0) + " " + describeRoI(roi);
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
                const int bw = buffer.width();
                const int bh = buffer.height();
                std::vector<float> planes((std::size_t)bw * bh * 4, 0.f);
                for (int c = 0; c < 4; ++c) {
                    for (int y = buffer.y1; y < buffer.y2; ++y) {
                        for (int x = buffer.x1; x < buffer.x2; ++x) {
                            const bool inside = (x >= sourceWindow.x1) && (x < sourceWindow.x2) && (y >= sourceWindow.y1) && (y < sourceWindow.y2);
                            planes[(std::size_t)c * bw * bh + (std::size_t)(y - buffer.y1) * bw + (x - buffer.x1)] = inside ? specialValue(x, y, c) : 0.f;
                        }
                    }
                }
                const int halves[2][2] = {
                    { std::max(0, (int)size.x), std::max(0, (int)size.y) },
                    { std::max(0, -(int)size.x), std::max(0, -(int)size.y) },
                };
                ErodeDilateKernels::LineScratch scratch;
                for (int ph = 0; ph < 2; ++ph) {
                    const bool takeMax = (ph == 0) ? dilate : !dilate;
                    for (int c = 0; c < 4; ++c) {
                        float* plane = &planes[(std::size_t)c * bw * bh];
                        if (halves[ph][0] > 0) {
                            for (int row = 0; row < bh; ++row) {
                                ErodeDilateKernels::filterLine(plane + (std::size_t)row * bw, bw, 1, halves[ph][0], takeMax, scratch);
                            }
                        }
                        if (halves[ph][1] > 0) {
                            for (int column = 0; column < bw; ++column) {
                                ErodeDilateKernels::filterLine(plane + column, bh, bw, halves[ph][1], takeMax, scratch);
                            }
                        }
                    }
                }

                const GpuRunResult run = runRenderGpu(*runtime, *effect, eligibleQuery(), std::vector<GpuParityImage>(1, image), nullptr, roi, outputLayout());
                ASSERT_TRUE(run.ok) << what;
                ASSERT_EQ((std::size_t)roi.area() * 4, run.pixels.size()) << what;
                std::size_t mismatches = 0;
                for (int y = roi.y1; y < roi.y2; ++y) {
                    for (int x = roi.x1; x < roi.x2; ++x) {
                        for (int c = 0; c < 4; ++c) {
                            const float want = planes[(std::size_t)c * bw * bh + (std::size_t)(y - buffer.y1) * bw + (x - buffer.x1)];
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

namespace {

struct StripChain {
    NodePtr source;
    NodePtr first;
    NodePtr filter;
    NodePtr second;
};

class StripOverride {
public:
    explicit StripOverride(std::uint64_t maxBufferBytes)
    {
        setGpuStripOverridesForTests(maxBufferBytes, 0);
    }

    ~StripOverride()
    {
        setGpuStripOverridesForTests(0, 0);
    }
};

// The pool allocates every buffer at a size class of at least 64 kB, so a cap below that makes even
// the thinnest strip too large and the region falls back to the CPU. Up to 131072 the classes are
// multiples of 16 kB, so a buffer fits exactly when its raw size does.
const std::uint64_t kStripBufferBytes = 131072;

// Grows the 64x48 source's region of definition by 100 pixels on every side, to 264x248, with a halo
// of 100 rows: the output is 1047552 bytes, eight times the cap.
const double kStripFilterSize = 100.;

RectI
pixelRoD(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod));

    return rod.toPixelEnclosing(0u, 1.);
}

// The "strips" of every region record in the profile at path.
std::vector<int>
profiledRegionStrips(const std::string& path)
{
    std::vector<int> strips;
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return strips;
    }
    const std::string key = "{\"kind\":\"region\",";
    const std::string field = "\"strips\":";
    for (const QByteArray& bytes : file.readAll().split('\n')) {
        const std::string line = bytes.toStdString();
        const std::size_t at = line.find(field);
        if ((line.compare(0, key.size(), key) == 0) && (at != std::string::npos)) {
            strips.push_back(std::atoi(line.c_str() + at + field.size()));
        }
    }

    return strips;
}

int
regionsHolding(const EndToEndResult& result,
               const StripChain& chain,
               bool* together)
{
    int holding = 0;
    *together = true;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), chain.first) != nodes.end();
        const bool hasFilter = std::find(nodes.begin(), nodes.end(), chain.filter) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), chain.second) != nodes.end();
        if (hasFirst || hasFilter || hasSecond) {
            ++holding;
            *together = *together && hasFirst && hasFilter && hasSecond;
        }
    }

    return holding;
}

} // namespace

TEST_F(NativeErodeDilateGpuTest, GradeErodeGradeChainRunsAsOneRegion)
{
    for (bool dilate : { false, true }) {
        StripChain chain;
        chain.source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        chain.first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        chain.filter = makeNode(dilate);
        chain.second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        ASSERT_TRUE(bool(chain.source) && bool(chain.first) && bool(chain.filter) && bool(chain.second));
        connectNodes(chain.source, chain.first, 0, true);
        connectNodes(chain.first, chain.filter, 0, true);
        connectNodes(chain.filter, chain.second, 0, true);
        ASSERT_TRUE(setKnobValues(chain.first, "multiply", std::vector<double>(4, 1.5)));
        ASSERT_TRUE(setKnobValues(chain.filter, kErodeDilateParamSize, { 6., -2. }));
        ASSERT_TRUE(setKnobValues(chain.second, "offset", std::vector<double>(4, 0.25)));

        const RectI window = paritySourceWindow(chain.source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ chain.first, chain.filter, chain.second }), chain.second, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;

        bool together = false;
        EXPECT_EQ(1, regionsHolding(result, chain, &together)) << (dilate ? "dilate" : "erode");
        EXPECT_TRUE(together) << (dilate ? "dilate" : "erode");
        EXPECT_TRUE(result.ranInRegion(chain.filter)) << (dilate ? "dilate" : "erode");

        ASSERT_EQ(result.cpu.size(), result.gpu.size());
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
        EXPECT_EQ(0u, diff.mismatches) << (dilate ? "dilate" : "erode") << ": first at float " << diff.firstMismatchIndex;
    }
}

// With a storage buffer range below the output's size the region runs on the GPU as strips, each
// filtering its own rows plus the halo, and its pixels are those of the region run whole.
TEST_F(NativeErodeDilateGpuTest, StripsMatchTheWholeRegionExactly)
{
    for (bool dilate : { false, true }) {
        StripChain chain;
        chain.source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        chain.first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        chain.filter = makeNode(dilate);
        chain.second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        ASSERT_TRUE(bool(chain.source) && bool(chain.first) && bool(chain.filter) && bool(chain.second));
        connectNodes(chain.source, chain.first, 0, true);
        connectNodes(chain.first, chain.filter, 0, true);
        connectNodes(chain.filter, chain.second, 0, true);
        ASSERT_TRUE(setKnobValues(chain.first, "multiply", std::vector<double>(4, 1.5)));
        const double size = dilate ? kStripFilterSize : -kStripFilterSize;
        ASSERT_TRUE(setKnobValues(chain.filter, kErodeDilateParamSize, { size, size }));
        ASSERT_TRUE(setKnobValues(chain.second, "offset", std::vector<double>(4, 0.25)));
        const std::vector<NodePtr> graph({ chain.first, chain.filter, chain.second });
        const RectI window = pixelRoD(chain.second);
        ASSERT_FALSE(window.isNull());
        const std::string name = dilate ? "dilate" : "erode";
        const std::uint64_t outputBytes = (std::uint64_t)window.width() * (std::uint64_t)window.height() * 4u * sizeof(float);
        ASSERT_GT(outputBytes, 2 * kStripBufferBytes) << name << ": the output is too small to need strips";

        const EndToEndResult whole = renderEndToEnd(graph, chain.second, window, kTime);
        if (whole.skipped) {
            return;
        }
        ASSERT_TRUE(whole.ok) << name << ": " << whole.error;
        EXPECT_TRUE(whole.ranInRegion(chain.filter)) << name;
        EXPECT_EQ(1u, whole.gpuTasksPerNode.count(chain.filter->getScriptName_mt_safe())) << name << ": the region fell back to the CPU";

        QTemporaryDir profileDir;
        ASSERT_TRUE(profileDir.isValid());
        EndToEndResult stripped;
        std::string profileFile;
        {
            StripOverride strips(kStripBufferBytes);
            RenderScheduler::setProfilePath(profileDir.path().toStdString() + "/profile.jsonl");
            profileFile = RenderScheduler::getProfileFilePath();
            stripped = renderEndToEnd(graph, chain.second, window, kTime);
            RenderScheduler::setProfilePath(std::string());
        }
        ASSERT_TRUE(stripped.ok) << name << ": " << stripped.error;
        EXPECT_TRUE(stripped.ranInRegion(chain.filter)) << name << " ran on the CPU when stripped";
        EXPECT_EQ(1u, stripped.gpuTasksPerNode.count(chain.filter->getScriptName_mt_safe())) << name << ": the stripped region fell back to the CPU";
        const std::vector<int> regionStrips = profiledRegionStrips(profileFile);
        ASSERT_EQ(1u, regionStrips.size()) << name << ": one GPU region expected in the profile";
        EXPECT_GT(regionStrips[0], 1) << name << ": the region did not run as strips";

        ASSERT_EQ(whole.gpu.size(), stripped.gpu.size()) << name;
        const ImageDiff diff = compareImages(whole.gpu, stripped.gpu, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex;
    }
}
