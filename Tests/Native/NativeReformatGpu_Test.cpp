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
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/FilterKnobRenderChecks.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/Reformat.h"
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

// One setting of the Reformat knobs that decide the matrix. Box 50x20 is fixed, so the output
// format does not follow the source.
struct ReformatCase {
    const char* name;
    const char* type;
    const char* resize;
    bool center;
    bool flip;
    bool flop;
    bool turn;
    bool preserveBB;
};

const ReformatCase kCases[] = {
    { "none", kReformatParamTypeOptionToBox, kReformatParamResizeOptionNone, false, false, false, false, true },
    { "none-center", kReformatParamTypeOptionToBox, kReformatParamResizeOptionNone, true, false, false, false, true },
    { "none-center-clipped", kReformatParamTypeOptionToBox, kReformatParamResizeOptionNone, true, false, false, false, false },
    { "width", kReformatParamTypeOptionToBox, kReformatParamResizeOptionWidth, true, false, false, false, true },
    { "height", kReformatParamTypeOptionToBox, kReformatParamResizeOptionHeight, true, false, false, false, true },
    { "fit", kReformatParamTypeOptionToBox, kReformatParamResizeOptionFit, true, false, false, false, true },
    { "fit-clipped", kReformatParamTypeOptionToBox, kReformatParamResizeOptionFit, true, false, false, false, false },
    { "fill", kReformatParamTypeOptionToBox, kReformatParamResizeOptionFill, true, false, false, false, true },
    { "fill-clipped", kReformatParamTypeOptionToBox, kReformatParamResizeOptionFill, true, false, false, false, false },
    { "distort", kReformatParamTypeOptionToBox, kReformatParamResizeOptionDistort, false, false, false, false, true },
    { "distort-flip", kReformatParamTypeOptionToBox, kReformatParamResizeOptionDistort, false, true, false, false, true },
    { "distort-flop", kReformatParamTypeOptionToBox, kReformatParamResizeOptionDistort, false, false, true, false, false },
    { "distort-turn", kReformatParamTypeOptionToBox, kReformatParamResizeOptionDistort, false, false, false, true, true },
    { "fit-turn-flip-flop", kReformatParamTypeOptionToBox, kReformatParamResizeOptionFit, true, true, true, true, false },
    { "scale", kReformatParamTypeOptionScale, kReformatParamResizeOptionDistort, false, false, false, false, true },
    { "project-fill", kReformatParamTypeOptionToProjectFormat, kReformatParamResizeOptionFill, true, false, false, false, false },
};

} // namespace

class NativeReformatGpuTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeReformatGpu64x48", 1.));
    }

    NodePtr makeNode()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_REFORMAT), PLUGIN_MAJOR_NATRON_REFORMAT);
        EXPECT_TRUE(bool(node));

        return node;
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }

    static bool setCase(const NodePtr& node,
                        const ReformatCase& c)
    {
        return setKnobValue(node, kReformatParamType, std::string(c.type)) && setKnobValues(node, kReformatParamBoxSize, { 50., 20. }) && setKnobValues(node, kReformatParamBoxFixed, { 1. }) && setKnobValues(node, kReformatParamScale, { 0.45, 0.7 }) && setKnobValue(node, kReformatParamResize, std::string(c.resize)) && setKnobValues(node, kReformatParamCenter, { c.center ? 1. : 0. }) && setKnobValues(node, kReformatParamFlip, { c.flip ? 1. : 0. }) && setKnobValues(node, kReformatParamFlop, { c.flop ? 1. : 0. }) && setKnobValues(node, kReformatParamTurn, { c.turn ? 1. : 0. }) && setKnobValues(node, kReformatParamPreserveBoundingBox, { c.preserveBB ? 1. : 0. });
    }
};

TEST_F(NativeReformatGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeNode();
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    connectNodes(source, node, 0, true);
    ASSERT_TRUE(setCase(node, kCases[5]));

    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }
    {
        GpuQuery q = eligibleQuery();
        q.hasConcatenatedInput = true;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << "a transform concatenated from upstream";
    }
}

TEST_F(NativeReformatGpuTest, InputRegionIsTheCpuRegionOfInterest)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);
    for (const ReformatCase& c : kCases) {
        NodePtr node = makeNode();
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setCase(node, c)) << c.name;

        const RectI rect(10, 5, 31, 17);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const RectD window = rect.toCanonical_noClipping(mipmapLevel, 1.);
            RoIMap rois;
            effect->getRegionsOfInterest_public(kTime, scale, window, window, ViewIdx(0), &rois);
            RoIMap::const_iterator found = rois.find(source->getEffectInstance());
            ASSERT_TRUE(found != rois.end()) << c.name << " mipmap " << mipmapLevel;
            EXPECT_EQ(found->second.toPixelEnclosing(scale, 1.), effect->getGpuInputRoI(0, effect->getCurrentTime(), effect->getCurrentView(), rect, scale)) << c.name << " mipmap " << mipmapLevel;
        }
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

