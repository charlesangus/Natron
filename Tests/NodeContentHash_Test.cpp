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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// Reads recompute dirty hashes on their own; these refreshes also force a recompute of nodes
// nothing marked, which the diamond test relies on to compare a forced result with a lazy one.
void
refreshHashes(const std::vector<NodePtr>& upstreamFirst)
{
    for (const NodePtr& node : upstreamFirst) {
        node->refreshHashNow();
    }
}

KnobDoublePtr
blurSize(const NodePtr& blur)
{
    return std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
}

} // namespace

class NodeContentHashTest
    : public BaseTest {
protected:
    NodePtr createConstant()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    }

    NodePtr createBlur()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    }

    NodePtr createMerge()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
    }
};

TEST_F(NodeContentHashTest, IdenticalGraphsHashEqualWhateverTheirNames)
{
    NodePtr constantA = createConstant();
    NodePtr blurA = createBlur();
    NodePtr constantB = createConstant();
    NodePtr blurB = createBlur();
    ASSERT_TRUE(constantA && blurA && constantB && blurB);

    connectNodes(constantA, blurA, 0, true);
    connectNodes(constantB, blurB, 0, true);
    refreshHashes({ constantA, blurA, constantB, blurB });

    EXPECT_NE(constantA->getScriptName(), constantB->getScriptName());
    EXPECT_NE(blurA->getScriptName(), blurB->getScriptName());

    EXPECT_NE(0u, blurA->getContentHash());
    EXPECT_EQ(constantA->getContentHash(), constantB->getContentHash());
    EXPECT_EQ(blurA->getContentHash(), blurB->getContentHash());
    EXPECT_NE(constantA->getContentHash(), blurA->getContentHash());
}

TEST_F(NodeContentHashTest, KnobChangeChangesHashAndRevertRestoresIt)
{
    NodePtr constant = createConstant();
    NodePtr blur = createBlur();
    ASSERT_TRUE(constant && blur);
    connectNodes(constant, blur, 0, true);

    KnobDoublePtr size = blurSize(blur);
    ASSERT_TRUE(bool(size));

    refreshHashes({ constant, blur });
    const double original = size->getValue(0);
    const U64 before = blur->getContentHash();

    size->setValue(original + 5., ViewSpec::all(), 0);
    refreshHashes({ blur });
    const U64 changed = blur->getContentHash();
    EXPECT_NE(before, changed);

    size->setValue(original, ViewSpec::all(), 0);
    refreshHashes({ blur });
    EXPECT_EQ(before, blur->getContentHash());
}

TEST_F(NodeContentHashTest, UpstreamKnobChangeReachesDownstream)
{
    NodePtr constant = createConstant();
    NodePtr blur = createBlur();
    NodePtr downstream = createBlur();
    ASSERT_TRUE(constant && blur && downstream);
    connectNodes(constant, blur, 0, true);
    connectNodes(blur, downstream, 0, true);

    KnobDoublePtr size = blurSize(blur);
    ASSERT_TRUE(bool(size));

    refreshHashes({ constant, blur, downstream });
    const double original = size->getValue(0);
    const U64 before = downstream->getContentHash();

    size->setValue(original + 3., ViewSpec::all(), 0);
    refreshHashes({ blur, downstream });
    EXPECT_NE(before, downstream->getContentHash());

    size->setValue(original, ViewSpec::all(), 0);
    refreshHashes({ blur, downstream });
    EXPECT_EQ(before, downstream->getContentHash());
}

// A -> {B, C} -> Merge D -> E. An edit of A must reach D only once both B and C hold their new
// hashes; otherwise D keys a state (new B, old C) that never existed and can collide later.
TEST_F(NodeContentHashTest, DiamondJoinHashesAfterAllItsInputs)
{
    NodePtr constant = createConstant();
    NodePtr a = createBlur();
    NodePtr b = createBlur();
    NodePtr c = createBlur();
    NodePtr d = createMerge();
    NodePtr e = createBlur();
    ASSERT_TRUE(constant && a && b && c && d && e);
    connectNodes(constant, a, 0, true);
    connectNodes(a, b, 0, true);
    connectNodes(a, c, 0, true);
    connectNodes(b, d, kNativeMergeInputB, true);
    connectNodes(c, d, kNativeMergeInputA, true);
    connectNodes(d, e, 0, true);

    KnobDoublePtr aSize = blurSize(a);
    KnobDoublePtr cSize = blurSize(c);
    ASSERT_TRUE(aSize && cSize);
    cSize->setValue(cSize->getValue(0) + 2., ViewSpec::all(), 0);
    refreshHashes({ constant, a, b, c, d, e });

    for (int edit = 1; edit <= 3; ++edit) {
        const U64 dBefore = d->getContentHash();
        aSize->setValue(aSize->getValue(0) + 1.5, ViewSpec::all(), 0);
        a->refreshHashNow();

        const U64 dAfterEdit = d->getContentHash();
        const U64 eAfterEdit = e->getContentHash();
        EXPECT_NE(dBefore, dAfterEdit) << "edit " << edit;

        d->refreshHashNow();
        EXPECT_EQ(dAfterEdit, d->getContentHash()) << "edit " << edit;
        EXPECT_EQ(eAfterEdit, e->getContentHash()) << "edit " << edit;

        e->refreshHashNow();
        EXPECT_EQ(eAfterEdit, e->getContentHash()) << "edit " << edit;
    }
}

