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
#include <cstdio>
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
#include "Engine/GpuCostModel.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/Transform/CornerPin.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderScheduler.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

using TransformMath::Mat3;

namespace {

const double kTime = 1.;

const char* const kToNames[4] = { kCornerPinNodeParamTo1, kCornerPinNodeParamTo2, kCornerPinNodeParamTo3, kCornerPinNodeParamTo4 };
const char* const kFromNames[4] = { kCornerPinNodeParamFrom1, kCornerPinNodeParamFrom2, kCornerPinNodeParamFrom3, kCornerPinNodeParamFrom4 };
const char* const kEnableNames[4] = { kCornerPinNodeParamEnable1, kCornerPinNodeParamEnable2, kCornerPinNodeParamEnable3, kCornerPinNodeParamEnable4 };

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

// One pin of the parity tests on the 64x48 source. The "to" points sit off the pixel grid, so no
// sample position lands exactly on a pixel edge, where the impulse filter's tap is decided by the
// last bit of the CPU's double position. With no point enabled the extra matrix alone applies.
struct PinCase {
    const char* name;
    int enabled;
    double from[4][2];
    double to[4][2];
    bool invert;
    bool projective;
    Mat3 extra;
};

// A mild homography, one that minifies (supersampling levels that change across the output), one
// that magnifies, the first inverted, an affine three-point pin, and a perspective extra matrix
// whose z crosses 0 between pixel centres near x = 50, leaving the right of the output black. Its
// entries are not round decimals: with -0.03 and 1.5, (y + 0.5) / z is an integer at many pixels.
const PinCase kPins[] = {
    { "homography", 4, { { 0., 0. }, { 64., 0. }, { 64., 48. }, { 0., 48. } }, { { 4.13, 2.27 }, { 58.31, 6.19 }, { 62.07, 44.43 }, { 3.29, 40.11 } }, false, true, Mat3::identity() },
    { "homography-minify", 4, { { 0., 0. }, { 64., 0. }, { 64., 48. }, { 0., 48. } }, { { 20.17, 14.29 }, { 41.33, 16.07 }, { 38.41, 31.13 }, { 23.71, 28.37 } }, false, true, Mat3::identity() },
    { "homography-magnify", 4, { { 16., 12. }, { 48., 10. }, { 50., 36. }, { 14., 38. } }, { { 0.17, 0.23 }, { 64.31, 0.11 }, { 63.73, 48.29 }, { 0.41, 47.87 } }, false, true, Mat3::identity() },
    { "homography-invert", 4, { { 0., 0. }, { 64., 0. }, { 64., 48. }, { 0., 48. } }, { { 4.13, 2.27 }, { 58.31, 6.19 }, { 62.07, 44.43 }, { 3.29, 40.11 } }, true, true, Mat3::identity() },
    { "affine-3", 3, { { 0., 0. }, { 64., 0. }, { 64., 48. }, { 0., 0. } }, { { 3.17, -2.29 }, { 60.41, 5.13 }, { 57.23, 46.37 }, { 0., 0. } }, false, false, Mat3::identity() },
    { "behind-camera", 0, {}, {}, false, true, Mat3(1., 0., 0., 0., 1., 0., -0.03007, 0., 1.50131) },
};

bool
setPin(const NodePtr& node,
       const PinCase& pin)
{
    for (int i = 0; i < 4; ++i) {
        const bool on = i < pin.enabled;
        if (!setKnobValues(node, kEnableNames[i], { on ? 1. : 0. })) {
            return false;
        }
        if (on && (!setKnobValues(node, kFromNames[i], { pin.from[i][0], pin.from[i][1] }) || !setKnobValues(node, kToNames[i], { pin.to[i][0], pin.to[i][1] }))) {
            return false;
        }
    }

    return setKnobValues(node, kCornerPinNodeParamExtraMatrix, std::vector<double>(pin.extra.m, pin.extra.m + 9)) && setKnobValues(node, kCornerPinNodeParamInvert, { pin.invert ? 1. : 0. });
}

CornerPinNode*
cornerPinOf(const NodePtr& node)
{
    return node ? dynamic_cast<CornerPinNode*>(node->getEffectInstance().get()) : NULL;
}

StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
}

// Whether node samples through a matrix with a perspective row at kTime.
bool
samplesProjectively(const NodePtr& node)
{
    Mat3 m;
    CornerPinNode* pin = cornerPinOf(node);
    EXPECT_TRUE(pin && pin->getInverseMatrix(kTime, ViewIdx(0), &m));

    return (m(2, 0) != 0.) || (m(2, 1) != 0.) || (m(2, 2) != 1.);
}

} // namespace

