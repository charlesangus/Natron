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
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/HostOverlaySupport.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/NativeGenerator.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const int kOfxConstantMajor = 1;
const int kNativeConstantMajor = PLUGIN_MAJOR_NATRON_CONSTANT;
const double kTime = 1.;

// A generator writes knob values straight to the output: no arithmetic, so the exact class.
const ParityTolerance kConstantTolerance = ParityTolerance::exact();

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// Sets a choice by option ID as a user edit, which is what makes the OpenFX generator refresh
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
setTargetChannels(const NodePtr& node,
                  const std::vector<std::string>& channels)
{
    KnobLayerSelectPtr layer = node ? std::dynamic_pointer_cast<KnobLayerSelect>(node->getLayerKnob()) : KnobLayerSelectPtr();

    EXPECT_TRUE(bool(layer));
    if (!layer) {
        return false;
    }
    layer->setChannels(channels);

    return true;
}

RectD
regionOfDefinition(const NodePtr& node,
                   double time)
{
    RectD rod;
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

    EXPECT_TRUE(bool(effect));
    if (!effect) {
        return rod;
    }
    // Called directly rather than through the cached action: a project format change does not
    // change the render hash.
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), time, RenderScale::identity, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat) << node->getPluginID();

    return rod;
}

// A window wider than the extents the cases use, so clipping to the region of definition is
// compared too.
RectI
caseWindow(unsigned mipmapLevel)
{
    return RectD(-4., -4., 68., 52.).toPixelEnclosing(mipmapLevel, 1.);
}

} // namespace

class NativeConstantTest
    : public BaseTest {
protected:
    // A fresh project whose default format is 64x48 at the given pixel aspect ratio.
    void resetProject(double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        useProjectFormat(64, 48, par);
    }

    void useProjectFormat(int width,
                          int height,
                          double par)
    {
        ProjectPtr project = getApp()->getProject();

        // The format choice is keyed by name, so each size needs its own.
        const std::string name = "nativeConstant" + std::to_string(width) + "x" + std::to_string(height) + "par" + std::to_string(par);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, width, height, name, par));
    }

    // Both versions of a generator with nothing connected. The parity source is only there
    // because compareParity() requires one; every case passes its own window.
    ParityPair makeGeneratorPair(const std::string& id)
    {
        ParityPair pair;

        pair.id = id;
        pair.ofxMajor = kOfxConstantMajor;
        pair.nativeMajor = kNativeConstantMajor;
        pair.app = getApp();
        pair.source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        if (isPluginMajorRegistered(id, kOfxConstantMajor)) {
            pair.ofx = createNodeAtMajor(getApp(), id, kOfxConstantMajor);
        }
        pair.native = createNodeAtMajor(getApp(), id, kNativeConstantMajor);
        EXPECT_TRUE(bool(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX " << id << " is retired, so parity replays the recorded references";
        EXPECT_TRUE(isNative(pair.native));

        return pair;
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record,
                      double time = kTime)
    {
        ParityOptions options;

        options.time = time;
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, caseWindow(mipmapLevel), mipmapLevel, kConstantTolerance, record, options);
            EXPECT_TRUE(r.ok) << pair.id << " " << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] " << pair.id << " " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }
};

TEST_F(NativeConstantTest, DefaultExtent)
{
    resetProject();
    ParityPair pair = makeGeneratorPair(PLUGINID_NATRON_CONSTANT);
    ASSERT_TRUE(setKnobOnBoth(pair, kConstantParamColor, { 0.25, -0.5, 1.75, 0.6 }));
    expectParity(pair, "default", true);
}

TEST_F(NativeConstantTest, SizeExtentWithAnOffset)
{
    resetProject();
    ParityPair pair = makeGeneratorPair(PLUGINID_NATRON_CONSTANT);
    ASSERT_TRUE(setChoiceAsUser(pair.native, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobOnBoth(pair, kNativeGeneratorParamBottomLeft, { 11., 7. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kNativeGeneratorParamSize, { 37., 23. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kConstantParamColor, { 0.1, 0.2, 0.3, 0.4 }));
    expectParity(pair, "size-offset", true);
}

TEST_F(NativeConstantTest, OverSourceWritesTheSelectedChannelsAndPassesTheRestThrough)
{
    resetProject();
    ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_CONSTANT, kOfxConstantMajor, kNativeConstantMajor);
    ASSERT_TRUE(bool(pair.native));
    EXPECT_FALSE(pair.live());
    setParitySourceOrigin(pair.source, 5, 3);
    ASSERT_TRUE(setKnobOnBoth(pair, kConstantParamColor, { 0.75, 0.5, 0.25, 0.125 }));
    std::vector<std::string> rg;
    rg.push_back("R");
    rg.push_back("G");
    ASSERT_TRUE(setTargetChannels(pair.native, rg));
    expectParity(pair, "over-source-rg", true);
}

TEST_F(NativeConstantTest, SolidIsOpaque)
{
    resetProject();
    ParityPair pair = makeGeneratorPair(PLUGINID_NATRON_SOLID);
    ASSERT_TRUE(setKnobOnBoth(pair, kConstantParamColor, { 0.3, 0.6, 0.9 }));
    expectParity(pair, "default", true);
}

TEST_F(NativeConstantTest, FrameRangeIsTheTimeDomain)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(isNative(node));
    ASSERT_TRUE(setKnobValues(node, kNativeGeneratorParamFrameRange, { 3., 9. }));

    double first = 0., last = 0.;
    node->getEffectInstance()->getFrameRange_public(node->getEffectInstance()->getRenderHash(), &first, &last, true);
    EXPECT_EQ(3., first);
    EXPECT_EQ(9., last);
}

TEST_F(NativeConstantTest, RecenterCentresTheRectangleOnTheProject)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(isNative(node));
    ASSERT_TRUE(setChoiceAsUser(node, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { 21., 10. }));

    KnobButton* recenter = dynamic_cast<KnobButton*>(node->getKnobByName(kNativeGeneratorParamRecenter).get());
    ASSERT_TRUE(recenter != NULL);
    recenter->trigger();

    KnobDouble* bottomLeft = dynamic_cast<KnobDouble*>(node->getKnobByName(kNativeGeneratorParamBottomLeft).get());
    ASSERT_TRUE(bottomLeft != NULL);
    EXPECT_EQ(21.5, bottomLeft->getValue(0));
    EXPECT_EQ(19., bottomLeft->getValue(1));
}

