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
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const char* const kGradePluginID = "Grade";

NodePtr
createNamedNode(const AppInstancePtr& app,
                const char* pluginID,
                const std::string& name)
{
    CreateNodeArgs args(pluginID, app->getProject());

    args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);

    return app->createNode(args);
}

KnobDoubleBasePtr
doubleKnob(const NodePtr& node,
           const char* name)
{
    return std::dynamic_pointer_cast<KnobDoubleBase>(node->getKnobByName(name));
}

NodePtr
createWithMix(const AppInstancePtr& app,
              const std::string& name,
              double mix)
{
    NodePtr node = createNamedNode(app, kGradePluginID, name);
    if (node) {
        doubleKnob(node, "mix")->setValue(mix, ViewSpec::all(), 0);
    }

    return node;
}

double
readOffset(const KnobDoubleBasePtr& offset)
{
    return offset->getValueAtTime(1., 0, ViewSpec::current(), false);
}

} // namespace

class GraphScalingGuiEdits
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

TEST_F(GraphScalingGuiEdits, ExpressionCompiledOnce)
{
    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(grade));
    KnobDoubleBasePtr offset = std::dynamic_pointer_cast<KnobDoubleBase>(grade->getKnobByName("offset"));
    ASSERT_TRUE(bool(offset));

    ASSERT_NO_THROW(offset->setExpression(0, "frame * 2 + view", false, true));
    // The frame reaches the expression as the int or float literal a script would pass.
    ASSERT_NO_THROW(offset->setExpression(1, "1 if isinstance(frame, int) else 2", false, true));

    const U64 resolvedBefore = KnobHelper::getExpressionFunctionsResolvedCount();
    const U64 scriptsBefore = KnobHelper::getExpressionScriptsRunCount();
    for (int i = 0; i < 1000; ++i) {
        const double time = i * 0.5;
        EXPECT_DOUBLE_EQ(time * 2, offset->getValueAtTime(time, 0, ViewSpec::current(), false));
        EXPECT_DOUBLE_EQ((i % 2) ? 2. : 1., offset->getValueAtTime(time, 1, ViewSpec::current(), false));
    }
    EXPECT_EQ(0u, KnobHelper::getExpressionFunctionsResolvedCount() - resolvedBefore);
    EXPECT_EQ(0u, KnobHelper::getExpressionScriptsRunCount() - scriptsBefore);

    // Setting an expression after a rename keeps the function it defines under the new name.
    ASSERT_NO_THROW(grade->setScriptName("RenamedGrade"));
    ASSERT_NO_THROW(offset->setExpression(0, "frame + 1", false, true));
    const U64 resolvedAfterRename = KnobHelper::getExpressionFunctionsResolvedCount();
    for (int i = 0; i < 100; ++i) {
        EXPECT_DOUBLE_EQ(1000. + i + 1, offset->getValueAtTime(1000. + i, 0, ViewSpec::current(), false));
    }
    EXPECT_EQ(0u, KnobHelper::getExpressionFunctionsResolvedCount() - resolvedAfterRename);
    EXPECT_EQ(0u, KnobHelper::getExpressionScriptsRunCount() - scriptsBefore);

    offset->clearExpression(0, true);
    EXPECT_TRUE(offset->getExpression(0).empty());
}

TEST_F(GraphScalingGuiEdits, InvalidExpressionsRecheckedByName)
{
    const int kNumBroken = 200;
    std::vector<KnobIPtr> broken;

    for (int i = 0; i < kNumBroken; ++i) {
        NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
        ASSERT_TRUE(bool(grade));
        KnobIPtr offset = grade->getKnobByName("offset");
        ASSERT_TRUE(bool(offset));
        ASSERT_NO_THROW(offset->setExpression(0, "Target.mix.get()", false, false));
        ASSERT_FALSE(offset->isExpressionValid(0, 0));
        broken.push_back(offset);
    }

    NodePtr unrelatedInput = createNamedNode(getApp(), kGradePluginID, "UnrelatedInput");
    NodePtr unrelated = createNamedNode(getApp(), kGradePluginID, "Unrelated");
    ASSERT_TRUE(unrelatedInput && unrelated);
    const U64 beforeConnect = getApp()->getInvalidExpressionRechecksCount();
    connectNodes(unrelatedInput, unrelated, 0, true);
    EXPECT_EQ(0u, getApp()->getInvalidExpressionRechecksCount() - beforeConnect);
    for (int i = 0; i < kNumBroken; ++i) {
        EXPECT_FALSE(broken[i]->isExpressionValid(0, 0));
    }

    const U64 beforeCreate = getApp()->getInvalidExpressionRechecksCount();
    NodePtr target = createNamedNode(getApp(), kGradePluginID, "Target");
    ASSERT_TRUE(bool(target));
    ASSERT_EQ(std::string("Target"), target->getScriptName_mt_safe());
    EXPECT_EQ((U64)kNumBroken, getApp()->getInvalidExpressionRechecksCount() - beforeCreate);
    for (int i = 0; i < kNumBroken; ++i) {
        EXPECT_TRUE(broken[i]->isExpressionValid(0, 0));
    }

    // Nothing is left to recheck.
    const U64 beforeSecondConnect = getApp()->getInvalidExpressionRechecksCount();
    connectNodes(target, unrelatedInput, 0, true);
    EXPECT_EQ(0u, getApp()->getInvalidExpressionRechecksCount() - beforeSecondConnect);
}

