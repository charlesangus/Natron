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

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QCoreApplication>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

// Hashes are recomputed when read, after any edit marked them dirty. None of these tests refreshes
// a hash itself, so each one fails if an edit leaves a hash it changes unmarked.

namespace {

KnobDoublePtr
blurSize(const NodePtr& blur)
{
    return std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
}

void
setMTime(const std::string& path,
         time_t seconds)
{
    struct timespec times[2];

    times[0].tv_sec = seconds;
    times[0].tv_nsec = 0;
    times[1].tv_sec = seconds;
    times[1].tv_nsec = 0;
    ASSERT_EQ(0, utimensat(AT_FDCWD, path.c_str(), times, 0));
}

bool
writeFlatExr(const std::string& path,
             int width,
             int height,
             float value)
{
    OIIO::ImageSpec spec(width, height, 4, OIIO::TypeDesc::FLOAT);

    spec.channelnames = { "R", "G", "B", "A" };
    spec.alpha_channel = 3;
    std::vector<float> pixels((std::size_t)width * height * 4, value);
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        pixels[i] = 1.f;
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

    return out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();
}

} // namespace

class LazyHashTest
    : public BaseTest {
protected:
    // A source feeding two branches: the tests render the viewed one and leave the other unread.
    struct SplitGraph {
        NodePtr constant;
        NodePtr source;
        NodePtr viewed;
        NodePtr viewedTail;
        NodePtr unviewed;
        NodePtr unviewedTail;
    };

    NodePtr createConstant()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    }

    // Its output format is its Size knob, which the clip preferences depend on.
    NodePtr createSizedConstant(double width,
                                double height)
    {
        NodePtr constant = createConstant();
        if (!constant) {
            return NodePtr();
        }
        KnobChoicePtr extent = std::dynamic_pointer_cast<KnobChoice>(constant->getKnobByName(kNativeGeneratorParamExtent));
        KnobBoolPtr reformat = std::dynamic_pointer_cast<KnobBool>(constant->getKnobByName(kNativeGeneratorParamReformat));
        KnobDoublePtr bottomLeft = std::dynamic_pointer_cast<KnobDouble>(constant->getKnobByName(kNativeGeneratorParamBottomLeft));
        KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(constant->getKnobByName(kNativeGeneratorParamSize));
        if (!extent || !reformat || !bottomLeft || !size) {
            return NodePtr();
        }
        extent->setValue((int)ExtentKnobs::eExtentSize);
        reformat->setValue(true);
        bottomLeft->setValue(0., ViewSpec::all(), 0);
        bottomLeft->setValue(0., ViewSpec::all(), 1);
        size->setValue(width, ViewSpec::all(), 0);
        size->setValue(height, ViewSpec::all(), 1);

        return constant;
    }

    bool buildSplit(const NodePtr& constant,
                    SplitGraph* graph)
    {
        graph->constant = constant;
        graph->source = createBlur();
        graph->viewed = createBlur();
        graph->viewedTail = createBlur();
        graph->unviewed = createBlur();
        graph->unviewedTail = createBlur();
        if (!graph->constant || !graph->source || !graph->viewed || !graph->viewedTail || !graph->unviewed || !graph->unviewedTail) {
            return false;
        }

        const std::pair<NodePtr, NodePtr> links[] = {
            { graph->constant, graph->source },
            { graph->source, graph->viewed },
            { graph->viewed, graph->viewedTail },
            { graph->source, graph->unviewed },
            { graph->unviewed, graph->unviewedTail },
        };
        for (const std::pair<NodePtr, NodePtr>& link : links) {
            if (!link.second->connectInput(link.first, 0)) {
                return false;
            }
        }

        return true;
    }

    // After an edit above the split that moved the output format to formatAfter: the edit rehashed
    // no node below it, a render of the viewed branch rehashed that branch only, and the unviewed
    // branch is rehashed, and answers the new format, once its tail is read.
    void expectOnlyTheReadBranchRefreshes(const SplitGraph& g,
                                          U64 viewedTailBefore,
                                          U64 unviewedBefore,
                                          U64 unviewedTailBefore,
                                          const RectI& formatAfter)
    {
        EXPECT_EQ(unviewedBefore, g.unviewed->getStoredContentHash()) << "the edit rehashed a node downstream of it";
        EXPECT_EQ(unviewedTailBefore, g.unviewedTail->getStoredContentHash()) << "the edit rehashed a node downstream of it";

        std::vector<RenderedPlane> planes;
        std::string error;
        const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
        // Through the cache: a render that bypasses it first clears every cache, which refreshes
        // every node in the project.
        ASSERT_TRUE(renderNodePlanesDirect(g.viewedTail, 1., ViewIdx(0), 0, RectI(0, 0, 8, 8), layers, &planes, &error, false)) << error;

        EXPECT_NE(viewedTailBefore, g.viewedTail->getStoredContentHash()) << "the render did not rehash what it rendered";
        EXPECT_EQ(formatAfter, g.viewedTail->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat());
        std::list<ImageLayerDesc> present;
        g.viewedTail->getEffectInstance()->getPresentLayers(1., ViewIdx(0), -1, &present);
        EXPECT_TRUE(std::find(present.begin(), present.end(), ImageLayerDesc::getRGBAComponents()) != present.end());

        EXPECT_EQ(unviewedBefore, g.unviewed->getStoredContentHash()) << "rendering one branch rehashed the other";
        EXPECT_EQ(unviewedTailBefore, g.unviewedTail->getStoredContentHash()) << "rendering one branch rehashed the other";

        EXPECT_EQ(formatAfter, g.unviewedTail->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat());
        EXPECT_NE(unviewedBefore, g.unviewed->getStoredContentHash()) << "reading the tail did not rehash its dirty input";
    }

    NodePtr createBlur()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    }

    // A group with no Input or Output yet, so that each test wires its own.
    NodePtr createEmptyGroup(const NodeCollectionPtr& parent)
    {
        CreateNodeArgs args(PLUGINID_NATRON_GROUP, parent);
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    NodePtr createInside(const NodePtr& group,
                         const std::string& pluginID,
                         int majorVersion = -1)
    {
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
        if (!collection) {
            return NodePtr();
        }
        CreateNodeArgs args(pluginID, collection);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, majorVersion, 0);

        return getApp()->createNode(args);
    }
};

