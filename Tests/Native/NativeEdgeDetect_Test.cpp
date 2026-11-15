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
#include <functional>
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/BlurKernels.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kEdgeDetectID = PLUGINID_NATRON_EDGEDETECT;
const int kNativeEdgeDetectMajor = PLUGIN_MAJOR_NATRON_EDGEDETECT;
const double kTime = 1.;

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    select->set(value);

    return true;
}

bool
processRGBA(const NodePtr& node)
{
    KnobChannelSetPtr channels = node ? std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet)) : KnobChannelSetPtr();

    EXPECT_TRUE(bool(channels));
    if (!channels) {
        return false;
    }
    channels->setChannels(0, { "R", "G", "B", "A" });

    return true;
}

RectD
regionOfDefinition(const NodePtr& node,
                   double time,
                   const RenderScale& scale = RenderScale::identity)
{
    RectD rod;
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

    EXPECT_TRUE(bool(effect));
    if (!effect) {
        return rod;
    }
    // Called directly rather than through the cached action: a knob edit does not always change
    // the render hash.
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), time, scale, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat) << node->getPluginID();

    return rod;
}

} // namespace

class NativeEdgeDetectTest
    : public BaseTest {
protected:
    // A fresh project whose default format is the parity source's 64x48, so that cropToFormat
    // keeps the expanded region of definition, and the recorded references, at the source's size.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeEdgeDetect64x48", 1.));
    }

    // Renders `node` in Legacy mode and as a task graph at pool sizes 1 and 4 over its own region
    // of definition: bit-exact, and the task graph pulls nothing it did not plan.
    void expectSameBothWays(const NodePtr& node,
                            const std::string& caseName)
    {
        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            std::vector<int> unplannedPulls;
            const RectI window = regionOfDefinition(node, kTime, RenderScale::fromMipmapLevel(mipmapLevel)).toPixelEnclosing(mipmapLevel, 1.);
            const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
            EXPECT_FALSE(m.any) << caseName << ", mipmap " << mipmapLevel << ": " << describe(m);
            EXPECT_EQ(poolSizes.size(), unplannedPulls.size()) << caseName;
            for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
                EXPECT_EQ(0, unplannedPulls[p]) << caseName << ", mipmap " << mipmapLevel << ", pool " << poolSizes[p];
            }
        }
    }
};

TEST_F(NativeEdgeDetectTest, UnversionedRequestsGetTheNativeEdgeDetect)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kEdgeDetectID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeEdgeDetectMajor, unversioned->getMajorVersion());
}

TEST_F(NativeEdgeDetectTest, InputsAndDefaultChannels)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kEdgeDetectID), kNativeEdgeDetectMajor);
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_FALSE(bool(node->getKnobByName("hostUnPremultBy")));

    // R, G and B on and A off, on the colour layer only.
    bool r = false;
    bool g = false;
    bool b = false;
    bool a = true;
    EXPECT_TRUE(node->getEffectInstance()->isHostChannelSelectorSupported(&r, &g, &b, &a));
    EXPECT_TRUE(r);
    EXPECT_TRUE(g);
    EXPECT_TRUE(b);
    EXPECT_FALSE(a);
    EXPECT_FALSE(node->getEffectInstance()->defaultProcessesAllLayers());
}

