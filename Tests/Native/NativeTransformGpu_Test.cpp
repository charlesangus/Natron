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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Native/FilterKnobRenderChecks.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/GpuRegionStrips.h"
#include "Engine/GpuWeight.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/ResampleExecutor.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderScheduler.h"
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

// One geometry of the parity tests, about the centre (32, 24).
struct GeometryCase {
    const char* name;
    double translateX;
    double translateY;
    double rotate;
    double scaleX;
    double scaleY;
    double skewX;
    double skewY;
    bool skewOrderYX;
};

// Translation alone, magnification, two minifications (supersampling levels 1 and 2), a rotation,
// a rotation while minifying, and a skew, with the scales translated off the pixel grid. Then
// near-ties: a scale of 1 + 2^-50 whose inverse two floats round to 1, putting the positions within
// a few double ulps of integers in x and of half-integers in y, where the impulse tap and the box
// filter's edge decisions follow the last bits of the CPU's doubles.
const GeometryCase kGeometries[] = {
    { "translate", 3.25, -2.5, 0., 1., 1., 0., 0., false },
    { "scale-up", 0.2713, 0.1371, 0., 1.7, 1.4, 0., 0., false },
    { "scale-down", 0.3172, -0.2139, 0., 0.45, 0.3, 0., 0., false },
    { "scale-down-far", 1.5172, 0.1371, 0., 0.12, 0.2, 0., 0., false },
    { "rotate30", 0., 0., 30., 1., 1., 0., 0., false },
    { "rotate30-scale05", 0., 0., 30., 0.5, 0.5, 0., 0., false },
    { "skew-yx", 3.25, -2.5, 0., 1., 1., 0.3, -0.2, true },
    { "near-ties", 0.5, 0., 0., 1. + 0x1p-50, 1. + 0x1p-50, 0., 0., false },
};

} // namespace

class NativeTransformGpuTest
    : public BaseTest {
protected:
    // A 64x48 project format, the parity source's size.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeTransformGpu64x48", 1.));
    }

    // The centre is set explicitly so it does not depend on when the node reset it on connection.
    NodePtr makeNode()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
        EXPECT_TRUE(bool(node));

        return node;
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }

    // Keys translate, rotate and scale at times 0 and 2 about the centre (32, 24), so the matrices
    // change over any shutter around kTime and the scale minifies at its start.
    static bool animate(const NodePtr& node)
    {
        if (!setKnobValues(node, kTransformNodeParamCenter, { 32., 24. })) {
            return false;
        }
        const struct {
            const char* name;
            std::vector<double> at0;
            std::vector<double> at2;
        } keys[] = {
            { kTransformNodeParamTranslate, { -6., 3. }, { 8., -4. } },
            { kTransformNodeParamRotate, { -12. }, { 18. } },
            { kTransformNodeParamScale, { 0.7, 0.7 }, { 1.1, 0.9 } },
        };
        for (const auto& k : keys) {
            KnobDouble* knob = dynamic_cast<KnobDouble*>(node->getKnobByName(k.name).get());
            EXPECT_TRUE(knob != NULL) << k.name;
            if (!knob) {
                return false;
            }
            for (int d = 0; d < (int)k.at0.size(); ++d) {
                knob->setValueAtTime(0., k.at0[d], ViewSpec::all(), d);
                knob->setValueAtTime(2., k.at2[d], ViewSpec::all(), d);
            }
        }

        return true;
    }

    static bool setGeometry(const NodePtr& node,
                            const GeometryCase& g)
    {
        return setKnobValues(node, kTransformNodeParamCenter, { 32., 24. }) && setKnobValues(node, kTransformNodeParamTranslate, { g.translateX, g.translateY }) && setKnobValues(node, kTransformNodeParamRotate, { g.rotate }) && setKnobValues(node, kTransformNodeParamScale, { g.scaleX, g.scaleY }) && setKnobValues(node, kTransformNodeParamSkewX, { g.skewX }) && setKnobValues(node, kTransformNodeParamSkewY, { g.skewY }) && setKnobValue(node, kTransformNodeParamSkewOrder, std::string(g.skewOrderYX ? "YX" : "XY"));
    }
};

TEST_F(NativeTransformGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    for (bool masked : { false, true }) {
        NodePtr node = makeNode();
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setGeometry(node, kGeometries[5]));
        const std::string name = masked ? "Transform under a mask" : "Transform";

        for (int nComps : { 1, 2, 3, 4 }) {
            GpuQuery q = eligibleQuery();
            q.nComps = nComps;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << name << " " << nComps << " components";
        }
        for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
            GpuQuery q = eligibleQuery();
            q.bitDepth = depth;
            EXPECT_FALSE(effect->canRenderOnGpu(q)) << name << " bit depth " << (int)depth;
        }
        {
            GpuQuery q = eligibleQuery();
            q.draftMode = true;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << name << " draft";
            q.draftMode = false;
            q.hasConcatenatedInput = true;
            EXPECT_FALSE(effect->canRenderOnGpu(q)) << name << " a transform concatenated from upstream";
        }
        if (masked) {
            GpuQuery q = eligibleQuery();
            q.hasMask = true;
            q.mix = 0.5;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << name << " mask and mix";
        }

        ASSERT_TRUE(animate(node));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 1. }));
        EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << name << " motion blur";
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamShutter, { 0. }));
        EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << name << " motion blur with a closed shutter";
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 0. }));
        EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << name << " no motion blur";
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamDirectionalBlur, { 1. }));
        EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << name << " directional blur at quality 0";
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 1. }));
        EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << name << " directional blur";
        {
            GpuQuery q = eligibleQuery();
            q.hasConcatenatedInput = true;
            EXPECT_FALSE(effect->canRenderOnGpu(q)) << name << " directional blur with a transform concatenated from upstream";
        }
    }
}