class NativeCornerPinGpuTest
    : public BaseTest {
protected:
    // A 64x48 project format, the parity source's size.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeCornerPinGpu64x48", 1.));
    }

    // A CornerPin reading source, its points set after the connection reset them.
    NodePtr makePin(const NodePtr& source,
                    const PinCase& pin)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CORNERPIN), PLUGIN_MAJOR_NATRON_CORNERPIN);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, 0, true);
        }
        EXPECT_TRUE(setPin(node, pin)) << pin.name;

        return node;
    }

    NodePtr makeTransform(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(source, node, 0, true);
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamCenter, { 32., 24. }));
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamRotate, { 12. }));
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamScale, { 0.8, 0.9 }));
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { 2.37, -1.41 }));

        return node;
    }

    // Keys the "to" points of the first pin at times 0 and 2, about its values, so the homography
    // changes over any shutter around kTime.
    static bool animate(const NodePtr& node)
    {
        const PinCase& pin = kPins[0];
        const double shift[4][2] = { { -5., 3. }, { 4., -2. }, { 6., 3. }, { -3., -4. } };
        for (int i = 0; i < 4; ++i) {
            KnobDouble* knob = dynamic_cast<KnobDouble*>(node->getKnobByName(kToNames[i]).get());
            EXPECT_TRUE(knob != NULL) << kToNames[i];
            if (!knob) {
                return false;
            }
            for (int d = 0; d < 2; ++d) {
                knob->setValueAtTime(0., pin.to[i][d] - shift[i][d], ViewSpec::all(), d);
                knob->setValueAtTime(2., pin.to[i][d] + shift[i][d], ViewSpec::all(), d);
            }
        }

        return true;
    }
};

TEST_F(NativeCornerPinGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    for (const PinCase& pin : kPins) {
        NodePtr node = makePin(source, pin);
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        EXPECT_EQ(pin.projective, samplesProjectively(node)) << pin.name;

        for (int nComps : { 1, 2, 3, 4 }) {
            GpuQuery q = eligibleQuery();
            q.nComps = nComps;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << pin.name << " " << nComps << " components";
        }
        for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
            GpuQuery q = eligibleQuery();
            q.bitDepth = depth;
            EXPECT_FALSE(effect->canRenderOnGpu(q)) << pin.name << " bit depth " << (int)depth;
        }
        GpuQuery q = eligibleQuery();
        q.draftMode = true;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << pin.name << " draft";
        q.draftMode = false;
        q.hasConcatenatedInput = true;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << pin.name << " a transform concatenated from upstream";
        q.hasConcatenatedInput = false;
        q.hasMask = true;
        q.mix = 0.5;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << pin.name << " mask and mix";
    }

    NodePtr node = makePin(source, kPins[0]);
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(animate(node));
    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamMotionBlur, { 1. }));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "motion blur";
    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamShutter, { 0. }));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "motion blur with a closed shutter";
}

// The cost model prices CornerPin with its motion blur rows only while the knobs ask for motion blur.
TEST_F(NativeCornerPinGpuTest, CostTraitsFollowMotionBlur)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    NodePtr node = makePin(source, kPins[0]);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(animate(node));

    GpuCostModel model;
    FrameGraph::Task task;
    task.key.node = node;
    task.key.time = kTime;
    task.key.view = ViewIdx(0);
    task.key.mipmapLevel = 0;
    EXPECT_EQ(std::string(PLUGINID_NATRON_CORNERPIN), model.traitsOf(task).pluginId);
    EXPECT_FALSE(model.traitsOf(task).motionBlur);
    EXPECT_EQ(0., model.traitsOf(task).sigma);
    EXPECT_TRUE(model.hasGpuCost(model.traitsOf(task)));
    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamMotionBlur, { 1. }));
    EXPECT_TRUE(model.traitsOf(task).motionBlur);
    EXPECT_TRUE(model.hasGpuCost(model.traitsOf(task)));
    ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamShutter, { 0. }));
    EXPECT_FALSE(model.traitsOf(task).motionBlur);
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
    StagedImageEffect* effect = effectOf(node);
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