TEST_F(NativeEdgeDetectTest, SourceRegionOfInterestAddsErosionAndSuppression)
{
    EdgeDetectParams params;
    const RectI rect(10, 20, 30, 40);

    // Gaussian with blur size 0: max(3, 0) plus the derivative order.
    params.filter = eEdgeDetectFilterGaussian;
    RectI roi = EdgeDetect::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(10 - 4, roi.x1);
    EXPECT_EQ(40 + 4, roi.y2);

    // The finite-difference schemes use the Gaussian pre-blur's halo, at least the recursion's
    // reach, which is longer than CImg's 1.5 times the size.
    params.filter = eEdgeDetectFilterSobel;
    params.sizeX = params.sizeY = 10.;
    const float sigma = (float)(10. / 2.4);
    const int preBlurReach = BlurKernels::LineFilter::vanVliet(sigma, 0, EdgeMode::Black).recursiveReach();
    EXPECT_GT(preBlurReach, 15);
    roi = EdgeDetect::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(10 - preBlurReach - 1, roi.x1);
    EXPECT_EQ(30 + preBlurReach + 1, roi.x2);

    // Quadratic: 3 * (floor((10 - 1) / 2) + 1) = 15, plus one for the derivative.
    params.filter = eEdgeDetectFilterQuadratic;
    roi = EdgeDetect::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(10 - 15 - 1, roi.x1);

    // Erosion radius 2 on each axis and one more pixel for non-maxima suppression.
    params.filter = eEdgeDetectFilterGaussian;
    params.erodeSize = -2.;
    params.nms = true;
    const int derivativeReach = std::max(BlurKernels::LineFilter::vanVliet(sigma, 0, EdgeMode::Black).recursiveReach(),
                                         BlurKernels::LineFilter::vanVliet(sigma, 1, EdgeMode::Black).recursiveReach());
    EXPECT_GT(derivativeReach, 15 + 1);
    roi = EdgeDetect::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(10 - derivativeReach - 2 - 1, roi.x1);
    EXPECT_EQ(40 + derivativeReach + 2 + 1, roi.y2);

    // At half scale the erosion radius is floor(2 * 0.5) = 1.
    int rx = 0;
    int ry = 0;
    EdgeDetect::getErodeRadius(RenderScale::fromMipmapLevel(1), params, &rx, &ry);
    EXPECT_EQ(1, rx);
    EXPECT_EQ(1, ry);

    // The x radius follows the pixel aspect ratio.
    params.par = 2.;
    params.erodeSize = 3.;
    EdgeDetect::getErodeRadius(RenderScale::identity, params, &rx, &ry);
    EXPECT_EQ(1, rx);
    EXPECT_EQ(3, ry);
}

// A unit step edge, blurred by an operator with a known step response, gives a gradient magnitude
// of 1 at the edge whatever the blur size. Only the red channel is processed, so every
// multi-channel combination is the magnitude of that one gradient.
TEST_F(NativeEdgeDetectTest, UnitStepEdgeHasUnitGradientMagnitude)
{
    resetProject();
    const RectI format(0, 0, kParitySourceWidth, kParitySourceHeight);
    const RectI whiteBox(kParitySourceWidth / 2, 0, kParitySourceWidth, kParitySourceHeight);
    NodePtr black = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr white = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
    NodePtr edge = createNode(QString::fromUtf8(kEdgeDetectID), kNativeEdgeDetectMajor);
    ASSERT_TRUE(bool(black) && bool(white) && bool(merge));
    ASSERT_TRUE(isNative(edge));
    ASSERT_TRUE(setKnobValue(black, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
    ASSERT_TRUE(setKnobValues(black, kConstantParamColor, { 0., 0., 0., 0. }));
    ASSERT_TRUE(setKnobValue(white, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
    ASSERT_TRUE(setKnobValues(white, kNativeGeneratorParamBottomLeft, { (double)whiteBox.x1, (double)whiteBox.y1 }));
    ASSERT_TRUE(setKnobValues(white, kNativeGeneratorParamSize, { (double)whiteBox.width(), (double)whiteBox.height() }));
    ASSERT_TRUE(setKnobValues(white, kConstantParamColor, { 1., 1., 1., 1. }));
    setBlackOutside(white, true);
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, std::string("over")));
    connectNodes(black, merge, 0, true);
    connectNodes(white, merge, 1, true);
    connectNodes(merge, edge, 0, true);

    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(edge->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    channels->setChannels(0, { "R" });

    struct Operator {
        const char* name;
        double tolerance;
    };
    // The Gaussian-based operators are held to 3%: the step lies between two pixels, so the
    // sampled peak sits half a pixel off the continuous one. The box and triangle normalisations
    // are exact for CImg's discrete box sums followed by a centred difference; the quadratic one
    // is the OpenFX plug-in's approximation, 0.6% off at size 12 and 1.5% at worst from size 7 up.
    const Operator operators[] = {
        { kEdgeDetectParamFilterGaussian, 0.03 },
        { kEdgeDetectParamFilterQuasiGaussian, 0.03 },
        { kEdgeDetectParamFilterSimple, 0.03 },
        { kEdgeDetectParamFilterSobel, 0.03 },
        { kEdgeDetectParamFilterRotationInvariant, 0.03 },
        { kEdgeDetectParamFilterBox, 1e-3 },
        { kEdgeDetectParamFilterTriangle, 1e-3 },
        { kEdgeDetectParamFilterQuadratic, 0.01 },
    };
    const char* const combinations[] = {
        kEdgeDetectParamMultiChannelSeparate,
        kEdgeDetectParamMultiChannelRMS,
        kEdgeDetectParamMultiChannelMax,
        kEdgeDetectParamMultiChannelTensor,
    };
    ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamCropToFormat, { 0. }));
    const RectI window = regionOfDefinition(edge, kTime).toPixelEnclosing(0, 1.);
    ASSERT_GE(window.width(), format.width());
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    for (const Operator& op : operators) {
        ASSERT_TRUE(setKnobValue(edge, kEdgeDetectParamFilter, std::string(op.name)));
        for (const double blurSize : { 12., 24. }) {
            ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamBlurSize, { blurSize }));
            for (const char* combination : combinations) {
                ASSERT_TRUE(setKnobValue(edge, kEdgeDetectParamMultiChannel, std::string(combination)));
                std::vector<RenderedPlane> planes;
                std::string error;
                ASSERT_TRUE(renderNodePlanesDirect(edge, kTime, ViewIdx(0), 0, window, rgba, &planes, &error)) << error;
                ASSERT_EQ(1u, planes.size());
                ASSERT_EQ(4u, planes[0].channels.size());
                const int midRow = format.height() / 2;
                float peak = 0.f;
                for (int x = format.x1; x < format.x2; ++x) {
                    const std::size_t at = ((std::size_t)(midRow - window.y1) * window.width() + (std::size_t)(x - window.x1)) * 4;
                    peak = std::max(peak, planes[0].pixels[at]);
                }
                EXPECT_NEAR(1., peak, op.tolerance) << op.name << ", blur size " << blurSize << ", " << combination;
            }
        }
    }
}