// Transform is Heavy only while the knobs ask for motion blur, with or without a mix; otherwise
// it keeps the Resample default.
TEST_F(NativeTransformGpuTest, WeightIsHeavyWithMotionBlur)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    for (bool mixed : { false, true }) {
        const std::string name = mixed ? "Transform at mix 0.5" : "Transform";
        NodePtr node = makeNode();
        ASSERT_TRUE(bool(node));
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(animate(node));
        if (mixed) {
            ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
        }
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);

        EXPECT_EQ(GpuWeight::Class::Filter, effect->gpuWeight(kTime, ViewIdx(0), 0).weightClass) << name << " motion blur 0";
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 1. }));
        EXPECT_TRUE(effect->gpuWeight(kTime, ViewIdx(0), 0).isHeavy()) << name << " motion blur 1";
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamShutter, { 0. }));
        EXPECT_EQ(GpuWeight::Class::Filter, effect->gpuWeight(kTime, ViewIdx(0), 0).weightClass) << name << " closed shutter";
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamDirectionalBlur, { 1. }));
        EXPECT_TRUE(effect->gpuWeight(kTime, ViewIdx(0), 0).isHeavy()) << name << " directional blur";
    }
}

TEST_F(NativeTransformGpuTest, InputRegionIsTheCpuRegionOfInterest)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);
    for (bool masked : { false, true }) {
        NodePtr node = makeNode();
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setGeometry(node, kGeometries[5]));
        if (masked) {
            ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
        }

        const RectI rect(10, 20, 31, 47);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const RectD window = rect.toCanonical_noClipping(mipmapLevel, 1.);
            RoIMap rois;
            effect->getRegionsOfInterest_public(kTime, scale, window, window, ViewIdx(0), &rois);
            RoIMap::const_iterator found = rois.find(source->getEffectInstance());
            ASSERT_TRUE(found != rois.end()) << "mipmap " << mipmapLevel;
            EXPECT_EQ(found->second.toPixelEnclosing(scale, 1.), effect->getGpuInputRoI(0, effect->getCurrentTime(), effect->getCurrentView(), rect, scale)) << (masked ? "masked" : "plain") << " mipmap " << mipmapLevel;
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

// Pixels of a window that may miss the tolerance, as long as every value of theirs is within
// `absolute`: one in `pixelsPer` of the window's pixels, and at least `minPixels`.
struct ToleranceExceptions {
    double absolute = 0.;
    std::size_t pixelsPer = 0;
    std::size_t minPixels = 0;
};

// Expects the pixels of `got` missing `tolerance` to be few enough and close enough for
// `exceptions`.
void
expectWithinExceptions(const std::vector<float>& cpu,
                       const std::vector<float>& got,
                       const ParityTolerance& tolerance,
                       const ToleranceExceptions& exceptions,
                       const std::string& what)
{
    const ParityTolerance loose = ParityTolerance::make(exceptions.absolute, 0.);
    const std::size_t nPixels = cpu.size() / 4;
    std::size_t missed = 0;
    std::size_t beyond = 0;
    double worst = 0.;
    for (std::size_t p = 0; p < nPixels; ++p) {
        bool misses = false;
        for (std::size_t c = 0; c < 4; ++c) {
            const std::size_t i = p * 4 + c;
            if (!tolerance.accepts(cpu[i], got[i])) {
                misses = true;
                worst = (std::max)(worst, std::fabs((double)cpu[i] - (double)got[i]));
                if (!loose.accepts(cpu[i], got[i])) {
                    ++beyond;
                }
            }
        }
        if (misses) {
            ++missed;
        }
    }
    const std::size_t allowed = (std::max)(exceptions.minPixels, nPixels / exceptions.pixelsPer);
    EXPECT_EQ(0u, beyond) << what << ": " << beyond << " values beyond " << exceptions.absolute << ", worst " << worst;
    EXPECT_LE(missed, allowed) << what << ": " << missed << " of " << nPixels << " pixels miss the tolerance, worst " << worst;
}

