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

#include <cmath>
#include <functional>
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kTransformID = PLUGINID_NATRON_TRANSFORM;
const int kNativeTransformMajor = PLUGIN_MAJOR_NATRON_TRANSFORM;
const double kTime = 1.;

// The references are recorded from this resampler in one build; another build's float association
// order may differ in the last bits.
const ParityTolerance kResamplingTolerance = ParityTolerance::resampling();
// Motion blur averages up to hundreds of samples, each with the rounding above.
const ParityTolerance kMotionBlurTolerance = ParityTolerance::iir();

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// Keys `name` (a 2-D double knob) on both nodes of the pair to (x0, y0) at t0 and (x1, y1) at t1.
bool
animateOnBoth(const ParityPair& pair,
              const std::string& name,
              double t0,
              double x0,
              double y0,
              double t1,
              double x1,
              double y1)
{
    std::vector<NodePtr> nodes;
    nodes.push_back(pair.native);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        KnobDouble* knob = nodes[i] ? dynamic_cast<KnobDouble*>(nodes[i]->getKnobByName(name).get()) : NULL;
        EXPECT_TRUE(knob != NULL) << name;
        if (!knob) {
            return false;
        }
        knob->setValueAtTime(t0, x0, ViewSpec::all(), 0);
        knob->setValueAtTime(t0, y0, ViewSpec::all(), 1);
        knob->setValueAtTime(t1, x1, ViewSpec::all(), 0);
        knob->setValueAtTime(t1, y1, ViewSpec::all(), 1);
    }

    return true;
}

// The RGBA output of `node` over `window` at mipmap 0, rendered directly without the cache.
bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    EXPECT_EQ(4u, planes[0].channels.size());
    *plane = planes[0];

    return planes[0].channels.size() == 4;
}

float
planeValue(const RenderedPlane& plane,
           int x,
           int y,
           int c)
{
    return plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * 4 + c];
}

} // namespace

class NativeTransformTest
    : public BaseTest {
protected:
    // A fresh project whose default format is 64x48 at the given pixel aspect ratio.
    void resetProject(double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, 64, 48, "nativeTransformPar" + std::to_string(par), par));
    }

    // The pair on a 64x48 source at the origin, the centre set explicitly so it does not depend
    // on when either node reset it on connection.
    ParityPair makePair(const std::string& id,
                        int nativeMajor,
                        const std::string& maskInputLabel = std::string())
    {
        ParityPair pair = makeParityPair(getApp(), id, nativeMajor, maskInputLabel);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(isNative(pair.native));
        if (pair.source) {
            setParitySourceOrigin(pair.source, 0, 0);
        }
        if (pair.native) {
            EXPECT_TRUE(setKnobOnBoth(pair, kTransformNodeParamCenter, { 32., 24. }));
            // The recorded references read black past the source, which carries Stretch.
            EXPECT_TRUE(setKnobOnBoth(pair, kEdgeModeParam, std::string("black")));
        }

        return pair;
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      const ParityTolerance& tolerance,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, tolerance, record);
            EXPECT_TRUE(r.ok) << pair.id << " " << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] " << pair.id << " " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    void expectBothSchedulerModesAgree(const NodePtr& node)
    {
        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            std::vector<int> unplannedPulls;
            const RectI window = RectD(0., 0., 64., 48.).toPixelEnclosing(mipmapLevel, 1.);
            const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
            EXPECT_FALSE(m.any) << node->getScriptName() << ", mipmap " << mipmapLevel << ": " << describe(m);
            for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
                EXPECT_EQ(0, unplannedPulls[p]) << node->getScriptName() << ", mipmap " << mipmapLevel << ", pool " << poolSizes[p];
            }
        }
    }
};