// A constant image has no edge inside it. Where the node repeats the source's edge (Stretch, and
// Input over a generator, which reports Stretch) its border shows none either; where it reads black
// past the source the border is an edge.
TEST_F(NativeEdgeDetectTest, EdgeModeDecidesTheBorderResponseOfAConstant)
{
    const RectI format(0, 0, kParitySourceWidth, kParitySourceHeight);
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    const char* const filters[] = { kEdgeDetectParamFilterSimple, kEdgeDetectParamFilterSobel, kEdgeDetectParamFilterGaussian, kEdgeDetectParamFilterBox };

    for (const char* filter : filters) {
        for (const double blurSize : { 0., 6. }) {
            for (const char* mode : { "input", "stretch", "black" }) {
                resetProject();
                NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
                NodePtr edge = createNode(QString::fromUtf8(kEdgeDetectID), kNativeEdgeDetectMajor);
                ASSERT_TRUE(bool(constant));
                ASSERT_TRUE(isNative(edge));
                ASSERT_TRUE(setKnobValue(constant, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
                ASSERT_TRUE(setKnobValues(constant, kConstantParamColor, { 0.5, 0.5, 0.5, 0.5 }));
                connectNodes(constant, edge, 0, true);
                ASSERT_TRUE(processRGBA(edge));
                ASSERT_TRUE(setKnobValue(edge, kEdgeDetectParamFilter, std::string(filter)));
                ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamBlurSize, { blurSize }));
                ASSERT_TRUE(setKnobValue(edge, kEdgeModeParam, std::string(mode)));

                const std::string what = std::string(filter) + ", blur size " + std::to_string(blurSize) + ", edge mode " + mode;
                std::vector<RenderedPlane> planes;
                std::string error;
                ASSERT_TRUE(renderNodePlanesDirect(edge, kTime, ViewIdx(0), 0, format, rgba, &planes, &error)) << what << ": " << error;
                ASSERT_EQ(1u, planes.size()) << what;
                ASSERT_EQ(static_cast<std::size_t>(format.width()) * format.height() * 4, planes[0].pixels.size()) << what;
                float border = 0.f;
                float centre = 0.f;
                for (int y = format.y1; y < format.y2; ++y) {
                    for (int x = format.x1; x < format.x2; ++x) {
                        const bool onBorder = (x == format.x1) || (y == format.y1) || (x == format.x2 - 1) || (y == format.y2 - 1);
                        const bool inCentre = (x >= format.x1 + 16) && (x < format.x2 - 16) && (y >= format.y1 + 16) && (y < format.y2 - 16);
                        const std::size_t at = ((std::size_t)(y - format.y1) * format.width() + (std::size_t)(x - format.x1)) * 4;
                        const float v = std::abs(planes[0].pixels[at]);
                        if (onBorder) {
                            border = std::max(border, v);
                        }
                        if (inCentre) {
                            centre = std::max(centre, v);
                        }
                    }
                }
                // The black border's response reaches the centre through the blur's tails.
                const float centreLimit = (std::string(mode) == "black") ? 2e-3f : 1e-4f;
                EXPECT_LE(centre, centreLimit) << what << ": the inside of a constant has no edge";
                if (std::string(mode) == "black") {
                    EXPECT_GT(border, 0.05f) << what << ": the black past the source makes an edge";
                } else {
                    EXPECT_LE(border, 1e-4f) << what << ": the repeated edge makes none";
                }
            }
        }
    }
}