TEST_F(GraphScalingGuiEdits, ExpressionFunctionsSurviveDeactivation)
{
    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(grade));
    KnobDoubleBasePtr offset = std::dynamic_pointer_cast<KnobDoubleBase>(grade->getKnobByName("offset"));
    ASSERT_TRUE(bool(offset));
    ASSERT_NO_THROW(offset->setExpression(0, "frame * 2", false, true));

    grade->deactivate();
    grade->activate();

    const U64 scriptsBefore = KnobHelper::getExpressionScriptsRunCount();
    EXPECT_DOUBLE_EQ(14., offset->getValueAtTime(7., 0, ViewSpec::current(), false));
    EXPECT_TRUE(offset->isExpressionValid(0, 0));
    EXPECT_EQ(0u, KnobHelper::getExpressionScriptsRunCount() - scriptsBefore);
}

TEST_F(GraphScalingGuiEdits, RepeatedExpressionFailureWritesNoContent)
{
    NodePtr target = createNamedNode(getApp(), kGradePluginID, "Target");
    NodePtr reader = createNamedNode(getApp(), kGradePluginID, "Reader");
    ASSERT_TRUE(target && reader);
    KnobDoubleBasePtr offset = std::dynamic_pointer_cast<KnobDoubleBase>(reader->getKnobByName("offset"));
    ASSERT_TRUE(bool(offset));
    ASSERT_NO_THROW(offset->setExpression(0, "Target.mix.get()", false, true));

    target->deactivate();
    (void)offset->getValueAtTime(1., 0, ViewSpec::current(), false);
    ASSERT_FALSE(offset->isExpressionValid(0, 0));

    const U64 writes = KnobContentWrite::snapshot();
    const U64 marks = reader->getHashMarkCount();
    for (int i = 2; i < 12; ++i) {
        (void)offset->getValueAtTime(i, 0, ViewSpec::current(), false);
    }
    EXPECT_EQ(writes, KnobContentWrite::snapshot());
    EXPECT_EQ(marks, reader->getHashMarkCount());
}

TEST_F(GraphScalingGuiEdits, InvalidExpressionTextIsNotRun)
{
    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(grade));
    KnobDoubleBasePtr offset = std::dynamic_pointer_cast<KnobDoubleBase>(grade->getKnobByName("offset"));
    ASSERT_TRUE(bool(offset));
    ASSERT_NO_THROW(offset->setExpression(0, "Target.mix.get()", false, false));
    std::string validationError;
    ASSERT_FALSE(offset->isExpressionValid(0, &validationError));

    const U64 scriptsBefore = KnobHelper::getExpressionScriptsRunCount();
    (void)offset->getValueAtTime(1., 0, ViewSpec::current(), false);
    EXPECT_EQ(0u, KnobHelper::getExpressionScriptsRunCount() - scriptsBefore);
    std::string errorAfterRead;
    EXPECT_FALSE(offset->isExpressionValid(0, &errorAfterRead));
    EXPECT_EQ(validationError, errorAfterRead);
}

TEST_F(GraphScalingGuiEdits, ExpressionNamingARemovedNodeIsInvalid)
{
    {
        NodePtr target = createNamedNode(getApp(), kGradePluginID, "Target");
        NodePtr reader = createNamedNode(getApp(), kGradePluginID, "Reader");
        ASSERT_TRUE(target && reader);
        ASSERT_NO_THROW(reader->getKnobByName("offset")->setExpression(0, "Target.mix.get()", false, true));
    }
    getApp()->getProject()->reset(false, true);

    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(grade));
    KnobIPtr offset = grade->getKnobByName("offset");
    ASSERT_TRUE(bool(offset));
    ASSERT_NO_THROW(offset->setExpression(0, "Target.mix.get()", false, false));
    EXPECT_FALSE(offset->isExpressionValid(0, 0));
}