TEST_F(NativeTransformTest, FilterParameterKnobsAndTheirDefaults)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
    ASSERT_TRUE(bool(node));

    KnobIntPtr lobes = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kResamplerParamLobes));
    KnobDoublePtr b = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kResamplerParamBicubicB));
    KnobDoublePtr c = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kResamplerParamBicubicC));
    KnobIPtr clamp = node->getKnobByName(kResamplerParamFilterClamp);
    ASSERT_TRUE(lobes && b && c && clamp);

    EXPECT_EQ("Lobes", lobes->getLabel());
    EXPECT_EQ("Bicubic B", b->getLabel());
    EXPECT_EQ("Bicubic C", c->getLabel());
    EXPECT_EQ(4, lobes->getDefaultValue(0));
    EXPECT_EQ(2, lobes->getMinimum(0));
    EXPECT_EQ(8, lobes->getMaximum(0));
    EXPECT_DOUBLE_EQ(1. / 3., b->getDefaultValue(0));
    EXPECT_DOUBLE_EQ(1. / 3., c->getDefaultValue(0));

    // The default Cubic reads none of them and is never clamped.
    EXPECT_TRUE(lobes->getIsSecret());
    EXPECT_TRUE(b->getIsSecret());
    EXPECT_TRUE(c->getIsSecret());
    EXPECT_TRUE(clamp->getIsSecret());
}

TEST_F(NativeTransformTest, CubicMinifyingAndRotated)
{
    resetProject();
    ParityPair pair = makePair(kTransformID, kNativeTransformMajor);
    ASSERT_TRUE(setKnobOnBoth(pair, kTransformNodeParamRotate, { 30. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kTransformNodeParamScale, { 0.5, 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kResamplerParamFilterType, std::string("cubic")));
    expectParity(pair, "rotate30-scale05-cubic", kResamplingTolerance, true);
}

TEST_F(NativeTransformTest, SkewYX)
{
    resetProject();
    ParityPair pair = makePair(kTransformID, kNativeTransformMajor);
    ASSERT_TRUE(setKnobOnBoth(pair, kTransformNodeParamSkewX, { 0.3 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kTransformNodeParamSkewY, { -0.2 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kTransformNodeParamTranslate, { 3.25, -2.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kTransformNodeParamSkewOrder, std::string("YX")));
    expectParity(pair, "skew-yx", kResamplingTolerance, true);
}

TEST_F(NativeTransformTest, MotionBlurOverTheShutter)
{
    resetProject();
    ParityPair pair = makePair(kTransformID, kNativeTransformMajor);
    ASSERT_TRUE(animateOnBoth(pair, kTransformNodeParamTranslate, 0., 0., 0., 2., 8., 4.));
    ASSERT_TRUE(setKnobOnBoth(pair, kTransformNodeParamMotionBlur, { 1. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kResamplerParamShutterOffset, std::string("centered")));
    expectParity(pair, "motion-blur-centered", kMotionBlurTolerance, true);
}

TEST_F(NativeTransformTest, MaskedMixHalf)
{
    resetProject();
    ParityPair pair = makePair(kTransformID, kNativeTransformMajor, "Mask");
    ASSERT_TRUE(bool(pair.mask));
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kTransformNodeParamRotate, { 30. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kTransformNodeParamScale, { 0.5, 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kOfxMixParamName, { 0.5 }));
    expectParity(pair, "mask-mix05", kResamplingTolerance, true);
}

TEST_F(NativeTransformTest, ChainConcatenates)
{
    resetProject();
    ParityPair first = makePair(kTransformID, kNativeTransformMajor);
    ASSERT_TRUE(setKnobOnBoth(first, kTransformNodeParamTranslate, { 5.5, 3. }));
    ASSERT_TRUE(setKnobOnBoth(first, kTransformNodeParamRotate, { 15. }));

    ParityPair chain = first;
    chain.native = createNodeAtMajor(getApp(), kTransformID, kNativeTransformMajor);
    ASSERT_TRUE(isNative(chain.native));
    ASSERT_TRUE(chain.native->connectInput(first.native, 0));
    ASSERT_TRUE(setKnobOnBoth(chain, kTransformNodeParamCenter, { 32., 24. }));
    ASSERT_TRUE(setKnobOnBoth(chain, kTransformNodeParamScale, { 0.75, 0.6 }));
    ASSERT_TRUE(setKnobOnBoth(chain, kTransformNodeParamTranslate, { -2., 1.5 }));

    // The second transform reads the source directly, through the product of both matrices, so
    // the first one is never rendered.
    EffectInstancePtr second = chain.native->getEffectInstance();
    EXPECT_TRUE(second->getCanTransform());
    InputMatrixMap transforms;
    second->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    const InputMatrix& im = transforms[0];
    ASSERT_TRUE(bool(im.newInputEffect));
    ASSERT_TRUE(bool(im.cat));
    EXPECT_EQ(first.native->getEffectInstance(), im.newInputEffect);
    EXPECT_EQ(first.source->getEffectInstance(), im.newInputEffect->getInput(im.newInputNbToFetchFrom));

    expectBothSchedulerModesAgree(chain.native);
}

// Transform offers its matrix only while no mask is applied and mix is 1; it always applies the
// transforms concatenated into its source.
TEST_F(NativeTransformTest, MaskOrMixStopsItOfferingItsMatrix)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr maskSource = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr node = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(bool(maskSource));
    ASSERT_TRUE(isNative(node));
    connectNodes(source, node, 0, true);
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { 4., 2. }));
    EffectInstancePtr effect = node->getEffectInstance();

    std::list<int> inputs;
    EXPECT_TRUE(effect->getInputsHoldingTransform(&inputs));
    ASSERT_EQ(1u, inputs.size());
    EXPECT_EQ(0, inputs.front());

    Transform::Matrix3x3 matrix;
    EffectInstancePtr inputToTransform;
    EXPECT_TRUE(effect->getCanTransform());
    EXPECT_EQ(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));

    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    EXPECT_TRUE(effect->getCanTransform());
    EXPECT_NE(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 1. }));

    connectNodes(maskSource, node, 1, true);
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    EXPECT_FALSE(effect->getCanTransform());
    EXPECT_NE(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));
}

