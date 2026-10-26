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
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/HostOverlaySupport.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/NativeGenerator.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
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

TEST_F(NativeConstantTest, DefaultExtentIsTheProjectFormatIncludingItsOffset)
{
    resetProject();
    getApp()->getProject()->setAutoSetProjectFormatEnabled(true);
    getApp()->getProject()->setOrAddProjectFormat(Format(-10, -6, 50, 40, "nativeConstantOffset", 1.));
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(isNative(node));

    const RectD rod = regionOfDefinition(node, kTime);
    EXPECT_EQ(-10., rod.x1);
    EXPECT_EQ(-6., rod.y1);
    EXPECT_EQ(50., rod.x2);
    EXPECT_EQ(40., rod.y2);
}

TEST_F(NativeConstantTest, FormatChoiceListsTheProjectFormats)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(isNative(node));

    std::vector<ChoiceOption> projectEntries;
    int projectIndex = 0;
    getApp()->getProject()->getProjectFormatEntries(&projectEntries, &projectIndex);
    KnobChoice* format = dynamic_cast<KnobChoice*>(node->getKnobByName(kNatronParamFormatChoice).get());
    ASSERT_TRUE(format != NULL);
    const std::vector<ChoiceOption> entries = format->getEntries_mt_safe();
    ASSERT_EQ(projectEntries.size(), entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(projectEntries[i].id, entries[i].id);
        EXPECT_EQ(projectEntries[i].label, entries[i].label);
    }
    EXPECT_EQ(projectIndex, format->getValue());

    getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, 30, 20, "nativeConstantAdded", 1.));
    std::vector<ChoiceOption> grown;
    getApp()->getProject()->getProjectFormatEntries(&grown, &projectIndex);
    EXPECT_EQ(grown.size(), format->getEntries_mt_safe().size());
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

TEST_F(NativeConstantTest, ChannelsOutsideTheTargetSelectionAreZero)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(0, node->getNInputs());
    ASSERT_TRUE(setKnobValues(node, kConstantParamColor, { 0.75, 0.5, 0.25, 0.125 }));
    std::vector<std::string> rg;
    rg.push_back("R");
    rg.push_back("G");
    ASSERT_TRUE(setTargetChannels(node, rg));

    const RectI window = regionOfDefinition(node, kTime).toPixelEnclosing(0, 1.);
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    ASSERT_EQ(4u, planes[0].channels.size());
    const float expected[4] = { 0.75f, 0.5f, 0.f, 0.f };
    ASSERT_EQ((std::size_t)window.width() * window.height() * 4, planes[0].pixels.size());
    for (std::size_t i = 0; i < planes[0].pixels.size(); ++i) {
        ASSERT_EQ(expected[i % 4], planes[0].pixels[i]) << "float " << i;
    }
}

TEST_F(NativeConstantTest, SolidIsOpaque)
{
    resetProject();
    ParityPair pair = makeGeneratorPair(PLUGINID_NATRON_SOLID);
    ASSERT_TRUE(setKnobOnBoth(pair, kConstantParamColor, { 0.3, 0.6, 0.9 }));
    expectParity(pair, "default", true);
}

TEST_F(NativeConstantTest, NewGeneratorSpansTheProjectRange)
{
    resetProject();
    getApp()->getProject()->unionFrameRangeWith(5, 20);
    double projectFirst = 0., projectLast = 0.;
    getApp()->getProject()->getFrameRange(&projectFirst, &projectLast);
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(isNative(node));

    double first = 0., last = 0.;
    node->getEffectInstance()->getFrameRange_public(node->getEffectInstance()->getRenderHash(), &first, &last, true);
    EXPECT_EQ(projectFirst, first);
    EXPECT_EQ(projectLast, last);
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

    NodePtr rgOnly = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(isNative(rgOnly));
    ASSERT_TRUE(setKnobValues(rgOnly, kConstantParamColor, { 0.75, 0.5, 0.25, 0.125 }));
    std::vector<std::string> rg;
    rg.push_back("R");
    rg.push_back("G");
    ASSERT_TRUE(setTargetChannels(rgOnly, rg));

    const NodePtr nodes[2] = { alone, rgOnly };
    for (int i = 0; i < 2; ++i) {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            std::vector<int> unplannedPulls;
            const RectI window = regionOfDefinition(nodes[i], kTime).toPixelEnclosing(mipmapLevel, 1.);
            const RenderMismatch m = renderBothWaysDirect(nodes[i], kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
            EXPECT_FALSE(m.any) << (i ? "R and G only" : "all channels") << ", mipmap " << mipmapLevel << ": " << describe(m);
            for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
                EXPECT_EQ(0, unplannedPulls[p]) << (i ? "R and G only" : "all channels") << ", mipmap " << mipmapLevel << ", pool " << poolSizes[p];
            }
        }
    }
}

TEST_F(NativeConstantTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT, "Constant");
    expectNativeIdentity(PLUGINID_NATRON_SOLID, PLUGIN_MAJOR_NATRON_CONSTANT, "Solid");
}

// A Size extent that reformats, with its rectangle keyed to two sizes, is each frame's format,
// whichever frame the timeline is on.
TEST_F(NativeConstantTest, AnAnimatedFormatExtentIsEachFramesFormat)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), kNativeConstantMajor);
    ASSERT_TRUE(isNative(node));
    ASSERT_TRUE(setChoiceAsUser(node, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(node, kNativeGeneratorParamReformat, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    KnobDouble* size = dynamic_cast<KnobDouble*>(node->getKnobByName(kNativeGeneratorParamSize).get());
    ASSERT_TRUE(size != NULL);
    size->setValueAtTime(1, 64., ViewSpec::all(), 0);
    size->setValueAtTime(1, 48., ViewSpec::all(), 1);
    size->setValueAtTime(2, 100., ViewSpec::all(), 0);
    size->setValueAtTime(2, 30., ViewSpec::all(), 1);
    ASSERT_TRUE(size->isAnimated(0) && size->isAnimated(1));

    const SequenceTime timelineFrames[2] = { 2, 1 };
    for (int t = 0; t < 2; ++t) {
        getApp()->getTimeLine()->seekFrame(timelineFrames[t], false, NULL, eTimelineChangeReasonOtherSeek);
        NodeMetadataConstPtr first = node->getEffectInstance()->getMetadata(1, ViewIdx(0));
        NodeMetadataConstPtr second = node->getEffectInstance()->getMetadata(2, ViewIdx(0));
        ASSERT_TRUE(first && second);
        EXPECT_EQ(RectI(0, 0, 64, 48), first->getOutputFormat()) << "timeline at " << timelineFrames[t];
        EXPECT_EQ(RectI(0, 0, 100, 30), second->getOutputFormat()) << "timeline at " << timelineFrames[t];
        EXPECT_DOUBLE_EQ(1., second->getPixelAspectRatio(-1)) << "timeline at " << timelineFrames[t];
    }
}
