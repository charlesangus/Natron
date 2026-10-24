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

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Merge/Switch.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

RectI
fullWindow()
{
    return RectI(0, 0, kCountingTestSize, kCountingTestSize);
}

// The value of the first channel of the bottom-left pixel of node's RGBA output at time, or a
// failure when the render produced no plane.
bool
renderedValue(const NodePtr& node,
              double time,
              float* value,
              bool byPassCache = true)
{
    std::vector<RenderedPlane> planes;
    std::string error;

    if (!renderNodePlanesDirect(node, time, ViewIdx(0), 0, fullWindow(), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error, byPassCache)) {
        ADD_FAILURE() << "time " << time << ": " << error;

        return false;
    }
    if (planes.empty() || planes[0].pixels.empty()) {
        ADD_FAILURE() << "time " << time << ": no pixel was read back";

        return false;
    }
    *value = planes[0].pixels[0];

    return true;
}

// Renders node's RGBA output over the full window at time through renderRoI, the way a Legacy
// render does, and returns its result code; planes gets what it produced.
EffectInstance::RenderRoIRetCode
renderRoIDirect(const NodePtr& node,
                double time,
                std::map<ImageLayerDesc, ImagePtr>* planes)
{
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameArgs(time,
                                       ViewIdx(0),
                                       true /*isRenderUserInteraction*/,
                                       false /*isSequential*/,
                                       abortInfo,
                                       node,
                                       0 /*textureIndex*/,
                                       node->getApp()->getTimeLine().get(),
                                       NodePtr(),
                                       false /*isAnalysis*/,
                                       false /*draftMode*/,
                                       RenderStatsPtr());
    EffectInstancePtr effect = node->getEffectInstance();
    const RectI window = fullWindow();
    const RectD canonicalWindow = window.toCanonical_noClipping(0, effect->getAspectRatio(-1));
    FrameRequestMap request;

    if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0, canonicalWindow, node, request) == eStatusFailed) {
        return EffectInstance::eRenderRoIRetCodeFailed;
    }
    frameArgs.updateNodesRequest(request);

    EffectInstance::RenderRoIArgs args(time,
                                       RenderScale::identity,
                                       0,
                                       ViewIdx(0),
                                       false /*byPassCache*/,
                                       window,
                                       RectD(),
                                       std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()),
                                       eImageBitDepthFloat,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       time);
    try {
        return effect->renderRoI(args, planes);
    } catch (...) {
        return EffectInstance::eRenderRoIRetCodeFailed;
    }
}

// Whether every pixel of every plane is zero.
bool
allZero(const std::map<ImageLayerDesc, ImagePtr>& planes)
{
    for (std::map<ImageLayerDesc, ImagePtr>::const_iterator it = planes.begin(); it != planes.end(); ++it) {
        const ImagePtr& image = it->second;
        if (!image) {
            continue;
        }
        const RectI bounds = image->getBounds();
        const int nComps = (int)image->getComponentsCount();
        Image::ReadAccess access(image.get());
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            const float* pix = (const float*)access.pixelAt(bounds.x1, y);
            for (int i = 0; pix && (i < bounds.width() * nComps); ++i) {
                if (pix[i] != 0.f) {
                    return false;
                }
            }
        }
    }

    return true;
}

} // namespace

class NativeSwitchTest
    : public BaseTest {
protected:
    void resetProject()
    {
        getApp()->getProject()->reset(false, true);
    }

    NodePtr createSwitch()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));

        EXPECT_TRUE(node && dynamic_cast<Switch*>(node->getEffectInstance().get()));

        return node;
    }

    NodePtr createCountingLeaf(double value)
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));

        if (!node || !setKnobValues(node, "value", { value })) {
            return NodePtr();
        }

        return node;
    }

    // A switch with a counting leaf of value 10 * (k + 1) on each of its inputs 0..count-1.
    NodePtr createSwitchWithLeaves(int count,
                                   std::vector<NodePtr>* leaves)
    {
        NodePtr node = createSwitch();

        leaves->clear();
        for (int k = 0; node && (k < count); ++k) {
            NodePtr leaf = createCountingLeaf(10. * (k + 1));
            EXPECT_TRUE(bool(leaf)) << k;
            if (!leaf) {
                return NodePtr();
            }
            connectNodes(leaf, node, k, true);
            leaves->push_back(leaf);
        }

        return node;
    }

    KnobInt* whichKnob(const NodePtr& node)
    {
        KnobInt* which = node ? dynamic_cast<KnobInt*>(node->getKnobByName(kSwitchParamWhich).get()) : NULL;

        EXPECT_TRUE(which != NULL);

        return which;
    }
};