// The filter runs over the window plus its halo whatever the mask, so the masked node is its
// unmasked render blended with the source by mask x mix. The OpenFX plug-in shrank the filtered
// window past mask-zero rows and columns, which moves the recursive filters' lines, so this is
// checked against the node itself rather than a recorded reference.
TEST_F(NativeEdgeDetectTest, MaskAndMixBlendTheUnmaskedRender)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr plain = createNode(QString::fromUtf8(kEdgeDetectID), kNativeEdgeDetectMajor);
    NodePtr masked = createNode(QString::fromUtf8(kEdgeDetectID), kNativeEdgeDetectMajor);
    ASSERT_TRUE(bool(source) && bool(mask));
    ASSERT_TRUE(isNative(plain) && isNative(masked));
    setParitySourceOrigin(source, 0, 0);
    setParitySourceOrigin(mask, 8, 4);
    setBlackOutside(mask, true);
    connectNodes(source, plain, 0, true);
    connectNodes(source, masked, 0, true);
    connectNodes(mask, masked, 1, true);
    for (const NodePtr& node : { plain, masked }) {
        ASSERT_TRUE(processRGBA(node));
        ASSERT_TRUE(setKnobValues(node, kEdgeDetectParamBlurSize, { 3. }));
    }
    ASSERT_TRUE(setKnobValues(masked, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(masked, "maskChannel_Mask", "rgba.A"));
    const float mix = 0.5f;
    ASSERT_TRUE(setKnobValues(masked, kOfxMixParamName, { (double)mix }));

    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    const ParityTolerance tolerance = ParityTolerance::arithmetic();
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
        std::vector<RenderedPlane> planes[4];
        const NodePtr nodes[4] = { source, mask, plain, masked };
        for (int n = 0; n < 4; ++n) {
            std::string error;
            ASSERT_TRUE(renderNodePlanesDirect(nodes[n], kTime, ViewIdx(0), mipmapLevel, window, rgba, &planes[n], &error)) << error;
            ASSERT_EQ(1u, planes[n].size());
            ASSERT_EQ(4u, planes[n][0].channels.size());
            ASSERT_EQ((std::size_t)window.area() * 4, planes[n][0].pixels.size());
        }
        const std::vector<float>& src = planes[0][0].pixels;
        const std::vector<float>& maskPixels = planes[1][0].pixels;
        const std::vector<float>& filtered = planes[2][0].pixels;
        const std::vector<float>& got = planes[3][0].pixels;
        std::size_t mismatches = 0;
        std::size_t partlyMasked = 0;
        for (std::size_t i = 0; i < got.size(); i += 4) {
            const float alpha = maskPixels[i + 3] * mix;
            partlyMasked += ((alpha > 0.f) && (alpha < mix)) ? 1 : 0;
            for (int c = 0; c < 4; ++c) {
                const float want = (alpha == 0.f) ? src[i + c] : filtered[i + c] * alpha + (1.f - alpha) * src[i + c];
                if (!tolerance.accepts(want, got[i + c])) {
                    if (mismatches == 0) {
                        const std::size_t at = i / 4;
                        ADD_FAILURE() << "mipmap " << mipmapLevel << ": first at (" << (window.x1 + (int)(at % window.width())) << "," << (window.y1 + (int)(at / window.width())) << ") channel " << c
                                      << " got " << got[i + c] << " want " << want;
                    }
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(0u, mismatches) << "mipmap " << mipmapLevel;
        EXPECT_GT(partlyMasked, 0u) << "mipmap " << mipmapLevel << ": the mask has no fractional values";
    }
}

TEST_F(NativeEdgeDetectTest, RendersTheSameInBothSchedulerModesAtAnyPoolSize)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr edge = createNode(QString::fromUtf8(kEdgeDetectID), kNativeEdgeDetectMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(edge));
    connectNodes(source, edge, 0, true);
    ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamBlurSize, { 5. }));
    ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamCropToFormat, { 0. }));
    expectSameBothWays(edge, "gaussian-tensor-5");

    ASSERT_TRUE(setKnobValue(edge, kEdgeDetectParamFilter, std::string(kEdgeDetectParamFilterSobel)));
    ASSERT_TRUE(setKnobValue(edge, kEdgeDetectParamMultiChannel, std::string(kEdgeDetectParamMultiChannelRMS)));
    ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamErodeSize, { 2. }));
    ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamNMS, { 1. }));
    expectSameBothWays(edge, "sobel-rms-erode2-nms");

    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(mask));
    setParitySourceOrigin(mask, 8, 4);
    connectNodes(mask, edge, 1, true);
    ASSERT_TRUE(setKnobValues(edge, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(edge, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobValues(edge, kOfxMixParamName, { 0.5 }));
    expectSameBothWays(edge, "sobel-rms-mask-mix");
}

TEST_F(NativeEdgeDetectTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_EDGEDETECT, PLUGIN_MAJOR_NATRON_EDGEDETECT, "EdgeDetect");
}