// The edit and the read both happen off the main thread, and the main thread processes no event
// in between, so nothing but the mark can make the read see the edit.
TEST_F(LazyHashTest, KnobSetOnAWorkerIsSeenByAReadOnAnotherWorker)
{
    NodePtr constant = createConstant();
    NodePtr blur = createBlur();
    NodePtr downstream = createBlur();
    ASSERT_TRUE(constant && blur && downstream);
    connectNodes(constant, blur, 0, true);
    connectNodes(blur, downstream, 0, true);

    KnobDoublePtr size = blurSize(blur);
    ASSERT_TRUE(bool(size));
    const double original = size->getValue(0);
    const U64 before = blur->getHashValue();
    const U64 downstreamBefore = downstream->getHashValue();

    std::thread editor([&size, original]() {
        size->setValue(original + 3., ViewSpec::all(), 0);
    });
    editor.join();

    U64 afterOnWorker = 0;
    U64 downstreamAfterOnWorker = 0;
    std::thread reader([&]() {
        downstreamAfterOnWorker = downstream->getHashValue();
        afterOnWorker = blur->getHashValue();
    });
    reader.join();
    EXPECT_NE(before, afterOnWorker);
    EXPECT_NE(downstreamBefore, downstreamAfterOnWorker);
    EXPECT_EQ(afterOnWorker, blur->getHashValue());
    EXPECT_EQ(downstreamAfterOnWorker, downstream->getHashValue());

    QCoreApplication::processEvents();
    size->setValue(original, ViewSpec::all(), 0);
    EXPECT_EQ(before, blur->getHashValue());
    EXPECT_EQ(downstreamBefore, downstream->getHashValue());
}