TEST_F(NativeTransformTest, MotionBlurAndDirectionalBlurDoNotConcatenate)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr node = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(node));
    connectNodes(source, node, 0, true);
    EffectInstancePtr effect = node->getEffectInstance();
    EffectInstancePtr inputToTransform;
    Transform::Matrix3x3 matrix;

    EXPECT_EQ(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));
    EXPECT_EQ(source->getEffectInstance(), inputToTransform);

    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 1. }));
    EXPECT_NE(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));
    EXPECT_EQ(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, true, ViewIdx(0), &inputToTransform, &matrix));

    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamDirectionalBlur, { 1. }));
    EXPECT_NE(eStatusOK, effect->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));
}

TEST_F(NativeTransformTest, IdentityRules)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr node = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(node));
    connectNodes(source, node, 0, true);
    EffectInstancePtr effect = node->getEffectInstance();
    const RectI window(0, 0, 64, 48);
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;

    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);

    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { 1. }));
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { 0. }));

    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamRotate, { 10. }));
    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamAmount, { 0. }));
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));

    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 1. }));
    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
}

TEST_F(NativeTransformTest, ClampIsShownOnlyForFiltersThatHonourIt)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(isNative(node));
    KnobIPtr clamp = node->getKnobByName(kResamplerParamFilterClamp);
    ASSERT_TRUE(bool(clamp));

    EXPECT_TRUE(clamp->getIsSecret());
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, "keys"));
    EXPECT_FALSE(clamp->getIsSecret());
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, "cubic"));
    EXPECT_TRUE(clamp->getIsSecret());
}