GpuQuery
queryFor(const NodePtr& node,
         unsigned mipmapLevel)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    const ImageLayerDesc plane = colorLayer(node);
    GpuQuery q = eligibleQuery();

    q.mappedScale = RenderScale::fromMipmapLevel(mipmapLevel);
    q.nComps = plane.getNumComponents();
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

// The whole region of definition and an odd-sized window inside it.
std::vector<RectI>
testWindows(const RectI& rod)
{
    std::vector<RectI> rois;

    rois.push_back(rod);
    const int x2 = std::min(rod.x2 - 2, rod.x1 + 5 + 23);
    const int y2 = std::min(rod.y2 - 1, rod.y1 + 3 + 17);
    if ((x2 > rod.x1 + 5) && (y2 > rod.y1 + 3)) {
        rois.push_back(RectI(rod.x1 + 5, rod.y1 + 3, x2, y2));
    }

    return rois;
}

// Renders node on the CPU and through renderGpu() over each test window at both mipmap levels,
// expecting the resampling bound.
void
expectGpuMatchesCpu(GpuParityRuntime& runtime,
                    const NodePtr& source,
                    const NodePtr& node,
                    const std::string& base,
                    int* nCompared)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const std::string level = base + " mipmap " + std::to_string(mipmapLevel);
        const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
        ASSERT_FALSE(rod.isNull()) << level;
        const GpuQuery query = queryFor(node, mipmapLevel);
        ASSERT_EQ(4, query.nComps) << level;
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << level;

        for (const RectI& roi : testWindows(rod)) {
            const std::string what = level + " " + describeRoI(roi);
            RenderedPlane cpu;
            ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
            ASSERT_EQ(4u, cpu.channels.size()) << what;
            ASSERT_EQ(roi, cpu.window) << what;

            GpuParityImage image;
            ASSERT_TRUE(sourceImage(source, *effect, mipmapLevel, roi, &image)) << what;
            const GpuRunResult run = runRenderGpu(runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, roi, outputLayout());
            ASSERT_TRUE(run.ok) << what;
            EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";

            ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, ParityTolerance::resampling());
            *nCompared += (int)diff.compared;
            const std::size_t at = diff.firstMismatchIndex / 4;
            EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " mismatches, worst " << diff.worstAbsolute << ", first at ("
                                           << (roi.x1 + (int)(at % roi.width())) << "," << (roi.y1 + (int)(at / roi.width())) << ") channel " << (diff.firstMismatchIndex % 4)
                                           << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
        }
    }
}

} // namespace

// Every resize type, with centre, flip, flop, turn and preserveBB, under the default filter, on the
// parity source moved off the format's origin.
TEST_F(NativeReformatGpuTest, EveryCaseMatchesTheCpuRender)
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
    for (const ReformatCase& c : kCases) {
        NodePtr node = makeNode();
        ASSERT_TRUE(bool(node));
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setCase(node, c)) << c.name;
        expectGpuMatchesCpu(*runtime, source, node, c.name, &nCompared);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_GT(nCompared, 0);
}

// Reformatted to a larger box, shifted or scaled up by a fraction so the filter reads past the
// source, over a source carrying each mode and under each choice: the device reads past the source
// what the CPU does, and the output carries the resolved mode.
TEST_F(NativeReformatGpuTest, ToALargerFormatEdgeModesMatchTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const struct {
        const char* name;
        const char* resize;
        double boxWidth;
        double boxHeight;
    } larger[] = { { "none-center", kReformatParamResizeOptionNone, 97., 81. }, { "fit", kReformatParamResizeOptionFit, 100., 70. } };

    int nCompared = 0;
    for (bool blackSource : { false, true }) {
        for (const char* choice : { "input", "black", "stretch" }) {
            for (const auto& l : larger) {
                resetProject();
                NodePtr parity = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
                ASSERT_TRUE(bool(parity));
                setParitySourceOrigin(parity, 0, 0);
                NodePtr source = parity;
                if (blackSource) {
                    // The parity source carries Stretch; a Crop to its own window under Black gives
                    // the same pixels carrying Black.
                    source = createNodeAtMajor(getApp(), PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP);
                    ASSERT_TRUE(bool(source));
                    connectNodes(parity, source, 0, true);
                    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
                    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)kParitySourceWidth, (double)kParitySourceHeight }));
                    ASSERT_TRUE(setKnobValue(source, kEdgeModeParam, std::string("black")));
                }
                NodePtr node = makeNode();
                ASSERT_TRUE(bool(node));
                connectNodes(source, node, 0, true);
                ASSERT_TRUE(setKnobValue(node, kReformatParamType, std::string(kReformatParamTypeOptionToBox)));
                ASSERT_TRUE(setKnobValues(node, kReformatParamBoxSize, { l.boxWidth, l.boxHeight }));
                ASSERT_TRUE(setKnobValues(node, kReformatParamBoxFixed, { 1. }));
                ASSERT_TRUE(setKnobValue(node, kReformatParamResize, std::string(l.resize)));
                ASSERT_TRUE(setKnobValues(node, kReformatParamCenter, { 1. }));
                ASSERT_TRUE(setKnobValue(node, kEdgeModeParam, std::string(choice)));

                const std::string base = std::string(l.name) + " " + choice + (blackSource ? " over a Black source" : " over a Stretch source");
                const bool stretch = (std::string(choice) == "stretch") || ((std::string(choice) == "input") && !blackSource);
                EXPECT_TRUE(node->getEffectInstance()->getTimeInvariantMetadata()->getEdgeMode() == (stretch ? EdgeMode::Stretch : EdgeMode::Black)) << base;
                expectGpuMatchesCpu(*runtime, source, node, base, &nCompared);
                if (HasFatalFailure()) {
                    return;
                }
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

