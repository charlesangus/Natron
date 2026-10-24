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
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/Position.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#include <ofxNatron.h>

NATRON_NAMESPACE_USING

static_assert(GpuOffsetResampleOp<PositionOp>);
static_assert(!GpuMatrixResampleOp<PositionOp>);

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

struct TranslateCase {
    const char* name;
    double x;
    double y;
};

// Whole pixels in both signs, fractions rounded up and down at the pixel and at half scale, and a
// half that rounds toward positive infinity.
const TranslateCase kTranslations[] = {
    { "integer", 7., 5. },
    { "negative", -9., -6. },
    { "fractional", 3.4, -2.6 },
    { "half", 2.5, -2.5 },
    { "mixed", 13.75, -3. },
};

} // namespace

class NativePositionGpuTest
    : public BaseTest {
protected:
    // A fresh project whose default format is 64x48 at the given pixel aspect ratio.
    void resetProject(double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativePositionGpuPar" + std::to_string(par), par));
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }

    // A parity source moved off the origin, connected to a new Position translated by x, y.
    bool createGraph(double x,
                     double y,
                     NodePtr* source,
                     NodePtr* node)
    {
        *source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        *node = createNode(QString::fromUtf8(PLUGINID_NATRON_POSITION), PLUGIN_MAJOR_NATRON_POSITION);
        EXPECT_TRUE(bool(*source));
        EXPECT_TRUE(bool(*node));
        if (!*source || !*node) {
            return false;
        }
        setParitySourceComponents(*source, "rgba");
        setParitySourceOrigin(*source, 3, 5);
        connectNodes(*source, *node, 0, true);

        return setKnobValues(*node, kPositionParamTranslate, { x, y });
    }

    static bool setProcessChannels(const NodePtr& node,
                                   const std::bitset<4>& channels)
    {
        KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
        if (!channelSet) {
            return false;
        }
        const char* const names[4] = { "R", "G", "B", "A" };
        std::vector<std::string> enabled;
        for (int c = 0; c < 4; ++c) {
            if (channels[c]) {
                enabled.push_back(names[c]);
            }
        }
        channelSet->setChannels(0, enabled);

        return true;
    }
};

TEST_F(NativePositionGpuTest, EligibilityFollowsTheRequest)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source;
    NodePtr node;
    ASSERT_TRUE(createGraph(7., -5., &source, &node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
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
}

TEST_F(NativePositionGpuTest, InputRegionIsTheNodesRegionOfInterest)
{
    resetProject();
    NodePtr source;
    NodePtr node;
    ASSERT_TRUE(createGraph(7., -5., &source, &node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        const RectD window = rect.toCanonical_noClipping(mipmapLevel, 1.);
        RoIMap rois;
        effect->getRegionsOfInterest_public(kTime, scale, window, window, ViewIdx(0), &rois);
        RoIMap::const_iterator found = rois.find(source->getEffectInstance());
        ASSERT_TRUE(found != rois.end()) << "mipmap " << mipmapLevel;
        EXPECT_EQ(found->second.toPixelEnclosing(scale, 1.), effect->getGpuInputRoI(0, rect, scale)) << "mipmap " << mipmapLevel;
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

// The render bypasses the caches, so the source it fetches covers exactly its region of interest,
// the region the GPU is handed, and no larger image an earlier render left behind.
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
    bool isProjectFormat = false;
    const StatusEnum stat = effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod, &isProjectFormat);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(mipmapLevel, effect->getAspectRatio(-1));
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

// The source's pixels over bounds, inside a larger buffer at an offset and with padded rows.
bool
sourceImage(const NodePtr& source,
            unsigned mipmapLevel,
            const RectI& bounds,
            GpuParityImage* image)
{
    RenderedPlane in;
    if (!renderColor(source, mipmapLevel, bounds, &in)) {
        return false;
    }
    EXPECT_EQ(bounds, in.window);
    image->bounds = in.window;
    image->nComps = (int)in.channels.size();
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

void
expectExact(const std::vector<float>& expected,
            const GpuRunResult& run,
            const RectI& roi,
            int nComps,
            const std::string& what)
{
    ASSERT_TRUE(run.ok) << what;
    EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";
    ASSERT_EQ(expected.size(), run.pixels.size()) << what;
    const ImageDiff diff = compareImages(expected, run.pixels, ParityTolerance::exact());
    EXPECT_GT(diff.compared, 0u) << what;
    const std::size_t at = diff.firstMismatchIndex / nComps;
    EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " mismatches, first at (" << (roi.x1 + (int)(at % roi.width())) << ","
                                   << (roi.y1 + (int)(at / roi.width())) << ") channel " << (diff.firstMismatchIndex % nComps) << " gpu "
                                   << run.pixels[diff.firstMismatchIndex] << " cpu " << expected[diff.firstMismatchIndex];
}

// Renders roi on the CPU and on the device from the source over the node's region of interest of
// roi, clipped to the source's own region, and compares them exactly. The pixels of roi whose
// unshifted position lies outside that source are counted into *outside.
void
expectShiftMatches(GpuParityRuntime& runtime,
                   const NodePtr& source,
                   const NodePtr& node,
                   unsigned mipmapLevel,
                   const RectI& roi,
                   const std::string& what,
                   std::size_t* outside = NULL)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL) << what;
    const GpuQuery query = queryFor(node, mipmapLevel);
    ASSERT_EQ(4, query.nComps) << what;
    ASSERT_TRUE(effect->canRenderOnGpu(query)) << what;

    // A window wholly outside the node's region of definition renders no plane on the CPU; the
    // frame there is zero.
    RenderedPlane cpu;
    if (roi.intersects(pixelRegionOfDefinition(node, mipmapLevel))) {
        ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
        ASSERT_EQ(4u, cpu.channels.size()) << what;
        ASSERT_EQ(roi, cpu.window) << what;
    } else {
        cpu.pixels.assign((std::size_t)roi.area() * 4, 0.f);
    }

    const RectI bounds = effect->getGpuInputRoI(0, roi, RenderScale::fromMipmapLevel(mipmapLevel)).intersect(pixelRegionOfDefinition(source, mipmapLevel));
    std::vector<GpuParityImage> inputs(1);
    if (!bounds.isNull()) {
        ASSERT_TRUE(sourceImage(source, mipmapLevel, bounds, &inputs[0])) << what;
    }
    if (outside) {
        *outside = 0;
        const RectI roiSource = effect->getGpuInputRoI(0, roi, RenderScale::fromMipmapLevel(mipmapLevel));
        for (int y = roi.y1; y < roi.y2; ++y) {
            for (int x = roi.x1; x < roi.x2; ++x) {
                if (!roiSource.contains(x, y)) {
                    ++*outside;
                }
            }
        }
    }
    const GpuRunResult run = runRenderGpu(runtime, *effect, query, inputs, nullptr, roi, outputLayout());
    expectExact(cpu.pixels, run, roi, 4, what);
}

} // namespace

// Every translation over the whole region of definition and a window inside it, at both mipmap
// levels and at an anamorphic pixel aspect ratio, into padded output views.
TEST_F(NativePositionGpuTest, ShiftMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (double par : { 1., 2., 0.5 }) {
        resetProject(par);
        for (const TranslateCase& c : kTranslations) {
            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                NodePtr source;
                NodePtr node;
                ASSERT_TRUE(createGraph(c.x, c.y, &source, &node));
                const std::string level = std::string(c.name) + " par " + std::to_string(par) + " mipmap " + std::to_string(mipmapLevel);
                const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
                ASSERT_FALSE(rod.isNull()) << level;
                for (const RectI& roi : testWindows(rod)) {
                    expectShiftMatches(*runtime, source, node, mipmapLevel, roi, level + " " + describeRoI(roi));
                    if (HasFatalFailure()) {
                        return;
                    }
                }
            }
        }
    }
}