TEST_F(NativeTransformTest, ConnectingASourceCentresTheTransformOnce)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr node = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(node));
    setParitySourceOrigin(source, 20, 10);
    connectNodes(source, node, 0, true);

    KnobDouble* center = dynamic_cast<KnobDouble*>(node->getKnobByName(kTransformNodeParamCenter).get());
    KnobDouble* translate = dynamic_cast<KnobDouble*>(node->getKnobByName(kTransformNodeParamTranslate).get());
    KnobBool* centerChanged = dynamic_cast<KnobBool*>(node->getKnobByName(kTransformNodeParamCenterChanged).get());
    ASSERT_TRUE(center != NULL);
    ASSERT_TRUE(translate != NULL);
    ASSERT_TRUE(centerChanged != NULL);
    EXPECT_EQ(20. + 32., center->getValue(0));
    EXPECT_EQ(10. + 24., center->getValue(1));
    EXPECT_EQ(0., translate->getValue(0));
    EXPECT_EQ(0., translate->getValue(1));
    EXPECT_TRUE(centerChanged->getValue());

    // Once the centre was set, a new connection leaves it alone.
    setParitySourceOrigin(source, 0, 0);
    node->disconnectInput(0);
    connectNodes(source, node, 0, true);
    EXPECT_EQ(20. + 32., center->getValue(0));
    EXPECT_EQ(10. + 24., center->getValue(1));

    // Reset Center follows the source and clears the flag; with a rotation, translate
    // compensates so the image does not move.
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamRotate, { 90. }));
    KnobButton* reset = dynamic_cast<KnobButton*>(node->getKnobByName(kTransformNodeParamResetCenter).get());
    ASSERT_TRUE(reset != NULL);
    reset->trigger();
    EXPECT_EQ(32., center->getValue(0));
    EXPECT_EQ(24., center->getValue(1));
    EXPECT_FALSE(centerChanged->getValue());
    EXPECT_NEAR(10. + 20., translate->getValue(0), 1e-9);
    EXPECT_NEAR(-20. + 10., translate->getValue(1), 1e-9);
}

TEST_F(NativeTransformTest, UnversionedRequestsGetTheNativeTransforms)
{
    NodePtr transform = createNode(QString::fromUtf8(kTransformID));
    ASSERT_TRUE(bool(transform));
    EXPECT_TRUE(isNative(transform));
    EXPECT_EQ(kNativeTransformMajor, transform->getMajorVersion());
}

TEST_F(NativeTransformTest, RendersTheSameInBothSchedulerModes)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr node = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(node));
    connectNodes(source, node, 0, true);
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamRotate, { 30. }));
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamScale, { 0.5, 0.5 }));
    expectBothSchedulerModesAgree(node);

    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr masked = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(bool(mask));
    ASSERT_TRUE(isNative(masked));
    connectNodes(source, masked, 0, true);
    connectNodes(mask, masked, 1, true);
    ASSERT_TRUE(setKnobValues(masked, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(masked, kTransformNodeParamTranslate, { 4.5, -2.25 }));
    ASSERT_TRUE(setKnobValues(masked, kOfxMixParamName, { 0.5 }));
    expectBothSchedulerModesAgree(masked);
}

