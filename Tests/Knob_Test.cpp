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
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QLocale>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

TEST(KnobLayers, ColumnTagsAreLocaleStableAndRoundTrip)
{
    KnobLayersPtr knob = std::make_shared<KnobLayers>(static_cast<KnobHolder*>(NULL), std::string("layers"), 1, false);

    EXPECT_EQ(std::string("Name"), knob->getColumnTag(0));
    EXPECT_EQ(std::string("Channels"), knob->getColumnTag(1));
    EXPECT_EQ(std::string("UsedBy"), knob->getColumnTag(2));

    std::list<std::vector<std::string>> rows;
    {
        std::vector<std::string> row;
        row.push_back("Color");
        row.push_back("R G B A");
        row.push_back("0");
        rows.push_back(row);
    }
    {
        std::vector<std::string> row;
        row.push_back("diffuse");
        row.push_back("R G B");
        row.push_back("1");
        rows.push_back(row);
    }

    QLocale savedLocale;
    QLocale::setDefault(QLocale(QLocale::French));
    std::string encoded = knob->encodeToKnobTableFormat(rows);
    QLocale::setDefault(savedLocale);

    EXPECT_NE(std::string::npos, encoded.find("<Name>Color</Name>"));
    EXPECT_NE(std::string::npos, encoded.find("<Channels>R G B A</Channels>"));
    EXPECT_NE(std::string::npos, encoded.find("<UsedBy>0</UsedBy>"));
    EXPECT_EQ(std::string::npos, encoded.find("<Layer>"));
    EXPECT_EQ(std::string::npos, encoded.find("Used by"));

    std::list<std::vector<std::string>> decoded;
    knob->decodeFromKnobTableFormat(encoded, &decoded);

    ASSERT_EQ(rows.size(), decoded.size());
    std::list<std::vector<std::string>>::const_iterator expectedIt = rows.begin();
    std::list<std::vector<std::string>>::const_iterator actualIt = decoded.begin();
    for (; expectedIt != rows.end(); ++expectedIt, ++actualIt) {
        EXPECT_EQ(*expectedIt, *actualIt);
    }
}

TEST(Knob, TableKnobRefusesExpressions)
{
    KnobLayersPtr knob = std::make_shared<KnobLayers>(static_cast<KnobHolder*>(NULL), std::string("layers"), 1, false);

    EXPECT_THROW(knob->setExpression(0, "1", false, true), std::invalid_argument);
}

TEST_F(BaseTest, ScalarKnobAcceptsExpressions)
{
    ProjectPtr project = getApp()->getProject();
    KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(project->getKnobByName("frameRate"));

    ASSERT_TRUE(bool(knob));
    EXPECT_NO_THROW(knob->setExpression(0, "1", false, true));
}

// One expression result serves clamped and unclamped reads, whichever comes first.
TEST_F(BaseTest, ExpressionResultOutsideTheRangeServesClampedAndUnclampedReads)
{
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
    ASSERT_TRUE(bool(size));
    ASSERT_EQ(0., size->getMinimum(0));
    ASSERT_EQ(1000., size->getMaximum(1));

    size->setExpression(0, "-5", false, true);
    size->setExpression(1, "2000", false, true);

    EXPECT_EQ(0., size->getValue(0, ViewSpec::current(), true));
    EXPECT_EQ(-5., size->getValue(0, ViewSpec::current(), false));
    EXPECT_EQ(0., size->getValue(0, ViewSpec::current(), true));

    EXPECT_EQ(2000., size->getValue(1, ViewSpec::current(), false));
    EXPECT_EQ(1000., size->getValue(1, ViewSpec::current(), true));
    EXPECT_EQ(2000., size->getValue(1, ViewSpec::current(), false));

    EXPECT_EQ(1000., size->getValueAtTime(3., 1, ViewSpec::current(), true));
    EXPECT_EQ(2000., size->getValueAtTime(3., 1, ViewSpec::current(), false));
}