// A branch nothing reads is never rehashed: an edit only marks, and a render pulls the hashes of
// the nodes it renders. The stored hash of a node folding in the edited one changes as soon as it
// is recomputed, so it staying put says the node was not.
TEST_F(LazyHashTest, AnEditRehashesOnlyTheBranchThatIsRead)
{
    NodePtr constant = createConstant();
    NodePtr source = createBlur();
    NodePtr viewed = createBlur();
    NodePtr viewedTail = createBlur();
    NodePtr unviewed = createBlur();
    NodePtr unviewedTail = createBlur();
    ASSERT_TRUE(constant && source && viewed && viewedTail && unviewed && unviewedTail);
    connectNodes(constant, source, 0, true);
    connectNodes(source, viewed, 0, true);
    connectNodes(viewed, viewedTail, 0, true);
    connectNodes(source, unviewed, 0, true);
    connectNodes(unviewed, unviewedTail, 0, true);

    const U64 viewedBefore = viewedTail->getHashValue();
    const U64 unviewedBefore = unviewed->getHashValue();
    const U64 unviewedTailBefore = unviewedTail->getHashValue();

    KnobDoublePtr size = blurSize(source);
    ASSERT_TRUE(bool(size));
    size->setValue(size->getValue(0) + 2., ViewSpec::all(), 0);

    EXPECT_EQ(unviewedBefore, unviewed->getStoredContentHash()) << "the edit rehashed a node downstream of it";
    EXPECT_EQ(unviewedTailBefore, unviewedTail->getStoredContentHash()) << "the edit rehashed a node downstream of it";

    std::vector<RenderedPlane> planes;
    std::string error;
    const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    // Through the cache: a render that bypasses it first clears every cache, and clearing them
    // refreshes the metadata and layers of every node in the project, which reads every hash.
    ASSERT_TRUE(renderNodePlanesDirect(viewedTail, 1., ViewIdx(0), 0, RectI(0, 0, 8, 8), layers, &planes, &error, false)) << error;
    EXPECT_NE(viewedBefore, viewedTail->getStoredContentHash()) << "the render did not rehash what it rendered";

    EXPECT_EQ(unviewedBefore, unviewed->getStoredContentHash()) << "rendering one branch rehashed the other";
    EXPECT_EQ(unviewedTailBefore, unviewedTail->getStoredContentHash()) << "rendering one branch rehashed the other";

    const U64 unviewedTailAfter = unviewedTail->getHashValue();
    EXPECT_NE(unviewedTailBefore, unviewedTailAfter);
    EXPECT_NE(unviewedBefore, unviewed->getStoredContentHash()) << "reading the tail did not rehash its dirty input";
}

// The metadata of every node below an edit of a knob the clip preferences depend on changes too,
// yet is refreshed only where it is read, which is what keeps an unread branch from being rehashed.
TEST_F(LazyHashTest, AClipPreferenceEditRefreshesOnlyTheBranchThatIsRead)
{
    SplitGraph g;
    ASSERT_TRUE(buildSplit(createSizedConstant(32., 24.), &g));
    const RectI formatBefore = g.constant->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat();
    ASSERT_EQ(formatBefore, g.viewedTail->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat());
    ASSERT_EQ(formatBefore, g.unviewedTail->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat());
    const U64 viewedTailBefore = g.viewedTail->getHashValue();
    const U64 unviewedBefore = g.unviewed->getHashValue();
    const U64 unviewedTailBefore = g.unviewedTail->getHashValue();

    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(g.constant->getKnobByName(kNativeGeneratorParamSize));
    ASSERT_TRUE(size && size->getIsMetadataSlave());
    size->setValue(48., ViewSpec::all(), 0);
    const RectI formatAfter = g.constant->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat();
    ASSERT_NE(formatBefore, formatAfter);

    expectOnlyTheReadBranchRefreshes(g, viewedTailBefore, unviewedBefore, unviewedTailBefore, formatAfter);
}