// A masked Transform takes the transform of an upstream Transform as the host concatenates it, so the
// source it fetches is the one before that Transform. Where mask x mix is below 1 it must still
// show its immediate input, the upstream Transform's output, not that earlier source.
TEST_F(NativeTransformTest, MaskedAfterATransformMixesWithItsImmediateInput)
{
    resetProject();
    ASSERT_TRUE(appPTR->getCurrentSettings()->isTransformConcatenationEnabled());
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr maskSource = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr first = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    NodePtr masked = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    NodePtr plain = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(bool(maskSource));
    ASSERT_TRUE(isNative(first));
    ASSERT_TRUE(isNative(masked));
    ASSERT_TRUE(isNative(plain));
    setParitySourceOrigin(source, 0, 0);
    setParitySourceOrigin(maskSource, 10, 6);
    connectNodes(source, first, 0, true);
    connectNodes(first, masked, 0, true);
    connectNodes(maskSource, masked, 1, true);
    connectNodes(first, plain, 0, true);

    ASSERT_TRUE(setKnobValues(first, kTransformNodeParamCenter, { 32., 24. }));
    ASSERT_TRUE(setKnobValues(first, kTransformNodeParamRotate, { 15. }));
    ASSERT_TRUE(setKnobValues(first, kTransformNodeParamTranslate, { 5.5, 3. }));
    const NodePtr second[] = { masked, plain };
    for (const NodePtr& node : second) {
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamCenter, { 32., 24. }));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamScale, { 0.75, 0.6 }));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { -2., 1.5 }));
    }
    ASSERT_TRUE(setKnobValues(masked, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(masked, kOfxMixParamName, { 0.5 }));

    InputMatrixMap transforms;
    masked->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0)) << "the upstream Transform must concatenate into the masked Transform";

    const RectI window(0, 0, 64, 48);
    RenderedPlane out, immediate, transformed, mask, original;
    ASSERT_TRUE(renderRGBA(masked, window, &out));
    ASSERT_TRUE(renderRGBA(first, window, &immediate));
    ASSERT_TRUE(renderRGBA(plain, window, &transformed));
    ASSERT_TRUE(renderRGBA(maskSource, window, &mask));
    ASSERT_TRUE(renderRGBA(source, window, &original));

    const float mix = 0.5f;
    int partial = 0;
    int passThrough = 0;
    int againstOriginal = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float alpha = planeValue(mask, x, y, 3) * mix;
            if (alpha == 0.f) {
                ++passThrough;
            } else if (alpha < 1.f) {
                ++partial;
            }
            for (int c = 0; c < 4; ++c) {
                const float expected = planeValue(transformed, x, y, c) * alpha + (1.f - alpha) * planeValue(immediate, x, y, c);
                ASSERT_NEAR(expected, planeValue(out, x, y, c), 1e-4) << "x " << x << " y " << y << " c " << c;
                const float wrong = planeValue(transformed, x, y, c) * alpha + (1.f - alpha) * planeValue(original, x, y, c);
                if (std::abs(wrong - planeValue(out, x, y, c)) > 1e-2) {
                    ++againstOriginal;
                }
            }
        }
    }
    EXPECT_GT(partial, 0);
    EXPECT_GT(passThrough, 0);
    EXPECT_GT(againstOriginal, 0) << "the case cannot tell the immediate input from the original source";
}

// A partial render window whose pass-through pixels lie outside the back-transformed window: the
// region of interest must cover both, with or without a mask, or the unmasked part renders black.
TEST_F(NativeTransformTest, MaskedPartialWindowFetchesThePassThroughRegion)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr masked = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(masked));
    setParitySourceOrigin(source, 0, 0);
    connectNodes(source, masked, 0, true);
    ASSERT_TRUE(setKnobValues(masked, kTransformNodeParamCenter, { 32., 24. }));
    ASSERT_TRUE(setKnobValues(masked, kTransformNodeParamTranslate, { 30., 0. }));
    ASSERT_TRUE(setKnobValues(masked, kOfxMixParamName, { 0.5 }));

    // Output columns 34..49 read the transformed source at 4..19 and pass source 34..49 through.
    const RectI window(34, 0, 50, 48);
    RoIMap rois;
    masked->getEffectInstance()->getRegionsOfInterest_public(kTime, RenderScale::identity, RectD(0., 0., 95., 48.), RectD(34., 0., 50., 48.), ViewIdx(0), &rois);
    ASSERT_EQ(1u, rois.count(source->getEffectInstance()));
    const RectD& roi = rois[source->getEffectInstance()];
    EXPECT_LE(roi.x1, 4.);
    EXPECT_GE(roi.x2, 50.);

    // Rendered before anything else, so no cached source image hides a short region of interest.
    RenderedPlane out, original;
    ASSERT_TRUE(renderRGBA(masked, window, &out));
    ASSERT_TRUE(renderRGBA(source, RectI(0, 0, 64, 48), &original));

    const float mix = 0.5f;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
                const float expected = planeValue(original, x - 30, y, c) * mix + (1.f - mix) * planeValue(original, x, y, c);
                ASSERT_NEAR(expected, planeValue(out, x, y, c), 1e-6) << "x " << x << " y " << y << " c " << c;
            }
        }
    }
}

