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

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kEdgeDetectID = PLUGINID_NATRON_EDGEDETECT;
const int kOfxEdgeDetectMajor = 4;
const int kNativeEdgeDetectMajor = PLUGIN_MAJOR_NATRON_EDGEDETECT;
const double kTime = 1.;

// The gradients come from the IIR and box line filters (double arithmetic, float storage between
// passes, as in CImg) or from float finite differences; the combination, erosion and non-maxima
// suppression repeat the OpenFX arithmetic, so the whole node is held to the IIR class.
const ParityTolerance kEdgeDetectTolerance = ParityTolerance::iir();

const char* const kFilters[] = {
    kEdgeDetectParamFilterSimple,
    kEdgeDetectParamFilterSobel,
    kEdgeDetectParamFilterRotationInvariant,
    kEdgeDetectParamFilterQuasiGaussian,
    kEdgeDetectParamFilterGaussian,
    kEdgeDetectParamFilterBox,
    kEdgeDetectParamFilterTriangle,
    kEdgeDetectParamFilterQuadratic,
};

const char* const kMultiChannelModes[] = {
    kEdgeDetectParamMultiChannelSeparate,
    kEdgeDetectParamMultiChannelRMS,
    kEdgeDetectParamMultiChannelMax,
    kEdgeDetectParamMultiChannelTensor,
};

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

    ParityPair makePair(bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), kEdgeDetectID, kOfxEdgeDetectMajor, kNativeEdgeDetectMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(isNative(pair.native));
        if (pair.source) {
            setParitySourceOrigin(pair.source, 0, 0);
        }
        // This host keeps the OpenFX plug-in's own R, G, B, A switches all on and applies the
        // channel set afterwards, so the OpenFX EdgeDetect always folds alpha into the RMS, Max
        // and Tensor combinations, even with A off. The native node combines only the channels
        // it processes, so both are compared with alpha processed.
        EXPECT_TRUE(processRGBA(pair.native));
        if (pair.ofx) {
            EXPECT_TRUE(processRGBA(pair.ofx));
        }

        return pair;
    }

    // Compares the pair at mipmap 0 and 1 over the source's region of definition, which is the
    // full frame here, and prints the largest difference of each, so the tolerance actually needed
    // is on record.
    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      const ParityTolerance& tolerance,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, tolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] EdgeDetect " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    void expectSameRegionOfDefinition(const ParityPair& pair,
                                      const std::string& caseName)
    {
        if (!pair.ofx) {
            return;
        }
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const RectD ofx = regionOfDefinition(pair.ofx, kTime, scale);
            const RectD native = regionOfDefinition(pair.native, kTime, scale);
            EXPECT_EQ(ofx.x1, native.x1) << caseName << " mipmap " << mipmapLevel;
            EXPECT_EQ(ofx.y1, native.y1) << caseName << " mipmap " << mipmapLevel;
            EXPECT_EQ(ofx.x2, native.x2) << caseName << " mipmap " << mipmapLevel;
            EXPECT_EQ(ofx.y2, native.y2) << caseName << " mipmap " << mipmapLevel;
        }
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

TEST_F(NativeEdgeDetectTest, KnobsMatchTheOfxPlugin)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live());
    expectKnobParity(pair.ofx, pair.native);
}

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
    EXPECT_TRUE(bool(node->getKnobByName("hostUnPremultBy")));

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

    // The finite-difference schemes use the Gaussian pre-blur's halo.
    params.filter = eEdgeDetectFilterSobel;
    params.sizeX = params.sizeY = 10.;
    roi = EdgeDetect::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(10 - 15 - 1, roi.x1);
    EXPECT_EQ(30 + 15 + 1, roi.x2);

    // Quadratic: 3 * (floor((10 - 1) / 2) + 1) = 15, plus one for the derivative.
    params.filter = eEdgeDetectFilterQuadratic;
    roi = EdgeDetect::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(10 - 15 - 1, roi.x1);

    // Erosion radius 2 on each axis and one more pixel for non-maxima suppression.
    params.filter = eEdgeDetectFilterGaussian;
    params.erodeSize = -2.;
    params.nms = true;
    roi = EdgeDetect::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(10 - 15 - 1 - 2 - 1, roi.x1);
    EXPECT_EQ(40 + 15 + 1 + 2 + 1, roi.y2);

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

