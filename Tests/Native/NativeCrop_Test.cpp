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

#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/HostOverlaySupport.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Image/NativeGenerator.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCropID = PLUGINID_NATRON_CROP;
const int kOfxCropMajor = 1;
const int kNativeCropMajor = PLUGIN_MAJOR_NATRON_CROP;
const double kTime = 1.;

// Softness 0 copies source pixels, so the output is bit-identical.
const ParityTolerance kExactTolerance = ParityTolerance::exact();
// A soft edge multiplies by a smoothstep evaluated in double, then rounds to float.
const ParityTolerance kSoftTolerance = ParityTolerance::resampling();

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// Sets a choice by option ID as a user edit, which is what makes the OpenFX plug-in refresh
// which of its knobs are shown.
bool
setChoiceAsUser(const NodePtr& node,
                const std::string& name,
                const std::string& optionID)
{
    KnobChoice* choice = node ? dynamic_cast<KnobChoice*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(choice != NULL) << name;
    if (!choice) {
        return false;
    }
    const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == optionID) {
            choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

            return true;
        }
    }
    ADD_FAILURE() << name << " has no option " << optionID;

    return false;
}

bool
setBoolAsUser(const NodePtr& node,
              const std::string& name,
              bool value)
{
    KnobBool* knob = node ? dynamic_cast<KnobBool*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(knob != NULL) << name;
    if (!knob) {
        return false;
    }
    knob->setValue(value, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

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

// A window wider than the source and the rectangles the cases use, so clipping and the black
// border are compared too.
RectI
caseWindow(unsigned mipmapLevel)
{
    return RectD(-12., -4., 76., 62.).toPixelEnclosing(mipmapLevel, 1.);
}

} // namespace

class NativeCropTest
    : public BaseTest {
protected:
    // A fresh project whose default format is 64x48 at the given pixel aspect ratio.
    void resetProject(double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        const std::string name = "nativeCrop64x48par" + std::to_string(par);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, 64, 48, name, par));
    }

    ParityPair makePair()
    {
        ParityPair pair = makeParityPair(getApp(), kCropID, kOfxCropMajor, kNativeCropMajor);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX Crop is retired, so parity replays the recorded references";
        EXPECT_TRUE(isNative(pair.native));
        if (pair.source) {
            setParitySourceOrigin(pair.source, 0, 0);
        }

        return pair;
    }

    bool setRectangle(const ParityPair& pair,
                      double x,
                      double y,
                      double w,
                      double h)
    {
        return setKnobOnBoth(pair, kNativeGeneratorParamBottomLeft, { x, y }) && setKnobOnBoth(pair, kNativeGeneratorParamSize, { w, h });
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      const ParityTolerance& tolerance,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, caseWindow(mipmapLevel), mipmapLevel, tolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Crop " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }
};

TEST_F(NativeCropTest, SizeExtentIsTheDefault)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(isNative(node));
    KnobChoice* extent = dynamic_cast<KnobChoice*>(node->getKnobByName(kNativeGeneratorParamExtent).get());
    ASSERT_TRUE(extent != NULL);
    EXPECT_EQ((int)Crop::eExtentSize, extent->getValue());
    EXPECT_FALSE(node->getKnobByName(kNativeGeneratorParamSize)->getIsSecret());
    EXPECT_TRUE(node->getKnobByName(kNatronParamFormatChoice)->getIsSecret());
}

TEST_F(NativeCropTest, SizeExtentWithoutSoftness)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setRectangle(pair, 10., 8., 40., 30.));
    expectParity(pair, "size", kExactTolerance, true);
}

TEST_F(NativeCropTest, SoftEdges)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setRectangle(pair, 6., 5., 50., 38.));
    ASSERT_TRUE(setKnobOnBoth(pair, kCropParamSoftness, { 20. }));
    expectParity(pair, "softness-20", kSoftTolerance, true);
}

TEST_F(NativeCropTest, BlackOutside)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setRectangle(pair, 10., 8., 40., 30.));
    ASSERT_TRUE(setKnobOnBoth(pair, kCropParamBlackOutside, { 1. }));
    expectParity(pair, "black-outside", kExactTolerance, true);
}

TEST_F(NativeCropTest, ReformatOnFormatSetsTheOutputFormat)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(isNative(node));
    ASSERT_TRUE(setChoiceAsUser(node, kNativeGeneratorParamExtent, kNativeGeneratorExtentFormat));
    ASSERT_TRUE(setKnobValues(node, kNatronParamFormatSize, { 50., 40. }));
    ASSERT_TRUE(setKnobValues(node, kNatronParamFormatPar, { 2. }));
    ASSERT_TRUE(setKnobValues(node, kCropParamReformat, { 1. }));

    EffectInstancePtr effect = node->getEffectInstance();
    EXPECT_EQ(2., effect->getAspectRatio(-1));
    const RectI format = effect->getOutputFormat();
    EXPECT_EQ(0, format.x1);
    EXPECT_EQ(0, format.y1);
    EXPECT_EQ(50, format.x2);
    EXPECT_EQ(40, format.y2);
}