TEST_F(NativeTransformTest, InvertedMaskKeepsTheInputWhereTheMaskIsOpaque)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr maskSource = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr masked = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    NodePtr plain = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(bool(maskSource));
    ASSERT_TRUE(isNative(masked));
    ASSERT_TRUE(isNative(plain));
    setParitySourceOrigin(source, 0, 0);
    setParitySourceOrigin(maskSource, 10, 6);
    connectNodes(source, masked, 0, true);
    connectNodes(maskSource, masked, 1, true);
    connectNodes(source, plain, 0, true);
    const NodePtr both[] = { masked, plain };
    for (const NodePtr& node : both) {
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { 5., -3. }));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamRotate, { 10. }));
    }
    ASSERT_TRUE(setKnobValues(masked, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(masked, kOfxMaskInvertParamName, { 1. }));
    ASSERT_TRUE(setKnobValues(masked, kOfxMixParamName, { 0.75 }));

    const RectI window(0, 0, 64, 48);
    RenderedPlane out, transformed, mask, original;
    ASSERT_TRUE(renderRGBA(masked, window, &out));
    ASSERT_TRUE(renderRGBA(plain, window, &transformed));
    ASSERT_TRUE(renderRGBA(maskSource, window, &mask));
    ASSERT_TRUE(renderRGBA(source, window, &original));

    const float mix = 0.75f;
    int kept = 0;
    int blended = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float alpha = (1.f - planeValue(mask, x, y, 3)) * mix;
            if (alpha == 0.f) {
                ++kept;
            } else {
                ++blended;
            }
            for (int c = 0; c < 4; ++c) {
                const float expected = planeValue(transformed, x, y, c) * alpha + (1.f - alpha) * planeValue(original, x, y, c);
                ASSERT_NEAR(expected, planeValue(out, x, y, c), 1e-4) << "x " << x << " y " << y << " c " << c;
            }
        }
    }
    EXPECT_GT(kept, 0);
    EXPECT_GT(blended, 0);
}

TEST_F(NativeTransformTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, "Transform");
}

namespace {

const float kEdgeValue = 0.25f;

const struct {
    EdgeModeChoice choice;
    const char* id;
} kEdgeChoices[] = { { EdgeModeChoice::Input, "input" }, { EdgeModeChoice::Black, "black" }, { EdgeModeChoice::Stretch, "stretch" } };

const char*
edgeModeName(EdgeMode mode)
{
    return (mode == EdgeMode::Stretch) ? "Stretch" : "Black";
}

EdgeMode
outputEdgeMode(const NodePtr& node)
{
    return node->getEffectInstance()->getTimeInvariantMetadata()->getEdgeMode();
}

RectI
pixelRegionOfDefinition(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;

    EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));

    return rod.toPixelEnclosing(0, 1.);
}

// Every channel of plane at (x, y) is value under Stretch and 0 under Black.
void
expectEdgePixel(const RenderedPlane& plane,
                int x,
                int y,
                EdgeMode mode,
                const std::string& what)
{
    for (int c = 0; c < 4; ++c) {
        if (mode == EdgeMode::Stretch) {
            EXPECT_NEAR(kEdgeValue, planeValue(plane, x, y, c), 1e-5) << what << " at " << x << "," << y << " channel " << c;
        } else {
            EXPECT_EQ(0.f, planeValue(plane, x, y, c)) << what << " at " << x << "," << y << " channel " << c;
        }
    }
}

} // namespace