TEST_F(BaseTest, DuplicateOnHolderAndAliasKnobLayers)
{
    NodePtr nodeA = createNode(_generatorPluginID);
    NodePtr nodeB = createNode(_generatorPluginID);

    ASSERT_TRUE(bool(nodeA));
    ASSERT_TRUE(bool(nodeB));

    KnobLayersPtr source = AppManager::createKnob<KnobLayers>(nodeA->getEffectInstance().get(), std::string("layers"), 1, false);
    ASSERT_TRUE(bool(source));
    source->setName("layers");

    std::list<std::vector<std::string>> rows;
    std::vector<std::string> row;
    row.push_back("testLayer");
    row.push_back("R G B");
    row.push_back("0");
    rows.push_back(row);
    source->setTable(rows);

    KnobIPtr duplicate = source->createDuplicateOnHolder(nodeB->getEffectInstance().get(),
                                                         KnobPagePtr(),
                                                         KnobGroupPtr(),
                                                         -1,
                                                         false,
                                                         "layersDuplicate",
                                                         "Layers Duplicate",
                                                         "",
                                                         false,
                                                         true);

    ASSERT_TRUE(bool(duplicate));
    KnobIPtr sourceAsKnobI = source;
    EXPECT_EQ(sourceAsKnobI->typeName(), duplicate->typeName());

    KnobLayersPtr duplicateLayers = std::dynamic_pointer_cast<KnobLayers>(duplicate);
    ASSERT_TRUE(bool(duplicateLayers));
    EXPECT_EQ(source->getValue(), duplicateLayers->getValue());

    EXPECT_TRUE(source->setKnobAsAliasOfThis(duplicate, true));
}

TEST_F(BaseTest, DuplicateOnHolderKeepsStringKnobPreferredRows)
{
    NodePtr nodeA = createNode(_generatorPluginID);
    NodePtr nodeB = createNode(_generatorPluginID);

    ASSERT_TRUE(bool(nodeA));
    ASSERT_TRUE(bool(nodeB));

    KnobStringPtr source = AppManager::createKnob<KnobString>(nodeA->getEffectInstance().get(), std::string("notes"), 1, false);
    ASSERT_TRUE(bool(source));
    source->setName("notes");
    source->setAsMultiLine();
    source->setPreferredRows(20);

    KnobIPtr duplicate = source->createDuplicateOnHolder(nodeB->getEffectInstance().get(),
                                                         KnobPagePtr(),
                                                         KnobGroupPtr(),
                                                         -1,
                                                         false,
                                                         "notesDuplicate",
                                                         "Notes Duplicate",
                                                         "",
                                                         false,
                                                         true);

    ASSERT_TRUE(bool(duplicate));
    KnobStringPtr duplicateString = std::dynamic_pointer_cast<KnobString>(duplicate);
    ASSERT_TRUE(bool(duplicateString));
    EXPECT_TRUE(duplicateString->isMultiLine());
    EXPECT_EQ(20, duplicateString->getPreferredRows());
}

namespace {
const char* const kCascadeGradeID = "Grade";

NodePtr
createCascadeNode(const AppInstancePtr& app,
                  const char* pluginID,
                  const std::string& name = std::string())
{
    CreateNodeArgs args(pluginID, app->getProject());

    if (!name.empty()) {
        args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);
    }

    return app->createNode(args);
}

// The disable switch is a metadata knob, so a change reaching it also refreshes the input-related
// data downstream.
void
disableFollows(const NodePtr& node,
               const std::string& sourceName)
{
    KnobIPtr disable = node->getKnobByName(kDisableNodeKnobName);
    ASSERT_TRUE(bool(disable));
    ASSERT_NO_THROW(disable->setExpression(0, "int(" + sourceName + ".offset.getValue(0) > 1000)", false, true));
}

KnobDoubleBasePtr
offsetOf(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobDoubleBase>(node->getKnobByName("offset"));
}
} // anonymous namespace

class KnobValueChangeCascade
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