// Renders node on the CPU and through renderGpu() over each test window at both mipmap levels,
// expecting the resampling bound; `mask`, when given, is the mask input's source. With
// `exceptions`, a few pixels may miss the tolerance as those allow.
void
expectGpuMatchesCpu(GpuParityRuntime& runtime,
                    const NodePtr& source,
                    const NodePtr& maskSource,
                    const NodePtr& node,
                    const std::string& base,
                    int* nCompared,
                    const ParityTolerance& tolerance = ParityTolerance::resampling(),
                    const ToleranceExceptions* exceptions = nullptr)
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

        GpuParityImage mask;
        if (maskSource) {
            RenderedPlane maskRendered;
            ASSERT_TRUE(renderColor(maskSource, mipmapLevel, paritySourceWindow(maskSource, kTime, mipmapLevel), &maskRendered)) << level;
            mask.bounds = maskRendered.window;
            mask.nComps = (int)maskRendered.channels.size();
            mask.pixels = maskRendered.pixels;
            mask.layout.padFloats = 3;
            mask.layout.rowPadFloats = 5;
        }

        for (const RectI& roi : testWindows(rod)) {
            const std::string what = level + " " + describeRoI(roi);
            RenderedPlane cpu;
            ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
            ASSERT_EQ(4u, cpu.channels.size()) << what;
            ASSERT_EQ(roi, cpu.window) << what;

            GpuParityImage image;
            ASSERT_TRUE(sourceImage(source, *effect, mipmapLevel, roi, &image)) << what;
            const GpuRunResult run = runRenderGpu(runtime, *effect, query, std::vector<GpuParityImage>(1, image), query.hasMask ? &mask : nullptr, roi, outputLayout());
            ASSERT_TRUE(run.ok) << what;
            EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";

            ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, tolerance);
            *nCompared += (int)diff.compared;
            if (exceptions) {
                expectWithinExceptions(cpu.pixels, run.pixels, tolerance, *exceptions, what);
                continue;
            }
            const std::size_t at = diff.firstMismatchIndex / 4;
            EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " mismatches, worst " << diff.worstAbsolute << ", first at ("
                                           << (roi.x1 + (int)(at % roi.width())) << "," << (roi.y1 + (int)(at / roi.width())) << ") channel " << (diff.firstMismatchIndex % 4)
                                           << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
        }
    }
}

} // namespace

// The filter of the parameter, with clamp off and (if the filter honours it) on and black outside
// on and off, under each geometry, against the CPU render fed with the source over
// getGpuInputRoI(), on the parity source moved off the format's origin. One test per filter: the
// sweep of every filter took most of the per-test timeout in a debug build on a loaded CI runner.
class NativeTransformGpuFilterTest
    : public NativeTransformGpuTest,
      public ::testing::WithParamInterface<int> {
};

TEST_P(NativeTransformGpuFilterTest, ResamplingMatchesTheCpuRender)
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
    int nCompared = 0;
    for (const GeometryCase& g : kGeometries) {
        for (bool clamp : { false, true }) {
            if (clamp && !honoursClamp) {
                continue;
            }
            for (bool blackOutside : { false, true }) {
                // A fresh node per setting, so no region of definition cached under an earlier
                // setting is reused.
                NodePtr node = makeNode();
                ASSERT_TRUE(bool(node));
                connectNodes(source, node, 0, true);
                ASSERT_TRUE(setGeometry(node, g));
                ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(filters[f].id)));
                ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { clamp ? 1. : 0. }));
                ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { blackOutside ? 1. : 0. }));
                std::ostringstream base;
                base << filters[f].id << " " << g.name << (clamp ? " clamp" : "") << (blackOutside ? " black-outside" : "");
                expectGpuMatchesCpu(*runtime, source, NodePtr(), node, base.str(), &nCompared);
                if (HasFatalFailure()) {
                    return;
                }
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

INSTANTIATE_TEST_CASE_P(Filters, NativeTransformGpuFilterTest, ::testing::Range(0, (int)Resampler::eFilterBicubic + 1));

namespace {

enum MaskMode {
    eMaskNone,
    eMaskOn,
    eMaskInverted
};

struct MaskedCase {
    const char* name;
    MaskMode mask;
    double mix;
};

} // namespace

// Transform's mask-mix05 setting (rotate 30, scale 0.5, the mask enabled, mix 0.5) and its
// neighbours: an inverted mask, the mask at mix 1, and mix 0.5 without a mask.
TEST_F(NativeTransformGpuTest, MaskedMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    const MaskedCase cases[] = {
        { "mask-mix05", eMaskOn, 0.5 },
        { "inverted-mask-mix05", eMaskInverted, 0.5 },
        { "mask", eMaskOn, 1. },
        { "mix05", eMaskNone, 0.5 },
    };
    int nCompared = 0;
    for (const MaskedCase& c : cases) {
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
        ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { (c.mask != eMaskNone) ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMaskInvertParamName, { (c.mask == eMaskInverted) ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { c.mix }));
        ASSERT_TRUE(setGeometry(node, kGeometries[5]));
        ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("cubic")));

        expectGpuMatchesCpu(*runtime, source, (c.mask != eMaskNone) ? maskSource : NodePtr(), node, c.name, &nCompared);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_GT(nCompared, 0);
}

namespace {

struct MotionBlurCase {
    const char* name;
    double motionBlur;
    bool directional;
    const char* shutterOffset;
    double customOffset;
    const char* filter;
    bool blackOutside;
};

} // namespace