TEST_F(LazyHashTest, AReconnectionAboveASplitRefreshesOnlyTheBranchThatIsRead)
{
    SplitGraph g;
    ASSERT_TRUE(buildSplit(createSizedConstant(32., 24.), &g));
    NodePtr replacement = createSizedConstant(40., 30.);
    ASSERT_TRUE(bool(replacement));
    const RectI formatAfter = replacement->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat();
    ASSERT_NE(g.constant->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat(), formatAfter);
    const U64 viewedTailBefore = g.viewedTail->getHashValue();
    const U64 unviewedBefore = g.unviewed->getHashValue();
    const U64 unviewedTailBefore = g.unviewedTail->getHashValue();

    ASSERT_TRUE(g.source->replaceInput(replacement, 0));
    EXPECT_EQ(formatAfter, g.source->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat());

    expectOnlyTheReadBranchRefreshes(g, viewedTailBefore, unviewedBefore, unviewedTailBefore, formatAfter);
}

// A persistent knob that does not evaluate on change still renders differently, yet its edits
// bump no knobs age.
TEST_F(LazyHashTest, PersistentKnobThatDoesNotEvaluateOnChangeChangesTheHash)
{
    NodePtr constant = createConstant();
    NodePtr blur = createBlur();
    NodePtr downstream = createBlur();
    ASSERT_TRUE(constant && blur && downstream);
    connectNodes(constant, blur, 0, true);
    connectNodes(blur, downstream, 0, true);

    KnobDoublePtr size = blurSize(blur);
    ASSERT_TRUE(bool(size));
    size->setEvaluateOnChange(false);
    ASSERT_FALSE(size->getEvaluateOnChange());
    ASSERT_TRUE(size->isRenderHashed());

    const double original = size->getValue(0);
    const U64 before = blur->getHashValue();
    const U64 downstreamBefore = downstream->getHashValue();

    size->setValue(original + 2., ViewSpec::all(), 0);
    EXPECT_NE(before, blur->getHashValue());
    EXPECT_NE(downstreamBefore, downstream->getHashValue());

    size->setValue(original, ViewSpec::all(), 0);
    EXPECT_EQ(before, blur->getHashValue());
    EXPECT_EQ(downstreamBefore, downstream->getHashValue());
}

// Roto geometry lives in the context's items, not in knobs. The engine has no undo stack without
// the GUI, so undo is played by moving the point back.
TEST_F(LazyHashTest, RotoPointMoveChangesTheRotoAndDownstreamHashesAndUndoRestoresThem)
{
    getApp()->getProject()->reset(false, true);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    NodePtr blur = createBlur();
    ASSERT_TRUE(roto && blur);
    connectNodes(roto, blur, 0, true);
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    const double time = context->getTimelineCurrentTime();
    // Keyed at the current time: movePointByIndex() ignores a point with no keyframe there.
    BezierPtr square = context->makeSquare(1., 6., 4., time);
    ASSERT_TRUE(bool(square));

    const U64 rotoBefore = roto->getHashValue();
    const U64 blurBefore = blur->getHashValue();
    EXPECT_NE(U64(0), rotoBefore);

    square->movePointByIndex(0, time, 2., 3.);
    const U64 rotoMoved = roto->getHashValue();
    EXPECT_NE(rotoBefore, rotoMoved);
    EXPECT_NE(blurBefore, blur->getHashValue());

    square->movePointByIndex(0, time, -2., -3.);
    EXPECT_EQ(rotoBefore, roto->getHashValue());
    EXPECT_EQ(blurBefore, blur->getHashValue());

    square->movePointByIndex(0, time, 2., 3.);
    EXPECT_EQ(rotoMoved, roto->getHashValue());

    getApp()->getProject()->reset(false, true);
}