TEST_F(NativeEdgeDetectTest, DefaultGaussianTensor)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamBlurSize, { 3. }));
    expectParity(pair, "gaussian-tensor-3", kEdgeDetectTolerance, true);
    expectSameRegionOfDefinition(pair, "gaussian-tensor-3");
}

TEST_F(NativeEdgeDetectTest, EveryFilterAndMultiChannelModeAtBlurSize3)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamBlurSize, { 3. }));

    for (std::size_t f = 0; f < sizeof(kFilters) / sizeof(kFilters[0]); ++f) {
        ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamFilter, std::string(kFilters[f])));
        for (std::size_t m = 0; m < sizeof(kMultiChannelModes) / sizeof(kMultiChannelModes[0]); ++m) {
            ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamMultiChannel, std::string(kMultiChannelModes[m])));
            const std::string caseName = std::string(kFilters[f]) + "-" + kMultiChannelModes[m] + "-3";
            expectParity(pair, caseName, kEdgeDetectTolerance, false);
        }
        expectSameRegionOfDefinition(pair, std::string(kFilters[f]) + "-3");
    }
}

TEST_F(NativeEdgeDetectTest, UnblurredFiniteDifferences)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    const char* const schemes[] = { kEdgeDetectParamFilterSimple, kEdgeDetectParamFilterSobel, kEdgeDetectParamFilterRotationInvariant, kEdgeDetectParamFilterBox };
    for (std::size_t s = 0; s < sizeof(schemes) / sizeof(schemes[0]); ++s) {
        ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamFilter, std::string(schemes[s])));
        expectParity(pair, std::string(schemes[s]) + "-0", kEdgeDetectTolerance, false);
    }
}

TEST_F(NativeEdgeDetectTest, ErodeAndDilateWithSuppression)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamBlurSize, { 3. }));

    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamErodeSize, { 2. }));
    expectParity(pair, "gaussian-tensor-erode2", kEdgeDetectTolerance, false);
    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamErodeSize, { -2. }));
    expectParity(pair, "gaussian-tensor-dilate2", kEdgeDetectTolerance, false);

    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamNMS, { 1. }));
    for (std::size_t m = 0; m < sizeof(kMultiChannelModes) / sizeof(kMultiChannelModes[0]); ++m) {
        ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamMultiChannel, std::string(kMultiChannelModes[m])));
        expectParity(pair, std::string("gaussian-") + kMultiChannelModes[m] + "-dilate2-nms", kEdgeDetectTolerance, false);
    }

    // Erosion leaves flat plateaus, so suppression then compares exactly tied neighbours. Only the
    // Gaussian gradients match the OpenFX plug-in bit for bit (it is built with -Ofast); Sobel's
    // differ by ulps, which flips those ties and whole edge pixels with them.
    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamMultiChannel, std::string(kEdgeDetectParamMultiChannelSeparate)));
    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamErodeSize, { 2. }));
    expectParity(pair, "gaussian-separate-erode2-nms", kEdgeDetectTolerance, true);
}

TEST_F(NativeEdgeDetectTest, ExpandRoDOffAndUncropped)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamBlurSize, { 6. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamCropToFormat, { 0. }));
    expectSameRegionOfDefinition(pair, "uncropped");

    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamFilter, std::string(kEdgeDetectParamFilterTriangle)));
    expectSameRegionOfDefinition(pair, "uncropped-triangle");

    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamExpandRoD, { 0. }));
    expectSameRegionOfDefinition(pair, "expand-off");
    expectParity(pair, "triangle-expand-off", kEdgeDetectTolerance, false);
}

TEST_F(NativeEdgeDetectTest, MaskAndMix)
{
    resetProject();
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 8, 4);
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobOnBoth(pair, kOfxMixParamName, { 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeDetectParamBlurSize, { 3. }));
    expectParity(pair, "mask-mix", kEdgeDetectTolerance, true);

    ASSERT_TRUE(setKnobOnBoth(pair, kOfxMaskInvertParamName, { 1. }));
    expectParity(pair, "mask-mix-invert", kEdgeDetectTolerance, false);
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