// Motion blur at qualities 0.5, 1 and 4 over every shutter offset, and directional blur, which
// weights its matrices, against the CPU render under the IIR bound: a pixel averages up to
// motionBlur x 40 samples. A directional blur at quality 0 samples one matrix, under the resampling
// bound.
//
// A few blurred pixels may miss the IIR bound by up to the sampler's error target,
// motionBlurMaxError(). Each pixel stops sampling once floor(variance / maxError^2) is reached, and
// the device's float filters round each sample a few ulps away from the CPU's double ones, so a
// pixel whose quotient lies that close to an integer takes one sample more or fewer. That moves its
// mean by about maxError / sqrt(samples), an estimate the stop test accepts as readily as the CPU's.
// Lavapipe misses the IIR bound on at most 7 values per mipmap level here, RADV on one; one pixel
// in 250 of a window, and at least 2, leaves headroom.
TEST_F(NativeTransformGpuTest, MotionBlurMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    const MotionBlurCase cases[] = {
        { "motion-blur-05-start", 0.5, false, "start", 0., "bilinear", false },
        { "motion-blur-1-centered", 1., false, "centered", 0., "cubic", true },
        { "motion-blur-4-end", 4., false, "end", 0., "cubic", false },
        { "motion-blur-1-custom", 1., false, "custom", -0.35, "keys", false },
        { "directional-blur-1", 1., true, "start", 0., "cubic", false },
        { "directional-blur-0", 0., true, "start", 0., "cubic", false },
    };
    int nCompared = 0;
    for (const MotionBlurCase& c : cases) {
        NodePtr node = makeNode();
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(animate(node));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { c.motionBlur }));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamDirectionalBlur, { c.directional ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamShutter, { 1. }));
        ASSERT_TRUE(setKnobValue(node, kResamplerParamShutterOffset, std::string(c.shutterOffset)));
        ASSERT_TRUE(setKnobValues(node, kResamplerParamShutterCustomOffset, { c.customOffset }));
        ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(c.filter)));
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { c.blackOutside ? 1. : 0. }));
        const bool blurred = c.motionBlur != 0.;
        const TransformNode* transform = dynamic_cast<const TransformNode*>(effect);
        ASSERT_TRUE(transform != NULL);
        EXPECT_EQ(blurred, transform->rendersWithMotionBlur(kTime, ViewIdx(0))) << c.name;

        ToleranceExceptions exceptions;
        exceptions.absolute = Resampler::motionBlurMaxError(c.motionBlur);
        exceptions.pixelsPer = 250;
        exceptions.minPixels = 2;
        expectGpuMatchesCpu(*runtime, source, NodePtr(), node, c.name, &nCompared,
                            blurred ? ParityTolerance::iir() : ParityTolerance::resampling(), blurred ? &exceptions : nullptr);
        if (HasFatalFailure()) {
            return;
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

// The device time of an 8K (7680x4320) RGBA Transform with motion blur at qualities 1 and 4, its
// source and output resident: the wall time of submitting renderGpu()'s work alone and waiting for
// it, the fastest of three after a warm-up. Only runs with NATRON_GPU_BENCH=1, and is meant for a
// discrete device such as RADV; the times go to stdout and to the test's properties.
TEST_F(NativeTransformGpuTest, MotionBlur8kKernelTime)
{
    const char* bench = std::getenv("NATRON_GPU_BENCH");
    if (!bench || (std::string(bench) != "1")) {
        std::printf("skipped: set NATRON_GPU_BENCH=1 to time the 8K motion blur\n");

        return;
    }
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const int width = 7680;
    const int height = 4320;
    ProjectPtr project = getApp()->getProject();
    project->reset(false, true);
    project->setAutoSetProjectFormatEnabled(false);
    project->setOrAddProjectFormat(Format(0, 0, width, height, "nativeTransformGpu8k", 1.));

    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr node = makeNode();
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(bool(source) && (effect != NULL));
    connectNodes(source, node, 0, true);
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamCenter, { width / 2., height / 2. }));
    for (const char* name : { kTransformNodeParamTranslate, kTransformNodeParamRotate }) {
        KnobDouble* knob = dynamic_cast<KnobDouble*>(node->getKnobByName(name).get());
        ASSERT_TRUE(knob != NULL) << name;
        const bool translate = std::string(name) == kTransformNodeParamTranslate;
        knob->setValueAtTime(0., translate ? -200. : -5., ViewSpec::all(), 0);
        knob->setValueAtTime(2., translate ? 200. : 5., ViewSpec::all(), 0);
        if (translate) {
            knob->setValueAtTime(0., -80., ViewSpec::all(), 1);
            knob->setValueAtTime(2., 80., ViewSpec::all(), 1);
        }
    }
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamShutter, { 1. }));
    ASSERT_TRUE(setKnobValue(node, kResamplerParamShutterOffset, std::string("centered")));
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("cubic")));

    const RectI rect(0, 0, width, height);
    GpuQuery query = eligibleQuery();
    query.mappedScale = RenderScale::identity;
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 1. }));
    const RectI inputRoI = effect->getGpuInputRoI(0, effect->getCurrentTime(), effect->getCurrentView(), rect, RenderScale::identity);
    ASSERT_FALSE(inputRoI.isNull());

    // A pattern with edges in every direction, so the variance asks for more samples where it moves.
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
    gpu::GpuRuntime& vk = runtime->runtime();
    gpu::GpuBuffer* srcBuffer = runtime->buffer(0, pixels.size());
    ASSERT_TRUE(srcBuffer != NULL);
    ASSERT_TRUE(moveToDevice(vk, *srcBuffer, pixels));
    std::vector<float>().swap(pixels);
    gpu::GpuBuffer* dstBuffer = runtime->buffer(1, (std::size_t)rect.area() * 4);
    ASSERT_TRUE(dstBuffer != NULL);

    GpuImageView input;
    input.buffer = srcBuffer;
    input.bounds = inputRoI;
    input.rowStrideFloats = inputRoI.width() * 4;
    input.nComps = 4;
    const double megapixels = (double)rect.area() / 1e6;
    for (double quality : { 1., 4. }) {
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { quality }));
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << "quality " << quality;
        ASSERT_EQ(inputRoI, effect->getGpuInputRoI(0, effect->getCurrentTime(), effect->getCurrentView(), rect, RenderScale::identity)) << "quality " << quality;
        double best = 0.;
        for (int run = 0; run < 4; ++run) {
            std::unique_ptr<gpu::GpuRecordContext> context;
            const gpu::GpuStatus created = vk.newRecordContext(context);
            ASSERT_TRUE(bool(created)) << created.message;
            GpuRenderArgs args(*context);
            args.query = query;
            args.outputRect = rect;
            args.inputs.push_back(input);
            args.output.buffer = dstBuffer;
            args.output.bounds = rect;
            args.output.rowStrideFloats = rect.width() * 4;
            args.output.nComps = 4;
            ASSERT_EQ(eStatusOK, effect->renderGpu(args)) << "quality " << quality;
            const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            const gpu::GpuStatus submitted = context->submitAndWait(600'000'000'000ull);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            ASSERT_TRUE(bool(submitted)) << submitted.message;
            if ((run == 1) || ((run > 1) && (ms < best))) {
                best = ms;
            }
        }
        std::printf("transform motion blur %g, 8K: %.1f ms, %.3f ms/Mpx\n", quality, best, best / megapixels);
        const std::string key = "motion_blur_" + std::to_string((int)quality) + "_8k_ms";
        RecordProperty(key, std::to_string(best));
    }
}