// Under Stretch the passes hold the source grown by the reach of the passes before the last, and
// read past it by repeating its edge: the result matches the same filters under Black over a
// source whose edge is laid out explicitly past every pixel they reach, and a blur far larger
// than the image stays finite with a tile matching the whole.
TEST_F(NativeEdgeDetectTest, StretchFarPastTheImageMatchesTheLaidOutEdge)
{
    const RectI format(0, 0, kParitySourceWidth, kParitySourceHeight);
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    const int pad = 300;
    const RectI padded(format.x1 - pad, format.y1 - pad, format.x2 + pad, format.y2 + pad);
    const auto makeConstant = [&](const RectI& extent, const std::vector<double>& color, bool blackOutside) -> NodePtr {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        EXPECT_TRUE(bool(constant));
        if (constant) {
            EXPECT_TRUE(setKnobValue(constant, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamBottomLeft, { (double)extent.x1, (double)extent.y1 }));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamSize, { (double)extent.width(), (double)extent.height() }));
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, color));
            setBlackOutside(constant, blackOutside);
        }

        return constant;
    };
    const auto makeOver = [&](const NodePtr& b, const NodePtr& a) -> NodePtr {
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
        EXPECT_TRUE(bool(merge) && bool(a) && bool(b));
        if (!merge || !a || !b) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, std::string("over")));
        connectNodes(b, merge, 0, true);
        connectNodes(a, merge, 1, true);

        return merge;
    };
    const auto makeSource = [&](const RectI& left, const RectI& right) -> NodePtr {
        NodePtr halves = makeOver(makeConstant(left, { 0.2, 0.4, 0.6, 1. }, false), makeConstant(right, { 0.9, 0.1, 0.3, 1. }, true));

        return makeOver(halves, makeConstant(RectI(20, 14, 44, 34), { 1., 0.5, 0.25, 1. }, true));
    };
    const auto makeEdge = [&](const NodePtr& source, const char* filter, double blurSize, double erodeSize, bool nms, const char* mode) -> NodePtr {
        NodePtr edge = createNode(QString::fromUtf8(kEdgeDetectID), kNativeEdgeDetectMajor);
        EXPECT_TRUE(isNative(edge) && bool(source));
        if (!edge || !source) {
            return NodePtr();
        }
        connectNodes(source, edge, 0, true);
        EXPECT_TRUE(processRGBA(edge));
        EXPECT_TRUE(setKnobValue(edge, kEdgeDetectParamFilter, std::string(filter)));
        EXPECT_TRUE(setKnobValues(edge, kEdgeDetectParamBlurSize, { blurSize }));
        EXPECT_TRUE(setKnobValues(edge, kEdgeDetectParamErodeSize, { erodeSize }));
        EXPECT_TRUE(setKnobValues(edge, kEdgeDetectParamNMS, { nms ? 1. : 0. }));
        EXPECT_TRUE(setKnobValue(edge, kEdgeModeParam, std::string(mode)));

        return edge;
    };
    const auto render = [&](const NodePtr& node, const RectI& window, RenderedPlane* plane) -> bool {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool ok = node && renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, rgba, &planes, &error) && (planes.size() == 1);
        EXPECT_TRUE(ok) << error;
        if (ok) {
            *plane = planes[0];
        }

        return ok;
    };
    const auto pixel = [](const RenderedPlane& plane, int x, int y, int c) -> float {
        const RectI& w = plane.window;

        return plane.pixels[((std::size_t)(y - w.y1) * w.width() + (x - w.x1)) * 4 + c];
    };
    const RectI tile(5, 30, 27, 48);

    struct Case {
        const char* filter;
        double blurSize;
        double erodeSize;
        bool nms;
    };
    const Case cases[] = {
        { kEdgeDetectParamFilterSobel, 30., 2., true },
        { kEdgeDetectParamFilterSimple, 20., -3., false },
        { kEdgeDetectParamFilterGaussian, 25., 0., true },
        { kEdgeDetectParamFilterBox, 15., 0., false },
    };
    for (const Case& k : cases) {
        resetProject();
        const std::string what = std::string(k.filter) + ", blur size " + std::to_string(k.blurSize) + ", erode size " + std::to_string(k.erodeSize) + (k.nms ? ", nms" : "");
        NodePtr stretched = makeEdge(makeSource(format, RectI(32, format.y1, format.x2, format.y2)), k.filter, k.blurSize, k.erodeSize, k.nms, "stretch");
        NodePtr reference = makeEdge(makeSource(padded, RectI(32, padded.y1, padded.x2, padded.y2)), k.filter, k.blurSize, k.erodeSize, k.nms, "black");
        RenderedPlane whole, part, expected;
        ASSERT_TRUE(render(stretched, format, &whole)) << what;
        ASSERT_TRUE(render(stretched, tile, &part)) << what;
        // The recursive blurs' line ends under Black read zero past the buffer, a window plus the
        // halo, so the reference renders far enough past the format that this truncation stays
        // out of the compared pixels.
        const int margin = pad / 2;
        ASSERT_TRUE(render(reference, RectI(format.x1 - margin, format.y1 - margin, format.x2 + margin, format.y2 + margin), &expected)) << what;
        for (int y = format.y1; y < format.y2; ++y) {
            for (int x = format.x1; x < format.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const float v = pixel(whole, x, y, c);
                    ASSERT_TRUE(std::isfinite(v)) << what;
                    ASSERT_NEAR(pixel(expected, x, y, c), v, 1e-4 * std::max(1.f, std::fabs(v))) << what << " at (" << x << "," << y << "," << c << ")";
                    if (tile.contains(x, y)) {
                        ASSERT_NEAR(v, pixel(part, x, y, c), 1e-5 * std::max(1.f, std::fabs(v))) << what << " tile at (" << x << "," << y << "," << c << ")";
                    }
                }
            }
        }
    }

    // A derivative blur alone reads only the source, so a blur far larger than the image costs
    // no more memory than the image.
    resetProject();
    NodePtr huge = makeEdge(makeSource(format, RectI(32, format.y1, format.x2, format.y2)), kEdgeDetectParamFilterGaussian, 20000., 0., false, "stretch");
    RenderedPlane whole, part;
    ASSERT_TRUE(render(huge, format, &whole));
    ASSERT_TRUE(render(huge, tile, &part));
    for (int y = tile.y1; y < tile.y2; ++y) {
        for (int x = tile.x1; x < tile.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
                const float v = pixel(whole, x, y, c);
                ASSERT_TRUE(std::isfinite(v));
                ASSERT_NEAR(v, pixel(part, x, y, c), 1e-5 * std::max(1.f, std::fabs(v))) << "at (" << x << "," << y << "," << c << ")";
            }
        }
    }
}