TEST_F(NodeContentHashTest, ConnectingAndDisconnectingChangesHash)
{
    NodePtr constant = createConstant();
    NodePtr blur = createBlur();
    ASSERT_TRUE(constant && blur);

    refreshHashes({ constant, blur });
    const U64 unconnected = blur->getContentHash();

    connectNodes(constant, blur, 0, true);
    refreshHashes({ constant, blur });
    const U64 connected = blur->getContentHash();
    EXPECT_NE(unconnected, connected);

    disconnectNodes(constant, blur, true);
    refreshHashes({ blur });
    EXPECT_EQ(unconnected, blur->getContentHash());
}

TEST_F(NodeContentHashTest, SwappingInputsChangesHash)
{
    NodePtr constant = createConstant();
    NodePtr blurredConstant = createConstant();
    NodePtr blur = createBlur();
    NodePtr merge = createMerge();
    ASSERT_TRUE(constant && blurredConstant && blur && merge);
    connectNodes(blurredConstant, blur, 0, true);

    connectNodes(constant, merge, kNativeMergeInputB, true);
    connectNodes(blur, merge, kNativeMergeInputA, true);
    refreshHashes({ constant, blurredConstant, blur, merge });
    const U64 straight = merge->getContentHash();

    disconnectNodes(constant, merge, true);
    disconnectNodes(blur, merge, true);
    connectNodes(blur, merge, kNativeMergeInputB, true);
    connectNodes(constant, merge, kNativeMergeInputA, true);
    refreshHashes({ merge });
    const U64 swapped = merge->getContentHash();
    EXPECT_NE(straight, swapped);

    disconnectNodes(constant, merge, true);
    disconnectNodes(blur, merge, true);
    connectNodes(constant, merge, kNativeMergeInputB, true);
    connectNodes(blur, merge, kNativeMergeInputA, true);
    refreshHashes({ merge });
    EXPECT_EQ(straight, merge->getContentHash());
}

TEST_F(NodeContentHashTest, RenamingWithoutExpressionsKeepsHash)
{
    NodePtr constant = createConstant();
    NodePtr blur = createBlur();
    ASSERT_TRUE(constant && blur);
    connectNodes(constant, blur, 0, true);

    refreshHashes({ constant, blur });
    const U64 before = blur->getContentHash();

    blur->setScriptName("renamedBlur");
    refreshHashes({ blur });
    EXPECT_EQ(std::string("renamedBlur"), blur->getScriptName());
    EXPECT_EQ(before, blur->getContentHash());
}

TEST_F(NodeContentHashTest, RenamingWithAnExpressionChangesHash)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr size = blurSize(blur);
    ASSERT_TRUE(bool(size));

    size->setExpression(0, "1", false, false);
    refreshHashes({ blur });
    const U64 before = blur->getContentHash();

    blur->setScriptName("renamedExpressionBlur");
    refreshHashes({ blur });
    EXPECT_NE(before, blur->getContentHash());
}