#endif // NATRON_HAVE_GPU

namespace {

struct Chain {
    NodePtr source;
    NodePtr first;
    NodePtr transform;
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

int
regionsHolding(const EndToEndResult& result,
               const Chain& chain,
               bool* together)
{
    int holding = 0;
    *together = true;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), chain.first) != nodes.end();
        const bool hasTransform = std::find(nodes.begin(), nodes.end(), chain.transform) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), chain.second) != nodes.end();
        if (hasFirst || hasTransform || hasSecond) {
            ++holding;
            *together = *together && hasFirst && hasTransform && hasSecond;
        }
    }

    return holding;
}

} // namespace

TEST_F(NativeTransformGpuTest, GradeTransformGradeChainRunsAsOneRegion)
{
    Chain chain;
    chain.source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    chain.first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    chain.transform = makeNode();
    chain.second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(chain.source) && bool(chain.first) && bool(chain.transform) && bool(chain.second));
    connectNodes(chain.source, chain.first, 0, true);
    connectNodes(chain.first, chain.transform, 0, true);
    connectNodes(chain.transform, chain.second, 0, true);
    ASSERT_TRUE(setKnobValues(chain.first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setGeometry(chain.transform, kGeometries[4]));
    ASSERT_TRUE(setKnobValues(chain.second, "offset", std::vector<double>(4, 0.25)));

    const RectI window = paritySourceWindow(chain.source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ chain.first, chain.transform, chain.second }), chain.second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    bool together = false;
    EXPECT_EQ(1, regionsHolding(result, chain, &together));
    EXPECT_TRUE(together);
    EXPECT_TRUE(result.ranInRegion(chain.transform));

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::resampling());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
}

namespace {

// Twice the parity source's size and rotated, so the output is wide and tall enough that the whole
// region's largest buffer is several times kStripBufferBytes. The pool allocates every buffer at a
// size class of at least 64 kB, so a region whose largest buffer is below that is never stripped.
const GeometryCase kStripGeometry = { "rotate30-scale2", 0., 0., 30., 2., 2., 0., 0., false };

// Twice the smallest size class: every buffer of a thin strip fits with room to spare, while the
// whole 160-pixel-wide output does not.
const std::uint64_t kStripBufferBytes = 131072;

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

} // namespace

// With a storage buffer range below the whole output's size, the region runs on the GPU as several
// strips, each resampling its own rows from the rotated source region its rows need, and its pixels
// are those of the region run whole.
TEST_F(NativeTransformGpuTest, RotatedStripsMatchTheWholeRegionExactly)
{
    Chain chain;
    chain.source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    chain.first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    chain.transform = makeNode();
    chain.second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(chain.source) && bool(chain.first) && bool(chain.transform) && bool(chain.second));
    connectNodes(chain.source, chain.first, 0, true);
    connectNodes(chain.first, chain.transform, 0, true);
    connectNodes(chain.transform, chain.second, 0, true);
    ASSERT_TRUE(setKnobValues(chain.first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setGeometry(chain.transform, kStripGeometry));
    ASSERT_TRUE(setKnobValues(chain.second, "offset", std::vector<double>(4, 0.25)));
    const std::vector<NodePtr> graph({ chain.first, chain.transform, chain.second });
    const RectI window = pixelRoD(chain.second);
    ASSERT_FALSE(window.isNull());
    const std::uint64_t outputBytes = (std::uint64_t)window.width() * (std::uint64_t)window.height() * 4u * sizeof(float);
    ASSERT_GT(outputBytes, 2 * kStripBufferBytes) << "the output is too small to need strips";

    const EndToEndResult whole = renderEndToEnd(graph, chain.second, window, kTime);
    if (whole.skipped) {
        return;
    }
    ASSERT_TRUE(whole.ok) << whole.error;
    EXPECT_TRUE(whole.ranInRegion(chain.transform));
    EXPECT_EQ(1u, whole.gpuTasksPerNode.count(chain.transform->getScriptName_mt_safe())) << "the region fell back to the CPU";

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
    ASSERT_TRUE(stripped.ok) << stripped.error;
    EXPECT_TRUE(stripped.ranInRegion(chain.transform));
    EXPECT_EQ(1u, stripped.gpuTasksPerNode.count(chain.transform->getScriptName_mt_safe())) << "the stripped region fell back to the CPU";
    const std::vector<int> regionStrips = profiledRegionStrips(profileFile);
    ASSERT_EQ(1u, regionStrips.size()) << "one GPU region expected in the profile";
    EXPECT_GT(regionStrips[0], 1) << "the region did not run as strips";

    ASSERT_EQ(whole.gpu.size(), stripped.gpu.size());
    const ImageDiff diff = compareImages(whole.gpu, stripped.gpu, ParityTolerance::exact());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}