TEST_F(NativeReformatGpuTest, FilterParameterKnobsReachTheCpuAndGpuRenders)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    const bool hasGpu = createRuntime(runtime);
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));

    const auto make = [&]() {
        NodePtr n = makeNode();
        if (n) {
            connectNodes(source, n, 0, true);
            EXPECT_TRUE(setCase(n, kCases[5]));
        }

        return n;
    };
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    NodePtr node = make();
    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(sincLobesChangeTheRender(node, window, kTime));
    EXPECT_TRUE(bicubicParametersChangeTheRender(node, window, kTime));

    if (!hasGpu) {
        return;
    }
    int nCompared = 0;
    for (double lobes : { 2., 8. }) {
        NodePtr sinc = make();
        ASSERT_TRUE(bool(sinc));
        ASSERT_TRUE(selectFilter(sinc, Resampler::eFilterSinc));
        ASSERT_TRUE(setKnobValues(sinc, kResamplerParamLobes, { lobes }));
        expectGpuMatchesCpu(*runtime, source, sinc, "sinc " + std::to_string((int)lobes), &nCompared);
        if (HasFatalFailure()) {
            return;
        }
    }
    NodePtr bicubic = make();
    ASSERT_TRUE(bool(bicubic));
    ASSERT_TRUE(selectFilter(bicubic, Resampler::eFilterBicubic));
    ASSERT_TRUE(setKnobValues(bicubic, kResamplerParamBicubicB, { 0. }));
    ASSERT_TRUE(setKnobValues(bicubic, kResamplerParamBicubicC, { 0.5 }));
    expectGpuMatchesCpu(*runtime, source, bicubic, "bicubic b0 c0.5", &nCompared);
    EXPECT_GT(nCompared, 0);
}

// Each filter, with clamp off and (if the filter honours it) on and black outside on and off, under
// a minifying, a turning and a magnifying setting. One test per filter, as the sweep of every filter
// is long in a debug build.
class NativeReformatGpuFilterTest
    : public NativeReformatGpuTest,
      public ::testing::WithParamInterface<int> {
};

TEST_P(NativeReformatGpuFilterTest, ResamplingMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
    ASSERT_EQ((std::size_t)Resampler::eFilterBicubic + 1, filters.size());
    const std::size_t f = (std::size_t)GetParam();
    const bool honoursClamp = Resampler::filterHonoursClamp((Resampler::FilterEnum)f);
    const ReformatCase* const cases[] = { &kCases[5], &kCases[12], &kCases[14] };
    int nCompared = 0;
    for (const ReformatCase* c : cases) {
        for (bool clamp : { false, true }) {
            if (clamp && !honoursClamp) {
                continue;
            }
            for (bool blackOutside : { false, true }) {
                NodePtr node = makeNode();
                ASSERT_TRUE(bool(node));
                connectNodes(source, node, 0, true);
                ASSERT_TRUE(setCase(node, *c));
                ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(filters[f].id)));
                ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { clamp ? 1. : 0. }));
                ASSERT_TRUE(setKnobValue(node, kEdgeModeParam, std::string(blackOutside ? "black" : "stretch")));
                std::ostringstream base;
                base << filters[f].id << " " << c->name << (clamp ? " clamp" : "") << (blackOutside ? " black-outside" : "");
                expectGpuMatchesCpu(*runtime, source, node, base.str(), &nCompared);
                if (HasFatalFailure()) {
                    return;
                }
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

INSTANTIATE_TEST_CASE_P(Filters, NativeReformatGpuFilterTest, ::testing::Range(0, (int)Resampler::eFilterBicubic + 1));

#endif // NATRON_HAVE_GPU