TEST_F(NativeSwitchTest, InputsAreZeroToSixtyThreeAndTheOpIsARoute)
{
    resetProject();
    NodePtr node = createSwitch();
    ASSERT_TRUE(bool(node));
    // An inspector node shows only the next free input in the node graph, not all 64.
    EXPECT_TRUE(dynamic_cast<InspectorNode*>(node.get()) != NULL);

    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    ASSERT_EQ(kSwitchInputCount, effect->getNInputs());
    EXPECT_EQ("0", effect->getInputLabel(0));
    EXPECT_EQ("1", effect->getInputLabel(1));
    EXPECT_EQ("63", effect->getInputLabel(kSwitchInputCount - 1));
    for (int i = 0; i < kSwitchInputCount; ++i) {
        EXPECT_TRUE(effect->isInputOptional(i)) << i;
        EXPECT_FALSE(effect->isInputMask(i)) << i;
    }
    ASSERT_TRUE(effect->opKind().has_value());
    EXPECT_TRUE(*effect->opKind() == OpKind::Route);
    EXPECT_FALSE(bool(node->getKnobByName("automatic")));
}

TEST_F(NativeSwitchTest, WhichSelectsTheInputAndClampsToTheHighestConnected)
{
    resetProject();
    std::vector<NodePtr> leaves;
    NodePtr node = createSwitchWithLeaves(3, &leaves);
    ASSERT_TRUE(bool(node));
    ASSERT_EQ(3u, leaves.size());

    const int whichValues[] = { 0, 1, 2, 5, 63 };
    const int expectedInputs[] = { 0, 1, 2, 2, 2 };
    for (int c = 0; c < 5; ++c) {
        ASSERT_TRUE(setKnobValues(node, kSwitchParamWhich, { (double)whichValues[c] }));
        const int expected = expectedInputs[c];

        CountingTestRegistry::reset();
        float value = 0.f;
        ASSERT_TRUE(renderedValue(node, kTime, &value)) << "which " << whichValues[c];
        EXPECT_EQ(10.f * (expected + 1), value) << "which " << whichValues[c];
        for (int k = 0; k < 3; ++k) {
            EXPECT_EQ((k == expected) ? 1 : 0, CountingTestRegistry::renders(leaves[k])) << "which " << whichValues[c] << ", leaf " << k;
        }

        EffectInstancePtr effect = node->getEffectInstance();
        RectD rod;
        bool isProjectFormat = false;
        EXPECT_EQ(eStatusOK, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
        EXPECT_EQ(RectD(0., 0., kCountingTestSize, kCountingTestSize), rod) << "which " << whichValues[c];
    }
}

TEST_F(NativeSwitchTest, AnUnconnectedChoiceIsBlack)
{
    resetProject();
    NodePtr node = createSwitch();
    ASSERT_TRUE(bool(node));
    NodePtr low = createCountingLeaf(10.);
    NodePtr high = createCountingLeaf(30.);
    ASSERT_TRUE(low && high);
    connectNodes(low, node, 0, true);
    connectNodes(high, node, 2, true);
    ASSERT_TRUE(setKnobValues(node, kSwitchParamWhich, { 1. }));

    EffectInstancePtr effect = node->getEffectInstance();
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -3;
    EXPECT_TRUE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, fullWindow(), ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(-1, inputNb);

    RectD rod;
    bool isProjectFormat = false;
    effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);
    EXPECT_TRUE(rod.isNull());
}