// A Transform reading another one concatenates both matrices on the CPU, which the GPU path cannot,
// so it stays on the CPU and renders as the CPU frame does.
TEST_F(NativeTransformGpuTest, FilterParameterKnobsReachTheCpuAndGpuRenders)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    const bool hasGpu = createRuntime(runtime);
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));

    const auto makeMoved = [&](const NodePtr& input) {
        NodePtr node = makeNode();
        if (node) {
            connectNodes(input, node, 0, true);
            EXPECT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { 3.25, -2.75 }));
            EXPECT_TRUE(setKnobValues(node, kTransformNodeParamScale, { 0.7, 0.7 }));
        }

        return node;
    };
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    NodePtr node = makeMoved(source);
    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(sincLobesChangeTheRender(node, window, kTime));
    EXPECT_TRUE(bicubicParametersChangeTheRender(node, window, kTime));

    // A concatenated chain resamples once, with the last node's filter.
    NodePtr upstream = makeMoved(source);
    ASSERT_TRUE(bool(upstream));
    NodePtr last = makeMoved(upstream);
    ASSERT_TRUE(bool(last));
    EXPECT_TRUE(sincLobesChangeTheRender(last, window, kTime));

    if (!hasGpu) {
        return;
    }
    int nCompared = 0;
    for (double lobes : { 2., 8. }) {
        NodePtr sinc = makeMoved(source);
        ASSERT_TRUE(bool(sinc));
        ASSERT_TRUE(selectFilter(sinc, Resampler::eFilterSinc));
        ASSERT_TRUE(setKnobValues(sinc, kResamplerParamLobes, { lobes }));
        expectGpuMatchesCpu(*runtime, source, NodePtr(), sinc, "sinc " + std::to_string((int)lobes), &nCompared);
        if (HasFatalFailure()) {
            return;
        }
    }
    NodePtr bicubic = makeMoved(source);
    ASSERT_TRUE(bool(bicubic));
    ASSERT_TRUE(selectFilter(bicubic, Resampler::eFilterBicubic));
    ASSERT_TRUE(setKnobValues(bicubic, kResamplerParamBicubicB, { 0. }));
    ASSERT_TRUE(setKnobValues(bicubic, kResamplerParamBicubicC, { 0.5 }));
    expectGpuMatchesCpu(*runtime, source, NodePtr(), bicubic, "bicubic b0 c0.5", &nCompared);
    EXPECT_GT(nCompared, 0);
}

TEST_F(NativeTransformGpuTest, ConcatenatedTransformStaysOnTheCpu)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = makeNode();
    NodePtr second = makeNode();
    ASSERT_TRUE(bool(source) && bool(first) && bool(second));
    connectNodes(source, first, 0, true);
    connectNodes(first, second, 0, true);
    ASSERT_TRUE(setGeometry(first, kGeometries[0]));
    ASSERT_TRUE(setGeometry(second, kGeometries[4]));

    InputMatrixMap transforms;
    second->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    ASSERT_TRUE(bool(transforms[0].newInputEffect));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, second }), second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.ranInRegion(second));

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::exact());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}

#define kFilterSpecOpPluginID "test.natron.built-in.GpuFilterSpecResampleOp"
#define kFilterSpecOpParamTranslate "translate"
#define kFilterSpecOpParamScale "scale"
#define kFilterSpecOpParamPerspective "perspective"
#define kFilterSpecOpParamSalt "salt"

NATRON_NAMESPACE_ENTER

namespace {

// What the test op samples with; it has no filter knobs.
ResampleSettings gFilterSpecSettings;

constexpr InputSpec kFilterSpecInputs[] = {
    InputSpec { .label = "Source" },
};

// A matrix-form Resample op on the GPU: scale about the origin, then translate, with an optional
// perspective row, sampling with whatever gFilterSpecSettings says. At blend amount a the
// translation is scaled by a and the scale moves from 1 towards its value, so a directional blur
// averages matrices along that path.
struct FilterSpecResampleOp {
    struct Params {
        double translateX;
        double translateY;
        double scaleX;
        double scaleY;
        double perspectiveX;
        double perspectiveY;
        double salt;
    };