// Each shape memoizes its part of the Roto node's hash, so an edit of one shape rehashes that shape
// and only combines the others' memos, however many there are.
TEST_F(LazyHashTest, AnEditOfOneRotoShapeRehashesOnlyThatShape)
{
    getApp()->getProject()->reset(false, true);

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    NodePtr blur = createBlur();
    ASSERT_TRUE(roto && blur);
    connectNodes(roto, blur, 0, true);
    RotoContextPtr context = roto->getRotoContext();
    ASSERT_TRUE(bool(context));
    const double time = context->getTimelineCurrentTime();
    std::vector<BezierPtr> shapes;
    for (int i = 0; i < 12; ++i) {
        BezierPtr square = context->makeSquare(10. * i, 5., 4., time);
        ASSERT_TRUE(bool(square));
        shapes.push_back(square);
    }
    // A selected shape's knobs are slaved to the node's, which an edit of a shape knob would go
    // through instead.
    context->clearSelection(RotoItem::eSelectionReasonOther);
    const U64 before = roto->getHashValue();
    const U64 blurBefore = blur->getHashValue();

    std::mutex rehashedMutex;
    std::vector<const RotoItem*> rehashed;
    // The hook refers to this frame, so it must not outlive it, even past a failed assertion.
    struct HookGuard {
        ~HookGuard() { RotoItem::setContentRehashedHookForTests(std::function<void(const RotoItem*)>()); }
    } hookGuard;
    RotoItem::setContentRehashedHookForTests([&rehashedMutex, &rehashed](const RotoItem* item) {
        std::lock_guard<std::mutex> lock(rehashedMutex);
        rehashed.push_back(item);
    });
    auto takeRehashed = [&rehashedMutex, &rehashed]() {
        std::lock_guard<std::mutex> lock(rehashedMutex);
        std::vector<const RotoItem*> taken;
        taken.swap(rehashed);

        return taken;
    };

    EXPECT_EQ(before, roto->getHashValue());
    EXPECT_TRUE(takeRehashed().empty()) << "a read with no edit rehashed a shape";

    const RotoItem* moved = shapes[7].get();
    shapes[7]->movePointByIndex(0, time, 2., 3.);
    const U64 afterMove = roto->getHashValue();
    EXPECT_NE(before, afterMove);
    EXPECT_NE(blurBefore, blur->getHashValue());
    std::vector<const RotoItem*> afterMoveRehashed = takeRehashed();
    EXPECT_FALSE(afterMoveRehashed.empty());
    for (const RotoItem* item : afterMoveRehashed) {
        EXPECT_EQ(moved, item) << "a point move rehashed a shape it did not touch";
    }

    const RotoItem* faded = shapes[3].get();
    KnobDoublePtr opacity = shapes[3]->getOpacityKnob();
    ASSERT_TRUE(bool(opacity));
    const double opacityBefore = opacity->getValue();
    opacity->setValue(opacityBefore * 0.5, ViewSpec::all(), 0);
    const U64 afterFade = roto->getHashValue();
    EXPECT_NE(afterMove, afterFade);
    std::vector<const RotoItem*> afterFadeRehashed = takeRehashed();
    EXPECT_FALSE(afterFadeRehashed.empty());
    for (const RotoItem* item : afterFadeRehashed) {
        EXPECT_EQ(faded, item) << "a knob edit rehashed a shape it did not touch";
    }

    opacity->setValue(opacityBefore, ViewSpec::all(), 0);
    shapes[7]->movePointByIndex(0, time, -2., -3.);
    EXPECT_EQ(before, roto->getHashValue()) << "the memos did not follow the edits back";
    EXPECT_EQ(blurBefore, blur->getHashValue());

    getApp()->getProject()->reset(false, true);
}