TEST_F(NativeSwitchTest, AnAnimatedWhichChangesTheOutputPerFrame)
{
    resetProject();
    std::vector<NodePtr> leaves;
    NodePtr node = createSwitchWithLeaves(3, &leaves);
    ASSERT_TRUE(bool(node));
    KnobInt* which = whichKnob(node);
    ASSERT_TRUE(which != NULL);
    for (int frame = 1; frame <= 3; ++frame) {
        which->setValueAtTime(frame, frame - 1, ViewSpec::all(), 0);
    }

    EffectInstancePtr effect = node->getEffectInstance();
    const U64 hash = effect->getRenderHash();
    // Forwards then backwards, through the cache the second time, so a result cached for one
    // frame would show at another.
    const int order[] = { 1, 2, 3, 3, 2, 1 };
    for (int i = 0; i < 6; ++i) {
        const int frame = order[i];
        const bool byPassCache = i < 3;

        CountingTestRegistry::reset();
        float value = 0.f;
        ASSERT_TRUE(renderedValue(node, frame, &value, byPassCache)) << "frame " << frame;
        EXPECT_EQ(10.f * frame, value) << "frame " << frame;
        EXPECT_EQ(1, CountingTestRegistry::totalRenders()) << "frame " << frame;
        EXPECT_EQ(1, CountingTestRegistry::renders(leaves[frame - 1])) << "frame " << frame;

        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -3;
        EXPECT_TRUE(effect->isIdentity_public(true, hash, frame, RenderScale::identity, fullWindow(), ViewIdx(0), &inputTime, &inputView, &inputNb));
        EXPECT_EQ(frame - 1, inputNb) << "frame " << frame;
        EXPECT_EQ((double)frame, inputTime);
    }
    // The content hash is the same at every frame: the caches it keys hold one entry per time.
    EXPECT_EQ(hash, effect->getRenderHash());
}

TEST_F(NativeSwitchTest, DisplayRangeFollowsTheHighestConnectedInput)
{
    resetProject();
    NodePtr node = createSwitch();
    ASSERT_TRUE(bool(node));
    KnobInt* which = whichKnob(node);
    ASSERT_TRUE(which != NULL);
    EXPECT_EQ(0, which->getMinimum(0));
    EXPECT_EQ(kSwitchInputCount - 1, which->getMaximum(0));
    EXPECT_EQ(1, which->getDisplayMaximum(0));

    NodePtr leaf = createCountingLeaf(1.);
    ASSERT_TRUE(bool(leaf));
    connectNodes(leaf, node, 5, true);
    EXPECT_EQ(5, which->getDisplayMaximum(0));
    disconnectNodes(leaf, node, true);
    EXPECT_EQ(1, which->getDisplayMaximum(0));
}

TEST_F(NativeSwitchTest, NoInputIsBlackWithAnEmptyRegion)
{
    resetProject();
    NodePtr node = createSwitch();
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    const U64 hash = effect->getRenderHash();

    // Twice: the second answers come from the actions cache.
    for (int pass = 0; pass < 2; ++pass) {
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -3;
        EXPECT_TRUE(effect->isIdentity_public(true, hash, kTime, RenderScale::identity, fullWindow(), ViewIdx(0), &inputTime, &inputView, &inputNb)) << "pass " << pass;
        EXPECT_EQ(-1, inputNb) << "pass " << pass;

        RectD rod(1., 1., 2., 2.);
        bool isProjectFormat = false;
        EXPECT_EQ(eStatusFailed, effect->getRegionOfDefinition_public(hash, kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat)) << "pass " << pass;
        EXPECT_TRUE(rod.isNull()) << "pass " << pass;

        // The switch's own render fails, so a successful render never reached it.
        std::map<ImageLayerDesc, ImagePtr> planes;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderRoIDirect(node, kTime, &planes)) << "pass " << pass;
        EXPECT_TRUE(allZero(planes)) << "pass " << pass;
    }
}

namespace {

// A native Constant whose output format is width x height at par.
NodePtr
createFormatConstant(const AppInstancePtr& app,
                     double width,
                     double height,
                     double par)
{
    NodePtr node = createNodeAtMajor(app, PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    KnobChoice* extent = node ? dynamic_cast<KnobChoice*>(node->getKnobByName(kNativeGeneratorParamExtent).get()) : NULL;

    EXPECT_TRUE(extent != NULL);
    if (!extent) {
        return NodePtr();
    }
    const std::vector<ChoiceOption> entries = extent->getEntries_mt_safe();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == kNativeGeneratorExtentFormat) {
            extent->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
        }
    }
    if (!setKnobValues(node, kNatronParamFormatSize, { width, height }) || !setKnobValues(node, kNatronParamFormatPar, { par })) {
        return NodePtr();
    }

    return node;
}

const RectI kSwitchProjectFormat(0, 0, 80, 60);
const double kSwitchProjectPar = 1.5;