// The window is the source's own region, so the shift moves part of the source, or all of it, out
// of the window and leaves zero behind.
TEST_F(NativePositionGpuTest, SourceMovedOutOfTheWindow)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    const TranslateCase cases[] = {
        { "partly out", 40., 30. },
        { "partly out negative", -35., -25. },
        { "wholly out", 200., 0. },
        { "wholly out below", 0., -150. },
    };
    for (const TranslateCase& c : cases) {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            NodePtr source;
            NodePtr node;
            ASSERT_TRUE(createGraph(c.x, c.y, &source, &node));
            const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
            ASSERT_FALSE(window.isNull()) << c.name;
            expectShiftMatches(*runtime, source, node, mipmapLevel, window, std::string(c.name) + " mipmap " + std::to_string(mipmapLevel));
            if (HasFatalFailure()) {
                return;
            }
        }
    }
}

// Unprocessed channels are the source under the output pixel, unshifted. With the alpha and then
// the red and blue channels left out, they come from the unshifted position, which for the pixels
// near the window's edge lies outside the source the device is given: both sides then read zero,
// the CPU because the uncached source it fetched is exactly that region.
TEST_F(NativePositionGpuTest, UnprocessedChannelsReadTheUnshiftedSource)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    const std::bitset<4> subsets[] = { std::bitset<4>(0x7), std::bitset<4>(0xa) };
    const TranslateCase cases[] = {
        { "positive", 7., 5. },
        { "negative", -9., -6. },
        { "fractional", 3.4, -2.6 },
    };
    for (const std::bitset<4>& channels : subsets) {
        for (const TranslateCase& c : cases) {
            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                NodePtr source;
                NodePtr node;
                ASSERT_TRUE(createGraph(c.x, c.y, &source, &node));
                ASSERT_TRUE(setProcessChannels(node, channels));
                const std::string level = std::string(c.name) + " channels " + channels.to_string() + " mipmap " + std::to_string(mipmapLevel);
                const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
                ASSERT_FALSE(rod.isNull()) << level;
                const GpuQuery query = queryFor(node, mipmapLevel);
                for (int i = 0; i < 4; ++i) {
                    ASSERT_EQ(channels[i], query.processChannels[i]) << level << " channel " << i;
                }
                for (const RectI& roi : testWindows(rod)) {
                    std::size_t outside = 0;
                    expectShiftMatches(*runtime, source, node, mipmapLevel, roi, level + " " + describeRoI(roi), &outside);
                    EXPECT_GT(outside, 0u) << level << " " << describeRoI(roi) << ": no pixel reads its unprocessed channels from outside the source region";
                    if (HasFatalFailure()) {
                        return;
                    }
                }
            }
        }
    }
}

// The node placed in a GPU region of the scheduler renders the frame the CPU does.
TEST_F(NativePositionGpuTest, EndToEndRunsOnTheGpu)
{
    resetProject();
    NodePtr source;
    NodePtr node;
    ASSERT_TRUE(createGraph(7., -5., &source, &node));
    const RectI window = pixelRegionOfDefinition(node, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, node), node, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.ranInRegion(node));
    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::exact());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}

#endif // NATRON_HAVE_GPU