TEST_F(NativeCropTest, UserReformatEditTogglesBlackOutsideAndTheOverlay)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(isNative(node));

    std::list<HostOverlayKnobsPtr> pending = node->getPendingHostOverlays();
    ASSERT_EQ(1u, pending.size());
    HostOverlayKnobsRectanglePtr overlay = std::dynamic_pointer_cast<HostOverlayKnobsRectangle>(pending.front());
    ASSERT_TRUE(bool(overlay));
    EXPECT_TRUE(overlay->checkHostOverlayValid());
    EXPECT_EQ(node->getKnobByName(kNativeGeneratorParamBottomLeft), overlay->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationBottomLeft));
    EXPECT_EQ(node->getKnobByName(kNativeGeneratorParamSize), overlay->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationSize));

    KnobBoolPtr enable = std::dynamic_pointer_cast<KnobBool>(overlay->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationEnable));
    ASSERT_TRUE(bool(enable));
    EXPECT_EQ(node->getKnobByName(kNativeGeneratorParamRectangleEnable), KnobIPtr(enable));
    EXPECT_TRUE(enable->getIsSecret());
    EXPECT_TRUE(enable->getValue());

    KnobBool* blackOutside = dynamic_cast<KnobBool*>(node->getKnobByName(kCropParamBlackOutside).get());
    ASSERT_TRUE(blackOutside != NULL);
    EXPECT_FALSE(blackOutside->getValue());

    ASSERT_TRUE(setBoolAsUser(node, kCropParamReformat, true));
    EXPECT_FALSE(enable->getValue());
    EXPECT_FALSE(blackOutside->getValue());

    ASSERT_TRUE(setBoolAsUser(node, kCropParamReformat, false));
    EXPECT_TRUE(enable->getValue());
    EXPECT_TRUE(blackOutside->getValue());
}

TEST_F(NativeCropTest, RecenterCentresTheRectangleOnTheSource)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr node = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(node));
    setParitySourceOrigin(source, 20, 10);
    connectNodes(source, node, 0, true);
    ASSERT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { 20., 10. }));

    KnobButton* recenter = dynamic_cast<KnobButton*>(node->getKnobByName(kNativeGeneratorParamRecenter).get());
    ASSERT_TRUE(recenter != NULL);
    recenter->trigger();

    KnobDouble* bottomLeft = dynamic_cast<KnobDouble*>(node->getKnobByName(kNativeGeneratorParamBottomLeft).get());
    ASSERT_TRUE(bottomLeft != NULL);
    EXPECT_EQ(20. + 32. - 10., bottomLeft->getValue(0));
    EXPECT_EQ(10. + 24. - 5., bottomLeft->getValue(1));
}

TEST_F(NativeCropTest, UnversionedRequestsGetTheNativeCrop)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kCropID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeCropMajor, unversioned->getMajorVersion());
}

TEST_F(NativeCropTest, RendersTheSameInBothSchedulerModes)
{
    resetProject();
    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);

    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr crop = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(crop));
    connectNodes(source, crop, 0, true);
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 10., 8. }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { 40., 30. }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamSoftness, { 7. }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamBlackOutside, { 1. }));

    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        std::vector<int> unplannedPulls;
        const RectI window = regionOfDefinition(crop, kTime, RenderScale::fromMipmapLevel(mipmapLevel)).toPixelEnclosing(mipmapLevel, 1.);
        const RenderMismatch m = renderBothWaysDirect(crop, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
            EXPECT_EQ(0, unplannedPulls[p]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[p];
        }
    }
}

TEST_F(NativeCropTest, ReformatOnSizeMakesTheRectangleTheOutputFormat)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr crop = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    NodePtr downstream = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT));
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(crop));
    ASSERT_TRUE(isNative(downstream));
    setParitySourceOrigin(source, 0, 0);
    connectNodes(source, crop, 0, true);
    connectNodes(crop, downstream, 0, true);
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 10., 8. }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { 40., 30. }));

    const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> inPlace;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(crop, kTime, ViewIdx(0), 0, RectI(10, 8, 50, 38), layers, &inPlace, &error)) << error;

    ASSERT_TRUE(setBoolAsUser(crop, kCropParamReformat, true));
    EffectInstancePtr effect = crop->getEffectInstance();
    const RectI format = effect->getOutputFormat();
    EXPECT_EQ(RectI(0, 0, 40, 30), format);
    EXPECT_EQ(1., effect->getAspectRatio(-1));
    EXPECT_EQ(RectD(0., 0., 40., 30.), regionOfDefinition(crop, kTime));
    EXPECT_EQ(RectI(0, 0, 40, 30), downstream->getEffectInstance()->getOutputFormat());

    std::vector<RenderedPlane> moved;
    ASSERT_TRUE(renderNodePlanesDirect(crop, kTime, ViewIdx(0), 0, RectI(0, 0, 40, 30), layers, &moved, &error)) << error;
    ASSERT_EQ(1u, inPlace.size());
    ASSERT_EQ(1u, moved.size());
    EXPECT_EQ(inPlace[0].pixels, moved[0].pixels);

    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { 24., 12. }));
    EXPECT_EQ(RectI(0, 0, 24, 12), effect->getOutputFormat());

    ASSERT_TRUE(setBoolAsUser(crop, kCropParamReformat, false));
    EXPECT_EQ(RectI(0, 0, 64, 48), effect->getOutputFormat());
}

TEST_F(NativeCropTest, ReformatOnDefaultMakesTheSourceExtentTheOutputFormat)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr crop = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(crop));
    setParitySourceOrigin(source, 20, 10);
    connectNodes(source, crop, 0, true);
    ASSERT_TRUE(setChoiceAsUser(crop, kNativeGeneratorParamExtent, kNativeGeneratorExtentDefault));
    ASSERT_TRUE(setBoolAsUser(crop, kCropParamReformat, true));

    const RectD sourceRoD = regionOfDefinition(source, kTime);
    const RectI expected(0, 0, (int)(sourceRoD.x2 - sourceRoD.x1), (int)(sourceRoD.y2 - sourceRoD.y1));
    EXPECT_EQ(expected, crop->getEffectInstance()->getOutputFormat());
    EXPECT_EQ(RectD(0., 0., expected.x2, expected.y2), regionOfDefinition(crop, kTime));
}