// The source over getGpuInputRoI(roi) within its region of definition, inside a larger buffer at
// an offset and with padded rows. The GPU render reads nothing of the source outside both.
bool
sourceImage(const NodePtr& source,
            StagedImageEffect& effect,
            unsigned mipmapLevel,
            const RectI& roi,
            GpuParityImage* image)
{
    const RectI inputRoI = effect.getGpuInputRoI(0, roi, RenderScale::fromMipmapLevel(mipmapLevel)).intersect(pixelRegionOfDefinition(source, mipmapLevel));
    if (inputRoI.isNull()) {
        // A window that reads nothing of the source: an empty image stands for none.
        image->bounds = RectI();
        image->pixels.clear();

        return true;
    }
    RenderedPlane in;
    if (!renderColor(source, mipmapLevel, inputRoI, &in)) {
        return false;
    }
    EXPECT_EQ(inputRoI, in.window);
    image->bounds = in.window;
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

// The region of definition within the format grown by 8 pixels (a pin reaching behind the camera
// has an unbounded one), and an odd-sized window inside it.
std::vector<RectI>
testWindows(const RectI& rod,
            unsigned mipmapLevel)
{
    std::vector<RectI> rois;
    const RectI grown = RectD(-8., -8., kParitySourceWidth + 8., kParitySourceHeight + 8.).toPixelEnclosing(mipmapLevel, 1.);
    const RectI whole = rod.intersect(grown);
    if (whole.isNull()) {
        return rois;
    }
    rois.push_back(whole);
    const int x2 = std::min(whole.x2 - 2, whole.x1 + 5 + 23);
    const int y2 = std::min(whole.y2 - 1, whole.y1 + 3 + 17);
    if ((x2 > whole.x1 + 5) && (y2 > whole.y1 + 3)) {
        rois.push_back(RectI(whole.x1 + 5, whole.y1 + 3, x2, y2));
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
// expecting the tolerance; `mask`, when given, is the mask input's source. With `exceptions`, a
// few pixels may miss the tolerance as those allow.
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
    StagedImageEffect* effect = effectOf(node);
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

        const std::vector<RectI> rois = testWindows(rod, mipmapLevel);
        ASSERT_FALSE(rois.empty()) << level;
        for (const RectI& roi : rois) {
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
// on and off, under each pin, against the CPU render fed with the source over getGpuInputRoI(), on
// the parity source moved off the format's origin. One test per filter, as for Transform.
class NativeCornerPinGpuFilterTest
    : public NativeCornerPinGpuTest,
      public ::testing::WithParamInterface<int> {
};

TEST_P(NativeCornerPinGpuFilterTest, ResamplingMatchesTheCpuRender)
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
    ASSERT_EQ((std::size_t)Resampler::eFilterNotch + 1, filters.size());
    const std::size_t f = (std::size_t)GetParam();
    const bool honoursClamp = Resampler::filterHonoursClamp((Resampler::FilterEnum)f);
    int nCompared = 0;
    for (const PinCase& pin : kPins) {
        for (bool clamp : { false, true }) {
            if (clamp && !honoursClamp) {
                continue;
            }
            for (bool blackOutside : { false, true }) {
                // A fresh node per setting, so no region of definition cached under an earlier
                // setting is reused.
                NodePtr node = makePin(source, pin);
                ASSERT_TRUE(bool(node));
                ASSERT_EQ(pin.projective, samplesProjectively(node)) << pin.name;
                ASSERT_TRUE(setKnobValue(node, kCornerPinNodeParamFilter, std::string(filters[f].id)));
                ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamClamp, { clamp ? 1. : 0. }));
                ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamBlackOutside, { blackOutside ? 1. : 0. }));
                std::ostringstream base;
                base << filters[f].id << " " << pin.name << (clamp ? " clamp" : "") << (blackOutside ? " black-outside" : "");
                expectGpuMatchesCpu(*runtime, source, NodePtr(), node, base.str(), &nCompared);
                if (HasFatalFailure()) {
                    return;
                }
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

INSTANTIATE_TEST_CASE_P(Filters, NativeCornerPinGpuFilterTest, ::testing::Range(0, (int)Resampler::eFilterNotch + 1));

// The homography with the mask enabled at mix 0.5 and 1, inverted, and mix 0.5 without a mask.
TEST_F(NativeCornerPinGpuTest, MaskedMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    const struct {
        const char* name;
        bool mask;
        bool invert;
        double mix;
    } cases[] = {
        { "mask-mix05", true, false, 0.5 },
        { "inverted-mask-mix05", true, true, 0.5 },
        { "mask", true, false, 1. },
        { "mix05", false, false, 0.5 },
    };
    int nCompared = 0;
    for (const auto& c : cases) {
        NodePtr maskSource = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        ASSERT_TRUE(bool(maskSource));
        setParitySourceOrigin(maskSource, 8, 4);
        NodePtr node = makePin(source, kPins[0]);
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        const int maskInput = effect->getMaskInput();
        ASSERT_GE(maskInput, 0);
        connectNodes(maskSource, node, maskInput, true);
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
        ASSERT_TRUE(maskChannel != NULL);
        maskChannel->set("rgba.A");
        ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { c.mask ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMaskInvertParamName, { c.invert ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { c.mix }));
        ASSERT_TRUE(setKnobValue(node, kCornerPinNodeParamFilter, std::string("cubic")));

        expectGpuMatchesCpu(*runtime, source, c.mask ? maskSource : NodePtr(), node, c.name, &nCompared);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_GT(nCompared, 0);
}

// An animated homography under motion blur, every matrix of which is projective, against the CPU
// render under the IIR bound, with Transform's allowance for pixels whose variance stop test lands
// one sample apart (see NativeTransformGpu_Test).
TEST_F(NativeCornerPinGpuTest, MotionBlurMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    const struct {
        const char* name;
        double motionBlur;
        const char* shutterOffset;
        const char* filter;
        bool blackOutside;
        bool stallsOnLavapipe;
    } cases[] = {
        { "motion-blur-05-start", 0.5, "start", "bilinear", false, false },
        { "motion-blur-1-centered", 1., "centered", "cubic", true, false },
        { "motion-blur-2-end", 2., "end", "keys", false, true },
    };
    const bool cpuDevice = (runtime->runtime().device().info().type == VK_PHYSICAL_DEVICE_TYPE_CPU);
    int nCompared = 0;
    for (const auto& c : cases) {
        if (c.stallsOnLavapipe && cpuDevice) {
            std::printf("skipped %s: lavapipe stalls on this dispatch on some CPUs; the parity on RADV covers it\n", c.name);
            continue;
        }
        NodePtr node = makePin(source, kPins[0]);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(animate(node));
        ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamMotionBlur, { c.motionBlur }));
        ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamShutter, { 1. }));
        ASSERT_TRUE(setKnobValue(node, kCornerPinNodeParamShutterOffset, std::string(c.shutterOffset)));
        ASSERT_TRUE(setKnobValue(node, kCornerPinNodeParamFilter, std::string(c.filter)));
        ASSERT_TRUE(setKnobValues(node, kCornerPinNodeParamBlackOutside, { c.blackOutside ? 1. : 0. }));
        ASSERT_TRUE(samplesProjectively(node)) << c.name;

        ToleranceExceptions exceptions;
        exceptions.absolute = Resampler::motionBlurMaxError(c.motionBlur);
        exceptions.pixelsPer = 250;
        exceptions.minPixels = 2;
        expectGpuMatchesCpu(*runtime, source, NodePtr(), node, c.name, &nCompared, ParityTolerance::iir(), &exceptions);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_GT(nCompared, 0);
}

#endif // NATRON_HAVE_GPU

namespace {

// Expects the GPU frame of root to match the CPU frame within the resampling bound.
void
expectFramesMatch(const EndToEndResult& result,
                  const char* what)
{
    ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::resampling());
    EXPECT_EQ(0u, diff.mismatches) << what << ": worst " << diff.worstAbsolute << ", first at float " << diff.firstMismatchIndex;
}

} // namespace