TEST_F(NativeConstantTest, RectangleOverlayIsEnabledInSizeExtentOnly)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(isNative(node));

    std::list<HostOverlayKnobsPtr> pending = node->getPendingHostOverlays();
    ASSERT_EQ(1u, pending.size());
    HostOverlayKnobsRectanglePtr overlay = std::dynamic_pointer_cast<HostOverlayKnobsRectangle>(pending.front());
    ASSERT_TRUE(bool(overlay));
    EXPECT_TRUE(overlay->checkHostOverlayValid());
    EXPECT_EQ(node->getKnobByName(kNativeGeneratorParamBottomLeft), overlay->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationBottomLeft));
    EXPECT_EQ(node->getKnobByName(kNativeGeneratorParamSize), overlay->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationSize));
    EXPECT_EQ(node->getKnobByName(kNativeGeneratorParamInteractive), overlay->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationInteractive));

    KnobBoolPtr enable = std::dynamic_pointer_cast<KnobBool>(overlay->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationEnable));
    ASSERT_TRUE(bool(enable));
    EXPECT_EQ(node->getKnobByName(kNativeGeneratorParamRectangleEnable), KnobIPtr(enable));
    EXPECT_TRUE(enable->getIsSecret());
    EXPECT_FALSE(enable->getValue());

    ASSERT_TRUE(setChoiceAsUser(node, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    EXPECT_TRUE(enable->getValue());
    EXPECT_FALSE(node->getKnobByName(kNativeGeneratorParamSize)->getIsSecret());
    ASSERT_TRUE(setChoiceAsUser(node, kNativeGeneratorParamExtent, kNativeGeneratorExtentProject));
    EXPECT_FALSE(enable->getValue());
    EXPECT_TRUE(node->getKnobByName(kNativeGeneratorParamSize)->getIsSecret());
}

TEST_F(NativeConstantTest, UnversionedRequestsGetTheNativeGenerators)
{
    const char* const ids[2] = { PLUGINID_NATRON_CONSTANT, PLUGINID_NATRON_SOLID };

    for (int i = 0; i < 2; ++i) {
        NodePtr unversioned = createNode(QString::fromUtf8(ids[i]));
        ASSERT_TRUE(bool(unversioned)) << ids[i];
        EXPECT_TRUE(isNative(unversioned)) << ids[i];
        EXPECT_EQ(kNativeConstantMajor, unversioned->getMajorVersion()) << ids[i];
    }
}

TEST_F(NativeConstantTest, RendersTheSameInBothSchedulerModes)
{
    resetProject();
    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);

    NodePtr alone = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(isNative(alone));
    ASSERT_TRUE(setChoiceAsUser(alone, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(alone, kNativeGeneratorParamBottomLeft, { 11., 7. }));
    ASSERT_TRUE(setKnobValues(alone, kNativeGeneratorParamSize, { 37., 23. }));
    ASSERT_TRUE(setKnobValues(alone, kConstantParamColor, { 0.1, 0.2, 0.3, 0.4 }));

    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr over = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(over));
    connectNodes(source, over, 0, true);
    ASSERT_TRUE(setKnobValues(over, kConstantParamColor, { 0.75, 0.5, 0.25, 0.125 }));
    std::vector<std::string> rg;
    rg.push_back("R");
    rg.push_back("G");
    ASSERT_TRUE(setTargetChannels(over, rg));

    const NodePtr nodes[2] = { alone, over };
    for (int i = 0; i < 2; ++i) {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            std::vector<int> unplannedPulls;
            const RectI window = regionOfDefinition(nodes[i], kTime).toPixelEnclosing(mipmapLevel, 1.);
            const RenderMismatch m = renderBothWaysDirect(nodes[i], kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
            EXPECT_FALSE(m.any) << (i ? "over a source" : "alone") << ", mipmap " << mipmapLevel << ": " << describe(m);
            for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
                EXPECT_EQ(0, unplannedPulls[p]) << (i ? "over a source" : "alone") << ", mipmap " << mipmapLevel << ", pool " << poolSizes[p];
            }
        }
    }
}