// x reads y through an expression without being downstream of it: only the expression links them.
TEST_F(LazyHashTest, ExpressionListenerOutsideTheGraphGetsANewHash)
{
    NodePtr x = createBlur();
    NodePtr y = createBlur();
    NodePtr belowX = createBlur();
    ASSERT_TRUE(x && y && belowX);
    connectNodes(x, belowX, 0, true);
    KnobDoublePtr xSize = blurSize(x);
    KnobDoublePtr ySize = blurSize(y);
    ASSERT_TRUE(xSize && ySize);

    ASSERT_NO_THROW(xSize->setExpression(0, y->getScriptName() + ".size.getValue(0)", false, true));
    const U64 before = x->getHashValue();
    const U64 belowBefore = belowX->getHashValue();

    const double original = ySize->getValue(0);
    ySize->setValue(original + 5., ViewSpec::all(), 0);
    EXPECT_NE(before, x->getHashValue());
    EXPECT_NE(belowBefore, belowX->getHashValue());

    ySize->setValue(original, ViewSpec::all(), 0);
    EXPECT_EQ(before, x->getHashValue());
    EXPECT_EQ(belowBefore, belowX->getHashValue());
}

TEST_F(LazyHashTest, SlaveNodeHashFollowsItsMaster)
{
    NodePtr master = createBlur();
    NodePtr slave = createBlur();
    NodePtr belowSlave = createBlur();
    ASSERT_TRUE(master && slave && belowSlave);
    connectNodes(slave, belowSlave, 0, true);
    KnobDoublePtr masterSize = blurSize(master);
    KnobDoublePtr slaveSize = blurSize(slave);
    ASSERT_TRUE(masterSize && slaveSize);

    ASSERT_TRUE(slaveSize->slaveTo(0, masterSize, 0));
    const U64 before = slave->getHashValue();
    const U64 belowBefore = belowSlave->getHashValue();

    const double original = masterSize->getValue(0);
    masterSize->setValue(original + 4., ViewSpec::all(), 0);
    EXPECT_NE(before, slave->getHashValue());
    EXPECT_NE(belowBefore, belowSlave->getHashValue());

    masterSize->setValue(original, ViewSpec::all(), 0);
    EXPECT_EQ(before, slave->getHashValue());
    EXPECT_EQ(belowBefore, belowSlave->getHashValue());
}

// A file rewritten on disk is only noticed on Reload; the node reading it, otherwise untouched,
// and everything downstream must then take a new hash.
TEST_F(LazyHashTest, ReloadGivesTheReaderOfARewrittenFileANewHash)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("reloaded.exr")).toStdString();
    ASSERT_TRUE(writeFlatExr(path, 16, 12, 0.25f));
    setMTime(path, 1000000);

    NodePtr reader = createWorkingSpaceRead(path);
    ASSERT_TRUE(bool(reader));
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    connectNodes(reader, blur, 0, true);
    KnobFilePtr fileKnob = std::dynamic_pointer_cast<KnobFile>(reader->getKnobByName(kOfxImageEffectFileParamName));
    ASSERT_TRUE(bool(fileKnob));

    QCoreApplication::processEvents();
    const U64 readerBefore = reader->getHashValue();
    const U64 blurBefore = blur->getHashValue();

    ASSERT_TRUE(writeFlatExr(path, 32, 24, 0.75f));
    setMTime(path, 2000000);
    QCoreApplication::processEvents();
    EXPECT_EQ(readerBefore, reader->getHashValue());
    EXPECT_EQ(blurBefore, blur->getHashValue());

    fileKnob->reloadFile();
    EXPECT_NE(readerBefore, reader->getHashValue());
    EXPECT_NE(blurBefore, blur->getHashValue());
}