// The parser cannot see a node looked up by a computed name; the read the expression makes is
// recorded while it runs instead.
TEST_F(NodeContentHashTest, AnExpressionReadingANodeFoundByGetattrDependsOnIt)
{
    NodePtr x = createBlur();
    NodePtr y = createBlur();
    ASSERT_TRUE(x && y);
    KnobDoublePtr xSize = blurSize(x);
    KnobDoublePtr ySize = blurSize(y);
    ASSERT_TRUE(xSize && ySize);

    const std::string name = y->getScriptName();
    ASSERT_GE(name.size(), 2u);
    const std::string expression = "getattr(getattr(thisGroup, \"" + name.substr(0, 2) + "\" + \"" + name.substr(2) + "\").size, \"get\" + \"Value\")(0)";
    ASSERT_NO_THROW(xSize->setExpression(0, expression, false, true));
    std::list<std::pair<KnobIWPtr, int>> dependencies;
    ASSERT_TRUE(xSize->getExpressionDependencies(0, dependencies));
    bool found = false;
    for (const std::pair<KnobIWPtr, int>& dependency : dependencies) {
        found = found || (dependency.first.lock() == ySize);
    }
    EXPECT_TRUE(found) << "the size read by getattr is a dependency";

    refreshHashes({ y, x });
    const U64 before = x->getContentHash();
    const double original = ySize->getValue(0);
    ySize->setValue(original + 5., ViewSpec::all(), 0);
    refreshHashes({ y, x });
    EXPECT_NE(before, x->getContentHash());

    ySize->setValue(original, ViewSpec::all(), 0);
    refreshHashes({ y, x });
    EXPECT_EQ(before, x->getContentHash());
}

TEST_F(NodeContentHashTest, ExpressionDependencyOutsideTheGraphChangesHash)
{
    NodePtr x = createBlur();
    NodePtr y = createBlur();
    ASSERT_TRUE(x && y);
    KnobDoublePtr xSize = blurSize(x);
    KnobDoublePtr ySize = blurSize(y);
    ASSERT_TRUE(xSize && ySize);

    ASSERT_NO_THROW(xSize->setExpression(0, y->getScriptName() + ".size.getValue(0)", false, true));
    refreshHashes({ y, x });
    const U64 before = x->getContentHash();
    const U64 knobBefore = xSize->getContentHash();

    const double original = ySize->getValue(0);
    ySize->setValue(original + 5., ViewSpec::all(), 0);
    refreshHashes({ y, x });
    EXPECT_NE(knobBefore, xSize->getContentHash());
    EXPECT_NE(before, x->getContentHash());

    ySize->setValue(original, ViewSpec::all(), 0);
    refreshHashes({ y, x });
    EXPECT_EQ(knobBefore, xSize->getContentHash());
    EXPECT_EQ(before, x->getContentHash());
}

// A slaved knob reads its master on another node, whose edits never invalidate the slave's
// node; the node's knob digest must still see them.
TEST_F(NodeContentHashTest, MasterOnAnotherNodeChangesSlaveNodeHash)
{
    NodePtr master = createBlur();
    NodePtr slave = createBlur();
    ASSERT_TRUE(master && slave);
    KnobDoublePtr masterSize = blurSize(master);
    KnobDoublePtr slaveSize = blurSize(slave);
    ASSERT_TRUE(masterSize && slaveSize);

    ASSERT_TRUE(slaveSize->slaveTo(0, masterSize, 0));
    refreshHashes({ master, slave });
    const U64 before = slave->getContentHash();

    const double original = masterSize->getValue(0);
    masterSize->setValue(original + 4., ViewSpec::all(), 0);
    refreshHashes({ master, slave });
    EXPECT_NE(before, slave->getContentHash());

    masterSize->setValue(original, ViewSpec::all(), 0);
    refreshHashes({ master, slave });
    EXPECT_EQ(before, slave->getContentHash());
}

TEST_F(NodeContentHashTest, CyclicExpressionsHashTerminates)
{
    NodePtr x = createBlur();
    NodePtr y = createBlur();
    ASSERT_TRUE(x && y);
    KnobDoublePtr xSize = blurSize(x);
    KnobDoublePtr ySize = blurSize(y);
    ASSERT_TRUE(xSize && ySize);

    ASSERT_NO_THROW(xSize->setExpression(0, y->getScriptName() + ".size.getValue(0)", false, false));
    ASSERT_NO_THROW(ySize->setExpression(0, x->getScriptName() + ".size.getValue(0)", false, false));

    const U64 xHash = xSize->getContentHash();
    const U64 yHash = ySize->getContentHash();
    EXPECT_EQ(xHash, xSize->getContentHash());
    EXPECT_EQ(yHash, ySize->getContentHash());

    refreshHashes({ x, y });
    EXPECT_NE(U64(0), x->getContentHash());
}