TEST_F(GraphScalingGuiEdits, DeadParamWrapperRegistersNoDependency)
{
    const std::string appID = getApp()->getAppIDString();
    std::string error;
    {
        NodePtr target = createNamedNode(getApp(), kGradePluginID, "Target");
        ASSERT_TRUE(bool(target));
        ASSERT_TRUE(interpretPythonScript("deadParamWrapper = " + appID + ".Target.mix\n", &error, 0)) << error;
    }
    getApp()->getProject()->reset(false, true);

    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(grade));
    const std::string offsetPath = appID + "." + grade->getScriptName_mt_safe() + ".offset";
    EXPECT_TRUE(interpretPythonScript("deadParamWrapper.addAsDependencyOf(0, " + offsetPath + ", 0)\ndel deadParamWrapper\n", &error, 0)) << error;
}

TEST_F(GraphScalingGuiEdits, InputEditBatchRechecksOnceAtItsEnd)
{
    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(grade));
    KnobIPtr offset = grade->getKnobByName("offset");
    ASSERT_TRUE(bool(offset));
    ASSERT_NO_THROW(offset->setExpression(0, "Target.mix.get()", false, false));

    const std::set<std::string> names = { "Target" };
    const U64 before = getApp()->getInvalidExpressionRechecksCount();
    {
        Node::InputEditBatch batch(getApp());
        {
            Node::InputEditBatch nested(getApp());
            getApp()->recheckInvalidExpressionsReferencing(names);
        }
        getApp()->recheckInvalidExpressionsReferencing(names);
        EXPECT_EQ(0u, getApp()->getInvalidExpressionRechecksCount() - before);
    }
    EXPECT_EQ(1u, getApp()->getInvalidExpressionRechecksCount() - before);
}

TEST_F(GraphScalingGuiEdits, InputEditBatchRefreshesEditedNodesAtItsEnd)
{
    NodePtr source = createNamedNode(getApp(), kGradePluginID, "Source");
    NodePtr first = createNamedNode(getApp(), kGradePluginID, "First");
    NodePtr second = createNamedNode(getApp(), kGradePluginID, "Second");
    ASSERT_TRUE(source && first && second);
    connectNodes(first, second, 0, true);
    first->forceRefreshAllInputRelatedData();
    ASSERT_FALSE(first->isInputRelatedDataDirty());
    ASSERT_FALSE(second->isInputRelatedDataDirty());

    {
        Node::InputEditBatch batch(getApp());
        connectNodes(source, first, 0, true);
        EXPECT_FALSE(first->isInputRelatedDataDirty());
        EXPECT_FALSE(second->isInputRelatedDataDirty());
    }
    EXPECT_FALSE(first->isInputRelatedDataDirty());
    EXPECT_TRUE(second->isInputRelatedDataDirty());
}

TEST_F(GraphScalingGuiEdits, DiamondListenersRefreshedOnce)
{
    const int kLevels = 20;
    std::vector<KnobDoubleBasePtr> left, right;

    for (int level = 0; level <= kLevels; ++level) {
        NodePtr l = createNamedNode(getApp(), kGradePluginID, "L" + std::to_string(level));
        NodePtr r = createNamedNode(getApp(), kGradePluginID, "R" + std::to_string(level));
        ASSERT_TRUE(l && r);
        left.push_back(std::dynamic_pointer_cast<KnobDoubleBase>(l->getKnobByName("offset")));
        right.push_back(std::dynamic_pointer_cast<KnobDoubleBase>(r->getKnobByName("offset")));
        ASSERT_TRUE(left.back() && right.back());
    }
    // Each level reads both knobs of the one before: 2^20 paths from the first level to the last.
    for (int level = 1; level <= kLevels; ++level) {
        const std::string previous = std::to_string(level - 1);
        ASSERT_NO_THROW(left[level]->setExpression(0, "L" + previous + ".offset.getValue(0) + R" + previous + ".offset.getValue(0)", false, true));
        ASSERT_NO_THROW(right[level]->setExpression(0, "L" + previous + ".offset.getValue(0) - R" + previous + ".offset.getValue(0)", false, true));
    }

    const U64 before = KnobHelper::getListenerRefreshCount();
    left[0]->setValue(1.);
    EXPECT_EQ((U64)(2 * kLevels), KnobHelper::getListenerRefreshCount() - before);

    // L1 = R1 = 1, then each level doubles L and zeroes R.
    EXPECT_DOUBLE_EQ(1., left[1]->getValue(0, ViewSpec::current(), false));
    EXPECT_DOUBLE_EQ(1., right[1]->getValue(0, ViewSpec::current(), false));
    EXPECT_DOUBLE_EQ(2., left[2]->getValue(0, ViewSpec::current(), false));
    EXPECT_DOUBLE_EQ(0., right[2]->getValue(0, ViewSpec::current(), false));
    EXPECT_DOUBLE_EQ(2., left[3]->getValue(0, ViewSpec::current(), false));
}