// What a Switch that routes to black answers: RGBA float at the project format and PAR.
void
expectBlackAnswer(const NodeMetadataConstPtr& metadata,
                  const std::string& what)
{
    ASSERT_TRUE(bool(metadata)) << what;
    EXPECT_EQ(4, metadata->getNComps(-1)) << what;
    EXPECT_EQ(std::string(kNatronColorLayerID), metadata->getComponentsType(-1)) << what;
    EXPECT_EQ(eImageBitDepthFloat, metadata->getBitDepth(-1)) << what;
    EXPECT_EQ(kSwitchProjectFormat, metadata->getOutputFormat()) << what;
    EXPECT_DOUBLE_EQ(kSwitchProjectPar, metadata->getPixelAspectRatio(-1)) << what;
}

} // namespace

// Keyed between two Constants of different format and PAR, the Switch answers each Constant's at
// the frame it routes to it, whichever frame the timeline is on.
TEST_F(NativeSwitchTest, AKeyedSwitchAnswersEachFramesInput)
{
    resetProject();
    NodePtr wide = createFormatConstant(getApp(), 64., 48., 1.);
    NodePtr anamorphic = createFormatConstant(getApp(), 100., 30., 2.);
    NodePtr node = createSwitch();
    ASSERT_TRUE(wide && anamorphic && node);
    connectNodes(wide, node, 0, true);
    connectNodes(anamorphic, node, 1, true);
    KnobInt* which = dynamic_cast<KnobInt*>(node->getKnobByName(kSwitchParamWhich).get());
    ASSERT_TRUE(which != NULL);
    which->setValueAtTime(1, 0, ViewSpec::all(), 0);
    which->setValueAtTime(2, 1, ViewSpec::all(), 0);
    ASSERT_TRUE(which->isAnimated(0));

    const SequenceTime timelineFrames[2] = { 2, 1 };
    for (int t = 0; t < 2; ++t) {
        getApp()->getTimeLine()->seekFrame(timelineFrames[t], false, NULL, eTimelineChangeReasonOtherSeek);
        NodeMetadataConstPtr first = node->getEffectInstance()->getMetadata(1, ViewIdx(0));
        NodeMetadataConstPtr second = node->getEffectInstance()->getMetadata(2, ViewIdx(0));
        ASSERT_TRUE(first && second);
        EXPECT_EQ(RectI(0, 0, 64, 48), first->getOutputFormat()) << "timeline at " << timelineFrames[t];
        EXPECT_DOUBLE_EQ(1., first->getPixelAspectRatio(-1)) << "timeline at " << timelineFrames[t];
        EXPECT_EQ(RectI(0, 0, 100, 30), second->getOutputFormat()) << "timeline at " << timelineFrames[t];
        EXPECT_DOUBLE_EQ(2., second->getPixelAspectRatio(-1)) << "timeline at " << timelineFrames[t];
    }
}

// A Switch whose chosen input is not connected, and a Switch with no input at all, route to black
// and answer RGBA float at the project format rather than any input's.
TEST_F(NativeSwitchTest, ABlackSwitchAnswersTheProjectFormat)
{
    resetProject();
    getApp()->getProject()->setAutoSetProjectFormatEnabled(true);
    getApp()->getProject()->setOrAddProjectFormat(Format(kSwitchProjectFormat.x1, kSwitchProjectFormat.y1, kSwitchProjectFormat.x2, kSwitchProjectFormat.y2, "nativeSwitch80x60", kSwitchProjectPar));

    NodePtr low = createFormatConstant(getApp(), 64., 48., 1.);
    NodePtr high = createFormatConstant(getApp(), 100., 30., 2.);
    NodePtr gap = createSwitch();
    ASSERT_TRUE(low && high && gap);
    connectNodes(low, gap, 0, true);
    connectNodes(high, gap, 2, true);
    ASSERT_TRUE(setKnobValues(gap, kSwitchParamWhich, { 1. }));
    expectBlackAnswer(gap->getEffectInstance()->getMetadata(1, ViewIdx(0)), "which 1 with inputs 0 and 2");

    NodePtr empty = createSwitch();
    ASSERT_TRUE(bool(empty));
    expectBlackAnswer(empty->getEffectInstance()->getMetadata(1, ViewIdx(0)), "no input");
}