// A homography alone renders on the GPU through the scheduler, as on the CPU.
TEST_F(NativeCornerPinGpuTest, HomographyFrameRunsOnTheGpu)
{
    resetProject();
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source));
    NodePtr pin = makePin(source, kPins[0]);
    ASSERT_TRUE(bool(pin));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ pin }), pin, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.ranInRegion(pin));
    expectFramesMatch(result, "CornerPin");
}

// A Transform after a CornerPin concatenates the pin's projective matrix and resamples once on the
// CPU, the GPU path taking no concatenated source; the frame matches the CPU one.
TEST_F(NativeCornerPinGpuTest, ConcatenatedIntoATransformMatchesTheCpuFrame)
{
    resetProject();
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source));
    NodePtr pin = makePin(source, kPins[0]);
    NodePtr transform = makeTransform(pin);
    ASSERT_TRUE(pin && transform);

    InputMatrixMap transforms;
    transform->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    ASSERT_TRUE(bool(transforms[0].newInputEffect));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ pin, transform }), transform, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.ranInRegion(transform));
    expectFramesMatch(result, "CornerPin -> Transform");
}

// A CornerPin after a Transform applies the concatenated upstream matrix on the CPU; the frame
// matches the CPU one.
TEST_F(NativeCornerPinGpuTest, ConcatenatingATransformMatchesTheCpuFrame)
{
    resetProject();
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source));
    NodePtr transform = makeTransform(source);
    NodePtr pin = makePin(transform, kPins[0]);
    ASSERT_TRUE(pin && transform);

    InputMatrixMap transforms;
    pin->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    ASSERT_TRUE(bool(transforms[0].newInputEffect));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ transform, pin }), pin, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.ranInRegion(pin));
    expectFramesMatch(result, "Transform -> CornerPin");
}