    static constexpr bool kResamplesOnGpu = true;

    static constexpr NodeSpec kNodeSpec = {
        .id = kFilterSpecOpPluginID,
        .label = "Test GPU Filter Spec Resample Op",
        .grouping = "Other/Test",
        .inputs = kFilterSpecInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    KnobDoubleWPtr translate;
    KnobDoubleWPtr scale;
    KnobDoubleWPtr perspective;
    KnobDoubleWPtr salt;

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
    {
        const struct {
            const char* name;
            int dimensions;
            double fallback;
            KnobDoubleWPtr FilterSpecResampleOp::* member;
        } knobs[] = {
            { kFilterSpecOpParamTranslate, 2, 0., &FilterSpecResampleOp::translate },
            { kFilterSpecOpParamScale, 2, 1., &FilterSpecResampleOp::scale },
            { kFilterSpecOpParamPerspective, 2, 0., &FilterSpecResampleOp::perspective },
            { kFilterSpecOpParamSalt, 1, 0., &FilterSpecResampleOp::salt },
        };
        for (const auto& k : knobs) {
            KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, std::string(k.name), k.dimensions);
            knob->setName(k.name);
            for (int d = 0; d < k.dimensions; ++d) {
                knob->setDefaultValue(k.fallback, d);
            }
            page->addKnob(knob);
            this->*(k.member) = knob;
        }
    }

    Params bake(const KnobReader& reader) const
    {
        Params p;

        p.translateX = reader.valueOr(translate, 0., 0);
        p.translateY = reader.valueOr(translate, 0., 1);
        p.scaleX = reader.valueOr(scale, 1., 0);
        p.scaleY = reader.valueOr(scale, 1., 1);
        p.perspectiveX = reader.valueOr(perspective, 0., 0);
        p.perspectiveY = reader.valueOr(perspective, 0., 1);
        p.salt = reader.valueOr(salt, 0., 0);

        return p;
    }

    bool isIdentity(const Params& /*params*/,
                    const PassContext& /*context*/) const
    {
        return false;
    }

    ResampleSettings settings(const Params& /*params*/,
                              const PassContext& /*context*/) const
    {
        return gFilterSpecSettings;
    }

    bool transform(const Params& params,
                   const PassContext& /*context*/,
                   double amount,
                   bool invert,
                   TransformMath::Mat3* matrix) const
    {
        const double sx = 1. + (params.scaleX - 1.) * amount;
        const double sy = 1. + (params.scaleY - 1.) * amount;
        TransformMath::Mat3 inverse = TransformMath::inverseTransformCanonical(params.translateX * amount, params.translateY * amount, sx, sy, 0., 0., false, 0., 0., 0.);
        inverse.m[6] = params.perspectiveX;
        inverse.m[7] = params.perspectiveY;
        if (!invert) {
            *matrix = inverse;

            return true;
        }

        return inverse.inverse(matrix);
    }
};

static_assert(GpuMatrixResampleOp<FilterSpecResampleOp>);

typedef ImageNode<FilterSpecResampleOp> FilterSpecResampleNode;

void
registerFilterSpecOpOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        EffectInstancePtr node(FilterSpecResampleNode::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;

        functions.insert(std::make_pair("BuildEffect", (void (*)())&FilterSpecResampleNode::BuildEffect));
        LibraryBinary* binary = new LibraryBinary(functions);

        std::list<std::string> grouping;
        node->getPluginGrouping(&grouping);
        QStringList qgrouping;
        for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
            qgrouping.push_back(QString::fromUtf8(it->c_str()));
        }

        Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                           QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
        // Registered after AppManager::load(), so the label-without-suffix the script name is made
        // from was never assigned.
        p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
        p->setOpenGLRenderSupport(node->supportsOpenGLRender());
    });
}

} // namespace

NATRON_NAMESPACE_EXIT

namespace {

struct FilterSpecCase {
    const char* name;
    Resampler::FilterSpec spec;
};

// The seven new filters at their default parameters, Sinc at its two extreme lobe counts and
// Bicubic at three (B, C) pairs besides the default, one with no outer lobe.
const FilterSpecCase kNewFilterSpecs[] = {
    { "lanczos2", Resampler::eFilterLanczos2 },
    { "lanczos3", Resampler::eFilterLanczos3 },
    { "lanczos4", Resampler::eFilterLanczos4 },
    { "sinc", Resampler::eFilterSinc },
    { "blackman-harris", Resampler::eFilterBlackmanHarris },
    { "gaussian", Resampler::eFilterGaussian },
    { "bicubic", Resampler::eFilterBicubic },
    { "sinc-lobes2", Resampler::FilterSpec(Resampler::eFilterSinc, 2) },
    { "sinc-lobes8", Resampler::FilterSpec(Resampler::eFilterSinc, 8) },
    { "bicubic-b0-c05", Resampler::FilterSpec(Resampler::eFilterBicubic, 4, 0., 0.5) },
    { "bicubic-b1-c0", Resampler::FilterSpec(Resampler::eFilterBicubic, 4, 1., 0.) },
    { "bicubic-b0-c0", Resampler::FilterSpec(Resampler::eFilterBicubic, 4, 0., 0.) },
};

struct FilterScaleCase {
    const char* name;
    double translateX;
    double translateY;
    double scaleX;
    double scaleY;
    double perspectiveX;
    double perspectiveY;
};

// 1:1, 2x and 3x up, 4x and 64x down, each on an exact scale and on one that is not; 3x up and
// just above it put the taps on and next to the kernel's zeros. The last is projective: a
// minification whose third row moves the kernel scale across the image, with z staying above 0.9
// over the format.
const FilterScaleCase kFilterScales[] = {
    { "1to1-exact", 2., -3., 1., 1., 0., 0. },
    { "1to1-inexact", 0.3172, -0.2139, 1., 1., 0., 0. },
    { "up2-exact", 0., 0., 2., 2., 0., 0. },
    { "up2-inexact", 0.2713, 0.1371, 2.13, 1.9, 0., 0. },
    { "up3-exact", 0., 0., 3., 3., 0., 0. },
    { "up3-near-boundary", 0.5, 0.25, 3.0004, 2.9996, 0., 0. },
    { "down4-exact", 0., 0., 0.25, 0.25, 0., 0. },
    { "down4-inexact", 0.3172, -0.2139, 0.27, 0.23, 0., 0. },
    { "down64-exact", 0., 0., 1. / 64., 1. / 64., 0., 0. },
    { "down64-inexact", 1.5172, 0.1371, 0.0161, 0.0173, 0., 0. },
    { "projective", 1.25, 0.75, 0.45, 0.4, 0.0008, -0.0006 },
};

} // namespace

