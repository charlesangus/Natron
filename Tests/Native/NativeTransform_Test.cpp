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
#include <list>
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
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kTransformID = PLUGINID_NATRON_TRANSFORM;
const char* const kTransformMaskedID = PLUGINID_NATRON_TRANSFORMMASKED;
const int kOfxTransformMajor = 1;
const int kNativeTransformMajor = PLUGIN_MAJOR_NATRON_TRANSFORM;
const int kNativeTransformMaskedMajor = PLUGIN_MAJOR_NATRON_TRANSFORMMASKED;
const double kTime = 1.;

// The resampler repeats the OpenFX double arithmetic, but sums in a different association order
// in places, so values may differ in the last float bits.
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
        ParityPair pair = makeParityPair(getApp(), id, kOfxTransformMajor, nativeMajor, maskInputLabel);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(isNative(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX " << id << " is retired, so parity replays the recorded references";
        if (pair.source) {
            setParitySourceOrigin(pair.source, 0, 0);
        }
        if (pair.native) {
            EXPECT_TRUE(setKnobOnBoth(pair, kTransformNodeParamCenter, { 32., 24. }));
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
            EXPECT_FALSE(r.live);
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
    ParityPair pair = makePair(kTransformMaskedID, kNativeTransformMaskedMajor, "Mask");
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

TEST_F(NativeTransformTest, MaskedTransformDoesNotOfferItsMatrix)
{
    resetProject();
    NodePtr masked = createNode(QString::fromUtf8(kTransformMaskedID), kNativeTransformMaskedMajor);
    NodePtr plain = createNode(QString::fromUtf8(kTransformID), kNativeTransformMajor);
    ASSERT_TRUE(isNative(masked));
    ASSERT_TRUE(isNative(plain));
    EXPECT_FALSE(masked->getEffectInstance()->getCanTransform());
    EXPECT_TRUE(plain->getEffectInstance()->getCanTransform());
    std::list<int> inputs;
    EXPECT_TRUE(masked->getEffectInstance()->getInputsHoldingTransform(&inputs));
    ASSERT_EQ(1u, inputs.size());
    EXPECT_EQ(0, inputs.front());
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
    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { 0. }));

    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamRotate, { 10. }));
    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamAmount, { 0. }));
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));

    ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 1. }));
    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
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

    NodePtr masked = createNode(QString::fromUtf8(kTransformMaskedID));
    ASSERT_TRUE(bool(masked));
    EXPECT_TRUE(isNative(masked));
    EXPECT_EQ(kNativeTransformMaskedMajor, masked->getMajorVersion());
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
    NodePtr masked = createNode(QString::fromUtf8(kTransformMaskedID), kNativeTransformMaskedMajor);
    ASSERT_TRUE(bool(mask));
    ASSERT_TRUE(isNative(masked));
    connectNodes(source, masked, 0, true);
    connectNodes(mask, masked, 1, true);
    ASSERT_TRUE(setKnobValues(masked, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(masked, kTransformNodeParamTranslate, { 4.5, -2.25 }));
    ASSERT_TRUE(setKnobValues(masked, kOfxMixParamName, { 0.5 }));
    expectBothSchedulerModesAgree(masked);
}