// Every node inside a group inherits its disabled state, including the nodes that are not downstream
// of a GroupInput and those in nested groups, which no walk through the outputs reaches.
TEST_F(LazyHashTest, GroupDisableChangesTheHashesOfAllItsInnerNodes)
{
    NodePtr source = createConstant();
    NodePtr group = createEmptyGroup(getApp()->getProject());
    ASSERT_TRUE(source && group);
    NodePtr groupInput = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr innerBlur = createInside(group, PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
    NodePtr groupOutput = createInside(group, PLUGINID_NATRON_OUTPUT);
    NodePtr looseConstant = createInside(group, PLUGINID_NATRON_CONSTANT);
    ASSERT_TRUE(groupInput && innerBlur && groupOutput && looseConstant);
    NodePtr nested = createEmptyGroup(std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance()));
    ASSERT_TRUE(bool(nested));
    NodePtr nestedConstant = createInside(nested, PLUGINID_NATRON_CONSTANT);
    ASSERT_TRUE(bool(nestedConstant));

    ASSERT_TRUE(innerBlur->connectInput(groupInput, 0));
    ASSERT_TRUE(groupOutput->connectInput(innerBlur, 0));
    ASSERT_GE(group->getNInputs(), 1);
    connectNodes(source, group, 0, true);
    NodePtr below = createBlur();
    ASSERT_TRUE(bool(below));
    ASSERT_TRUE(below->connectInput(group, 0));

    KnobBoolPtr disabled = group->getDisabledKnob();
    ASSERT_TRUE(bool(disabled));
    ASSERT_FALSE(disabled->getValue());

    const U64 innerBefore = innerBlur->getHashValue();
    const U64 looseBefore = looseConstant->getHashValue();
    const U64 nestedBefore = nestedConstant->getHashValue();
    const U64 belowBefore = below->getHashValue();

    disabled->setValue(true);
    EXPECT_NE(innerBefore, innerBlur->getHashValue());
    EXPECT_NE(looseBefore, looseConstant->getHashValue());
    EXPECT_NE(nestedBefore, nestedConstant->getHashValue());
    EXPECT_NE(belowBefore, below->getHashValue());

    disabled->setValue(false);
    EXPECT_EQ(innerBefore, innerBlur->getHashValue());
    EXPECT_EQ(looseBefore, looseConstant->getHashValue());
    EXPECT_EQ(nestedBefore, nestedConstant->getHashValue());
    EXPECT_EQ(belowBefore, below->getHashValue());
}

// The nodes below a GroupInput read the group's input through it: rewiring the group must move
// their hashes, and those of the nodes below the group, though none of them was rewired.
TEST_F(LazyHashTest, GroupInputRewireChangesTheHashesDownstreamOfIt)
{
    NodePtr sourceA = createConstant();
    NodePtr sourceB = createBlur();
    NodePtr group = createEmptyGroup(getApp()->getProject());
    ASSERT_TRUE(sourceA && sourceB && group);
    NodePtr groupInput = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr innerBlur = createInside(group, PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
    NodePtr groupOutput = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && innerBlur && groupOutput);
    ASSERT_TRUE(innerBlur->connectInput(groupInput, 0));
    ASSERT_TRUE(groupOutput->connectInput(innerBlur, 0));
    ASSERT_GE(group->getNInputs(), 1);
    NodePtr below = createBlur();
    ASSERT_TRUE(bool(below));
    ASSERT_TRUE(below->connectInput(group, 0));

    const U64 innerUnconnected = innerBlur->getHashValue();
    const U64 belowUnconnected = below->getHashValue();

    connectNodes(sourceA, group, 0, true);
    const U64 innerOnA = innerBlur->getHashValue();
    const U64 belowOnA = below->getHashValue();
    EXPECT_NE(innerUnconnected, innerOnA);
    EXPECT_NE(belowUnconnected, belowOnA);

    ASSERT_TRUE(group->replaceInput(sourceB, 0));
    EXPECT_NE(innerOnA, innerBlur->getHashValue());
    EXPECT_NE(belowOnA, below->getHashValue());

    ASSERT_TRUE(group->replaceInput(sourceA, 0));
    EXPECT_EQ(innerOnA, innerBlur->getHashValue());
    EXPECT_EQ(belowOnA, below->getHashValue());

    ASSERT_NE(-1, group->disconnectInput(0));
    EXPECT_EQ(innerUnconnected, innerBlur->getHashValue());
    EXPECT_EQ(belowUnconnected, below->getHashValue());
}