TEST_F(KnobValueChangeCascade, SingleChangeRefreshesDownstreamBeforeReturning)
{
    NodePtr source = createCascadeNode(getApp(), kCascadeGradeID, "Source");
    NodePtr first = createCascadeNode(getApp(), kCascadeGradeID, "First");
    NodePtr second = createCascadeNode(getApp(), kCascadeGradeID, "Second");
    ASSERT_TRUE(source && first && second);
    connectNodes(source, first, 0, true);
    connectNodes(first, second, 0, true);
    source->forceRefreshAllInputRelatedData();
    ASSERT_FALSE(first->isInputRelatedDataDirty());
    ASSERT_FALSE(second->isInputRelatedDataDirty());

    KnobBoolPtr disable = std::dynamic_pointer_cast<KnobBool>(source->getKnobByName(kDisableNodeKnobName));
    ASSERT_TRUE(bool(disable));
    const U64 before = Node::getDownstreamWalkVisitCount();
    disable->setValue(true);
    const U64 visits = Node::getDownstreamWalkVisitCount() - before;

    EXPECT_TRUE(first->isInputRelatedDataDirty());
    EXPECT_TRUE(second->isInputRelatedDataDirty());
    // Aborting, refreshing and finding the viewers each walk the three nodes at most once.
    EXPECT_LE(visits, (U64)9);
}

TEST_F(KnobValueChangeCascade, RefreshesDownstreamOfEveryDependent)
{
    NodePtr source = createCascadeNode(getApp(), kCascadeGradeID, "Source");
    NodePtr dependentA = createCascadeNode(getApp(), kCascadeGradeID, "DependentA");
    NodePtr dependentB = createCascadeNode(getApp(), kCascadeGradeID, "DependentB");
    NodePtr tailA = createCascadeNode(getApp(), kCascadeGradeID, "TailA");
    NodePtr tailB = createCascadeNode(getApp(), kCascadeGradeID, "TailB");
    ASSERT_TRUE(source && dependentA && dependentB && tailA && tailB);
    disableFollows(dependentA, "Source");
    disableFollows(dependentB, "Source");
    connectNodes(dependentA, tailA, 0, true);
    connectNodes(dependentB, tailB, 0, true);
    dependentA->forceRefreshAllInputRelatedData();
    dependentB->forceRefreshAllInputRelatedData();
    ASSERT_FALSE(tailA->isInputRelatedDataDirty());
    ASSERT_FALSE(tailB->isInputRelatedDataDirty());

    KnobDoubleBasePtr offset = offsetOf(source);
    ASSERT_TRUE(bool(offset));
    offset->setValue(1.);

    EXPECT_TRUE(tailA->isInputRelatedDataDirty());
    EXPECT_TRUE(tailB->isInputRelatedDataDirty());
}

TEST_F(KnobValueChangeCascade, FanOutWalksEachNodeOncePerWalk)
{
    const int kDependents = 50;
    const int kChain = 3000;

    NodePtr source = createCascadeNode(getApp(), kCascadeGradeID, "Source");
    ASSERT_TRUE(bool(source));
    std::vector<NodePtr> chain(1, source);
    {
        // Unbatched, finding each Dot a free numbered name scans every node per number tried.
        const NodeCollection::NamingBatch namingBatch(getApp()->getProject());
        for (int i = 0; i < kDependents; ++i) {
            NodePtr dependent = createCascadeNode(getApp(), kCascadeGradeID, "Dependent" + std::to_string(i));
            ASSERT_TRUE(bool(dependent));
            disableFollows(dependent, "Source");
            chain.push_back(dependent);
        }
        while ((int)chain.size() < kChain) {
            NodePtr dot = createCascadeNode(getApp(), PLUGINID_NATRON_DOT);
            ASSERT_TRUE(bool(dot));
            chain.push_back(dot);
        }
    }
    for (std::size_t i = 1; i < chain.size(); ++i) {
        connectNodes(chain[i - 1], chain[i], 0, true);
    }

    KnobDoubleBasePtr offset = offsetOf(source);
    ASSERT_TRUE(bool(offset));
    const U64 refreshesBefore = KnobHelper::getListenerRefreshCount();
    const U64 before = Node::getDownstreamWalkVisitCount();
    offset->setValue(1.);
    const U64 visits = Node::getDownstreamWalkVisitCount() - before;

    EXPECT_EQ((U64)kDependents, KnobHelper::getListenerRefreshCount() - refreshesBefore);
    // Aborting, refreshing and finding the viewers each visit a node at most once for the whole
    // change, where a walk per dependent would visit about kDependents times as many.
    EXPECT_LE(visits, (U64)(3 * kChain));
    EXPECT_TRUE(chain.back()->isInputRelatedDataDirty());
}