class NativeFilterSpecGpuTest
    : public NativeTransformGpuTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerFilterSpecOpOnce();
        NativeTransformGpuTest::SetUp();
    }

    void TearDown() OVERRIDE
    {
        gFilterSpecSettings = ResampleSettings();
        NativeTransformGpuTest::TearDown();
    }

    // A fresh node per setting, as the filter sweeps above do. Its salt differs from every other
    // node's, so no cache keyed on the knob values can serve a render made under another filter.
    NodePtr makeSpecNode(const NodePtr& source,
                         const FilterScaleCase& s)
    {
        static double salt = 0.;
        NodePtr node = createNode(QString::fromUtf8(kFilterSpecOpPluginID));
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(source, node, 0, true);
        salt += 1.;
        EXPECT_TRUE(setKnobValues(node, kFilterSpecOpParamTranslate, { s.translateX, s.translateY }));
        EXPECT_TRUE(setKnobValues(node, kFilterSpecOpParamScale, { s.scaleX, s.scaleY }));
        EXPECT_TRUE(setKnobValues(node, kFilterSpecOpParamPerspective, { s.perspectiveX, s.perspectiveY }));
        EXPECT_TRUE(setKnobValues(node, kFilterSpecOpParamSalt, { salt }));

        return node;
    }
};

// Each new filter, through the FilterSpec the op reports, with and without Clamp and black outside,
// on every scale above, against the CPU render over the same windows and mipmap levels as the
// Transform sweep.
TEST_F(NativeFilterSpecGpuTest, NewFiltersMatchTheCpuRenderAtEveryScale)
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
    for (const FilterSpecCase& f : kNewFilterSpecs) {
        const bool honoursClamp = Resampler::filterHonoursClamp(f.spec.filter);
        for (const FilterScaleCase& s : kFilterScales) {
            for (bool clamp : { false, true }) {
                if (clamp && !honoursClamp) {
                    continue;
                }
                gFilterSpecSettings = ResampleSettings();
                gFilterSpecSettings.filter = f.spec;
                gFilterSpecSettings.clamp = clamp;
                gFilterSpecSettings.blackOutside = true;
                NodePtr node = makeSpecNode(source, s);
                ASSERT_TRUE(bool(node));
                expectGpuMatchesCpu(*runtime, source, NodePtr(), node, std::string(f.name) + " " + s.name + (clamp ? " clamp" : ""), &nCompared);
                if (HasFatalFailure()) {
                    return;
                }
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

// A directional blur of the Sinc at 8 lobes, averaging up to the matrices of a 0.6x minification,
// under the IIR bound and the sampler-stop exceptions of MotionBlurMatchesTheCpuRender.
TEST_F(NativeFilterSpecGpuTest, MotionBlurMatchesTheCpuRenderWithAFilterSpec)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    const FilterScaleCase blurred = { "directional-blur", 6., -4., 0.6, 0.55, 0., 0. };
    gFilterSpecSettings = ResampleSettings();
    gFilterSpecSettings.filter = Resampler::FilterSpec(Resampler::eFilterSinc, 8);
    gFilterSpecSettings.blackOutside = true;
    gFilterSpecSettings.blur.motionBlur = 1.;
    gFilterSpecSettings.blur.directionalBlur = true;
    gFilterSpecSettings.blur.shutter = 1.;
    NodePtr node = makeSpecNode(source, blurred);
    ASSERT_TRUE(bool(node));

    ToleranceExceptions exceptions;
    exceptions.absolute = Resampler::motionBlurMaxError(1.);
    exceptions.pixelsPer = 250;
    exceptions.minPixels = 2;
    int nCompared = 0;
    expectGpuMatchesCpu(*runtime, source, NodePtr(), node, "sinc-lobes8 directional-blur", &nCompared, ParityTolerance::iir(), &exceptions);
    EXPECT_GT(nCompared, 0);
}