class NativeTransformEdgeModeTest
    : public NativeTransformTest {
protected:
    // A 32x32 constant source of kEdgeValue that carries mode.
    NodePtr createConstantSource(EdgeMode mode)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDCounting));

        EXPECT_TRUE(bool(source));
        if (source) {
            EXPECT_TRUE(setKnobValues(source, "value", { (double)kEdgeValue }));
            EXPECT_TRUE(setKnobValues(source, "blackEdge", { (mode == EdgeMode::Black) ? 1. : 0. }));
        }

        return source;
    }

    NodePtr createTransform(const NodePtr& input,
                            const char* edgeModeID)
    {
        NodePtr node = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);

        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValue(node, kEdgeModeParam, std::string(edgeModeID)));
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamCenter, { 16., 16. }));
        connectNodes(input, node, 0, true);

        return node;
    }

    // A hard Crop under Input of input to rect, which reads input past its region as input's
    // edge mode says.
    NodePtr createReader(const NodePtr& input,
                         const RectI& rect)
    {
        NodePtr crop = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP), PLUGIN_MAJOR_NATRON_CROP);

        EXPECT_TRUE(bool(crop));
        if (!crop) {
            return crop;
        }
        connectNodes(input, crop, 0, true);
        EXPECT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { (double)rect.x1, (double)rect.y1 }));
        EXPECT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { (double)rect.width(), (double)rect.height() }));
        EXPECT_TRUE(setKnobValues(crop, kCropParamSoftness, { 0. }));

        return crop;
    }
};

// Translated by half the image and rotated, the corners of the region read past the source: a
// Stretch read repeats the source's edge there and a Black one reads 0, under the mode the knob
// resolves to, which the output carries on to what reads past the Transform's own region.
TEST_F(NativeTransformEdgeModeTest, TranslatedByHalfTheImageReadsPastTheSourceInTheResolvedMode)
{
    for (EdgeMode sourceMode : { EdgeMode::Stretch, EdgeMode::Black }) {
        for (const auto& c : kEdgeChoices) {
            resetProject();
            const std::string what = std::string(c.id) + " over a " + edgeModeName(sourceMode) + " source";
            NodePtr source = createConstantSource(sourceMode);
            ASSERT_TRUE(bool(source)) << what;
            NodePtr transform = createTransform(source, c.id);
            ASSERT_TRUE(bool(transform)) << what;
            ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamTranslate, { 16., 16. }));
            ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamRotate, { 30. }));

            const EdgeMode mode = resolveEdgeMode(c.choice, sourceMode);
            EXPECT_EQ(mode, outputEdgeMode(transform)) << what;

            const RectI rod = pixelRegionOfDefinition(transform);
            ASSERT_FALSE(rod.isNull()) << what;
            RenderedPlane out;
            ASSERT_TRUE(renderRGBA(transform, rod, &out)) << what;
            expectEdgePixel(out, 32, 32, EdgeMode::Stretch, what + ", centre");
            expectEdgePixel(out, rod.x1, rod.y1, mode, what + ", corner");
            expectEdgePixel(out, rod.x2 - 1, rod.y2 - 1, mode, what + ", corner");

            const RectI around(rod.x1 - 8, rod.y1 - 8, rod.x2 + 8, rod.y2 + 8);
            NodePtr reader = createReader(transform, around);
            ASSERT_TRUE(bool(reader)) << what;
            EXPECT_EQ(mode, outputEdgeMode(reader)) << what;
            RenderedPlane read;
            ASSERT_TRUE(renderRGBA(reader, around, &read)) << what;
            expectEdgePixel(read, around.x1, around.y1, mode, what + ", past the Transform");
            expectEdgePixel(read, 32, 32, EdgeMode::Stretch, what + ", centre through the reader");
        }
    }
}

// The chain is resampled under one mode, so a Transform concatenates its input Transform only when
// both resolve to the same mode.
TEST_F(NativeTransformEdgeModeTest, ConcatenationStopsWhereTheModesDiffer)
{
    for (EdgeMode sourceMode : { EdgeMode::Stretch, EdgeMode::Black }) {
        for (const auto& c : kEdgeChoices) {
            resetProject();
            const std::string what = std::string(c.id) + " over a " + edgeModeName(sourceMode) + " source";
            NodePtr source = createConstantSource(sourceMode);
            ASSERT_TRUE(bool(source)) << what;
            NodePtr first = createTransform(source, "input");
            ASSERT_TRUE(bool(first)) << what;
            ASSERT_TRUE(setKnobValues(first, kTransformNodeParamTranslate, { 3., 1. }));
            NodePtr second = createTransform(first, c.id);
            ASSERT_TRUE(bool(second)) << what;
            ASSERT_TRUE(setKnobValues(second, kTransformNodeParamRotate, { 10. }));

            InputMatrixMap transforms;
            second->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
            const bool same = resolveEdgeMode(c.choice, sourceMode) == sourceMode;
            EXPECT_EQ(same ? 1u : 0u, transforms.count(0)) << what;
            expectBothSchedulerModesAgree(second);
        }
    }
}