// Naming nodes in a batch, as a paste does, indexes the group once instead of scanning it per
// candidate name, and gives the same names as before.
TEST_F(GraphScalingGuiEdits, BatchedNamingScansOnce)
{
    const AppInstancePtr app = getApp();
    const int count = 60;

    const U64 unbatchedBefore = NodeCollection::getNodeNameScanCount();
    for (int i = 0; i < count; ++i) {
        ASSERT_TRUE(bool(createNamedNode(app, kGradePluginID, "Plain")));
    }
    EXPECT_GE(NodeCollection::getNodeNameScanCount() - unbatchedBefore, (U64)count);

    std::vector<NodePtr> batched;
    const U64 batchedBefore = NodeCollection::getNodeNameScanCount();
    {
        const NodeCollection::NamingBatch namingBatch(app->getProject());
        for (int i = 0; i < count; ++i) {
            NodePtr node = createNamedNode(app, kGradePluginID, "Same");
            ASSERT_TRUE(bool(node));
            batched.push_back(node);
        }
        for (int i = 0; i < count; ++i) {
            CreateNodeArgs args(kGradePluginID, app->getProject());
            NodePtr node = app->createNode(args);
            ASSERT_TRUE(bool(node));
            batched.push_back(node);
        }
    }
    EXPECT_LE(NodeCollection::getNodeNameScanCount() - batchedBefore, (U64)2);

    std::set<std::string> names;
    for (std::size_t i = 0; i < batched.size(); ++i) {
        EXPECT_TRUE(names.insert(batched[i]->getScriptName()).second) << batched[i]->getScriptName();
    }
    EXPECT_EQ(std::string("Same"), batched[0]->getScriptName());
    for (int i = 1; i < count; ++i) {
        EXPECT_EQ("Same_" + std::to_string(i + 1), batched[i]->getScriptName());
    }

    // A name given up inside a batch is found free again.
    {
        const NodeCollection::NamingBatch namingBatch(app->getProject());
        batched[4]->destroyNode(true, false);
        NodePtr node = createNamedNode(app, kGradePluginID, "Same");
        ASSERT_TRUE(bool(node));
        EXPECT_EQ(std::string("Same_5"), node->getScriptName());
    }
}