// A rename replaces the old name only where it names that node, and an expression that named the
// new name becomes valid.
TEST_F(NodeContentHashTest, RenameRewritesOnlyTheRenamedNodeInExpressions)
{
    NodePtr source = createBlur();
    NodePtr longer = createBlur();
    NodePtr reader = createBlur();
    ASSERT_TRUE(source && longer && reader);
    source->setScriptName("renameSource");
    longer->setScriptName("renameSource0");
    ASSERT_EQ(std::string("renameSource"), source->getScriptName());
    ASSERT_EQ(std::string("renameSource0"), longer->getScriptName());
    KnobDoublePtr size = blurSize(reader);
    ASSERT_TRUE(bool(size));

    ASSERT_NO_THROW(size->setExpression(0, "renameSource.size.getValue(0) + renameSource0.size.getValue(0)", false, true));
    ASSERT_NO_THROW(size->setExpression(1, "renamedSource.size.getValue(0)", false, false));
    EXPECT_FALSE(size->isExpressionValid(1, 0));

    source->setScriptName("renamedSource");
    EXPECT_EQ(std::string("renamedSource.size.getValue(0) + renameSource0.size.getValue(0)"), size->getExpression(0));
    EXPECT_EQ(std::string("renamedSource.size.getValue(0)"), size->getExpression(1));
    EXPECT_TRUE(size->isExpressionValid(1, 0)) << "the expression naming the new name stayed invalid";

    longer->setScriptName("renamedLonger");
    EXPECT_EQ(std::string("renamedSource.size.getValue(0) + renamedLonger.size.getValue(0)"), size->getExpression(0));
}

// An expression only on a knob that keys no render leaves the frame range out of the hash, so a
// range edit must not mark the node either. Node::dependsOnProjectFrameRange() is what marks.
TEST_F(NodeContentHashTest, ARangeEditDoesNotMarkANodeWhoseOnlyExpressionIsOnANonHashedKnob)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr scratch = AppManager::createKnob<KnobDouble>(blur->getEffectInstance().get(), std::string("scratch"), 1, false);
    ASSERT_TRUE(bool(scratch));
    scratch->setIsPersistent(false);
    scratch->setEvaluateOnChange(false);
    // An expression resolves thisParam as a Python attribute of the node, which a knob added after
    // node creation lacks until the node redeclares its fields.
    blur->declarePythonFields();
    ASSERT_FALSE(scratch->isRenderHashed());
    scratch->setExpression(0, "frame", false, true);

    KnobIntPtr range = std::dynamic_pointer_cast<KnobInt>(getApp()->getProject()->getKnobByName("frameRange"));
    ASSERT_TRUE(bool(range));
    EXPECT_FALSE(blur->dependsOnProjectFrameRange());
    blur->refreshHashNow();
    const U64 before = blur->getContentHash();
    range->setValue(range->getValue(1) + 7, ViewSpec::all(), 1);
    EXPECT_EQ(before, blur->getContentHash());

    KnobDoublePtr size = blurSize(blur);
    ASSERT_TRUE(bool(size));
    size->setExpression(0, "frame", false, true);
    EXPECT_TRUE(blur->dependsOnProjectFrameRange());
    blur->refreshHashNow();
    const U64 folded = blur->getContentHash();
    range->setValue(range->getValue(1) + 3, ViewSpec::all(), 1);
    EXPECT_NE(folded, blur->getContentHash());
}

// A Settings knob that changes what renders produce re-keys every node through the project
// digest; one that does not leaves the digest alone.
TEST_F(NodeContentHashTest, ARenderAffectingSettingMovesTheProjectDigest)
{
    SettingsPtr settings = appPTR->getCurrentSettings();
    ASSERT_TRUE(bool(settings));
    ProjectPtr project = getApp()->getProject();
    ASSERT_TRUE(bool(project));
    KnobBoolPtr concatenation = std::dynamic_pointer_cast<KnobBool>(settings->getKnobByName("transformCatSupport"));
    KnobBoolPtr queueRenders = std::dynamic_pointer_cast<KnobBool>(settings->getKnobByName("queueRenders"));
    ASSERT_TRUE(bool(concatenation));
    ASSERT_TRUE(bool(queueRenders));
    EXPECT_TRUE(settings->isRenderAffecting(concatenation.get()));
    EXPECT_FALSE(settings->isRenderAffecting(queueRenders.get()));

    const U64 before = project->getContentDigest();
    const bool queued = queueRenders->getValue();
    queueRenders->setValue(!queued);
    EXPECT_EQ(before, project->getContentDigest());
    queueRenders->setValue(queued);

    const bool concatenates = concatenation->getValue();
    concatenation->setValue(!concatenates);
    EXPECT_NE(before, project->getContentDigest());
    concatenation->setValue(concatenates);
    EXPECT_EQ(before, project->getContentDigest());
}