// A window that the concatenated chain maps past a Stretch source, though inside the region of
// the Transform it concatenates, asks the source itself for its edge, in the source's space, and
// renders the source's edge; over a Black source it renders 0.
TEST_F(NativeTransformEdgeModeTest, AConcatenatedWindowPastTheSourceReadsTheSourceEdge)
{
    for (EdgeMode sourceMode : { EdgeMode::Stretch, EdgeMode::Black }) {
        resetProject();
        const std::string what = std::string("over a ") + edgeModeName(sourceMode) + " source";
        NodePtr source = createConstantSource(sourceMode);
        ASSERT_TRUE(bool(source)) << what;
        NodePtr first = createTransform(source, "input");
        ASSERT_TRUE(bool(first)) << what;
        ASSERT_TRUE(setKnobValues(first, kTransformNodeParamRotate, { 45. }));
        NodePtr second = createTransform(first, "input");
        ASSERT_TRUE(bool(second)) << what;
        ASSERT_TRUE(setKnobValues(second, kTransformNodeParamTranslate, { 100., 0. }));
        ASSERT_TRUE(setKnobValue(first, kResamplerParamFilterType, std::string("impulse")));
        ASSERT_TRUE(setKnobValue(second, kResamplerParamFilterType, std::string("impulse")));

        EffectInstancePtr effect = second->getEffectInstance();
        InputMatrixMapPtr transforms(new InputMatrixMap());
        effect->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, transforms.get());
        ASSERT_EQ(1u, transforms->count(0)) << what;
        ASSERT_EQ(first->getEffectInstance(), (*transforms)[0].newInputEffect) << what;

        // The corner of the region lies past the rotated square, so the whole window maps past
        // the source while staying inside the first Transform's region.
        const RectI rod = pixelRegionOfDefinition(second);
        const RectI corner(rod.x1, rod.y1, rod.x1 + 2, rod.y1 + 2);
        const RectD window = corner.toCanonical_noClipping(0, 1.);
        const RectD sourceRoD(0., 0., kCountingTestSize, kCountingTestSize);

        RoIMap rois;
        std::map<int, EffectInstancePtr> reroutes;
        effect->getRegionsOfInterest_public(kTime, RenderScale::identity, RectD(rod.x1, rod.y1, rod.x2, rod.y2), window, ViewIdx(0), &rois, transforms, 1., &reroutes);
        RoIMap::const_iterator found = rois.find(source->getEffectInstance());
        if (sourceMode == EdgeMode::Stretch) {
            ASSERT_TRUE(found != rois.end()) << what;
            EXPECT_TRUE(found->second.intersects(sourceRoD)) << what << ": the region asked of the source misses it";
            EXPECT_GE(found->second.x1, sourceRoD.x1) << what;
            EXPECT_LE(found->second.x2, sourceRoD.x2) << what;
            EXPECT_GE(found->second.y1, sourceRoD.y1) << what;
            EXPECT_LE(found->second.y2, sourceRoD.y2) << what;
        } else if (found != rois.end()) {
            EXPECT_FALSE(found->second.intersects(sourceRoD)) << what;
        }

        RenderedPlane out;
        ASSERT_TRUE(renderRGBA(second, corner, &out)) << what;
        for (int y = corner.y1; y < corner.y2; ++y) {
            for (int x = corner.x1; x < corner.x2; ++x) {
                expectEdgePixel(out, x, y, sourceMode, what);
            }
        }
    }
}