TEST_F(GraphScalingGuiEdits, SiblingReferenceEvaluatesAndFollowsTheSibling)
{
    NodePtr target = createWithMix(getApp(), "Target", 0.25);
    NodePtr reader = createNamedNode(getApp(), kGradePluginID, "Reader");
    ASSERT_TRUE(target && reader);
    KnobDoubleBasePtr offset = doubleKnob(reader, "offset");
    ASSERT_TRUE(bool(offset));
    ASSERT_NO_THROW(offset->setExpression(0, "Target.mix.get() * 2", false, true));
    EXPECT_TRUE(offset->isExpressionValid(0, 0));
    EXPECT_DOUBLE_EQ(0.5, readOffset(offset));

    std::set<NodePtr> deps;
    reader->getEffectInstance()->getAllExpressionDependenciesRecursive(deps);
    EXPECT_EQ(1u, deps.count(target));

    doubleKnob(target, "mix")->setValue(0.75, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(1.5, readOffset(offset));
}

TEST_F(GraphScalingGuiEdits, SiblingReachedByComputedNameStillResolves)
{
    NodePtr target = createWithMix(getApp(), "Target", 0.25);
    NodePtr reader = createNamedNode(getApp(), kGradePluginID, "Reader");
    ASSERT_TRUE(target && reader);
    KnobDoubleBasePtr offset = doubleKnob(reader, "offset");
    ASSERT_TRUE(bool(offset));

    const char* const expressions[] = {
        "eval('Tar' + 'get').mix.get()",
        "eval('Tar' + 'get.mix.get()')",
        "locals()['Target'].mix.get()",
        "vars()['Target'].mix.get()",
        "getattr(thisGroup, 'Tar' + 'get').mix.get()",
        "thisGroup.getNode('Tar' + 'get').getParam('mix').get()",
    };
    for (const char* expression : expressions) {
        SCOPED_TRACE(expression);
        ASSERT_NO_THROW(offset->setExpression(0, expression, false, true));
        EXPECT_TRUE(offset->isExpressionValid(0, 0));
        EXPECT_DOUBLE_EQ(0.25, readOffset(offset));
    }
}

TEST_F(GraphScalingGuiEdits, SiblingNameOnlyInAStringIsHarmless)
{
    NodePtr target = createWithMix(getApp(), "Target", 0.25);
    NodePtr reader = createNamedNode(getApp(), kGradePluginID, "Reader");
    ASSERT_TRUE(target && reader);
    KnobDoubleBasePtr offset = doubleKnob(reader, "offset");
    ASSERT_TRUE(bool(offset));

    ASSERT_NO_THROW(offset->setExpression(0, "float(len('Target'))", false, true));
    EXPECT_TRUE(offset->isExpressionValid(0, 0));
    EXPECT_DOUBLE_EQ(6., readOffset(offset));

    // A string naming a node that does not exist is not a reference to it.
    ASSERT_NO_THROW(offset->setExpression(0, "float(len('Missing'))", false, true));
    EXPECT_TRUE(offset->isExpressionValid(0, 0));
    EXPECT_DOUBLE_EQ(7., readOffset(offset));
}

TEST_F(GraphScalingGuiEdits, SiblingNameThatPrefixesAnotherIsNotConfused)
{
    NodePtr shortName = createWithMix(getApp(), "Blur1", 0.25);
    NodePtr longName = createWithMix(getApp(), "Blur10", 0.75);
    NodePtr reader = createNamedNode(getApp(), kGradePluginID, "Reader");
    ASSERT_TRUE(shortName && longName && reader);
    ASSERT_EQ(std::string("Blur1"), shortName->getScriptName_mt_safe());
    ASSERT_EQ(std::string("Blur10"), longName->getScriptName_mt_safe());
    KnobDoubleBasePtr offset = doubleKnob(reader, "offset");
    ASSERT_TRUE(bool(offset));

    ASSERT_NO_THROW(offset->setExpression(0, "Blur1.mix.get()", false, true));
    EXPECT_DOUBLE_EQ(0.25, readOffset(offset));
    ASSERT_NO_THROW(offset->setExpression(0, "Blur10.mix.get()", false, true));
    EXPECT_DOUBLE_EQ(0.75, readOffset(offset));

    // A longer name containing the shorter one does not declare the shorter one.
    ASSERT_NO_THROW(offset->setExpression(0, "Blur10.mix.get() + 0 * len('Blur1')", false, true));
    EXPECT_DOUBLE_EQ(0.75, readOffset(offset));
    offset->setExpression(0, "NotBlur1.mix.get()", false, false);
    EXPECT_FALSE(offset->isExpressionValid(0, 0));
}

TEST_F(GraphScalingGuiEdits, SiblingCreatedAfterTheExpressionResolves)
{
    NodePtr reader = createNamedNode(getApp(), kGradePluginID, "Reader");
    ASSERT_TRUE(bool(reader));
    KnobDoubleBasePtr offset = doubleKnob(reader, "offset");
    ASSERT_TRUE(bool(offset));
    ASSERT_NO_THROW(offset->setExpression(0, "Later.mix.get()", false, false));
    ASSERT_FALSE(offset->isExpressionValid(0, 0));

    NodePtr later = createWithMix(getApp(), "Later", 0.5);
    ASSERT_TRUE(bool(later));
    ASSERT_EQ(std::string("Later"), later->getScriptName_mt_safe());
    EXPECT_TRUE(offset->isExpressionValid(0, 0));
    EXPECT_DOUBLE_EQ(0.5, readOffset(offset));

    doubleKnob(later, "mix")->setValue(0.125, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.125, readOffset(offset));
}

TEST_F(GraphScalingGuiEdits, UndefinedSiblingStaysInvalidAmongOthers)
{
    NodePtr other = createNamedNode(getApp(), kGradePluginID, "Other");
    NodePtr reader = createNamedNode(getApp(), kGradePluginID, "Reader");
    ASSERT_TRUE(other && reader);
    KnobDoubleBasePtr offset = doubleKnob(reader, "offset");
    ASSERT_TRUE(bool(offset));

    ASSERT_NO_THROW(offset->setExpression(0, "Missing.mix.get()", false, false));
    EXPECT_FALSE(offset->isExpressionValid(0, 0));
    ASSERT_NO_THROW(offset->setExpression(0, "Other.mix.get() + Missing.mix.get()", false, false));
    EXPECT_FALSE(offset->isExpressionValid(0, 0));
}
