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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDExpandHierarchy.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/ScenePayload.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoRecorder.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
void
writeFile(const std::string& path,
          const std::string& content)
{
    QFile f(QString::fromStdString(path));

    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content.c_str(), (qint64)content.size());
}

const char* const kBuilding = "#usda 1.0\n"
                              "\n"
                              "def Xform \"Building\"\n"
                              "{\n"
                              "    def Cube \"Body\"\n"
                              "    {\n"
                              "    }\n"
                              "}\n";

// /World and /Annex, each holding a building B0 whose content is a payload of building.usda.
const char* const kCity = "#usda 1.0\n"
                          "\n"
                          "def Xform \"World\"\n"
                          "{\n"
                          "    def Xform \"B0\" (\n"
                          "        payload = @./building.usda@</Building>\n"
                          "    )\n"
                          "    {\n"
                          "    }\n"
                          "}\n"
                          "\n"
                          "def Xform \"Annex\"\n"
                          "{\n"
                          "    def Xform \"B0\" (\n"
                          "        payload = @./building.usda@</Building>\n"
                          "    )\n"
                          "    {\n"
                          "    }\n"
                          "}\n";

USDExpandHierarchy::Rule
makeRule(const std::string& pattern,
         SceneDrawStateEnum state,
         int levels = 0)
{
    USDExpandHierarchy::Rule rule;

    rule.pattern = pattern;
    rule.state = state;
    rule.levels = levels;

    return rule;
}

// Keeps what is recorded, the way the window's history would within one step.
class CollectingRecorder
    : public UndoRecorderI {
public:
    virtual void openStep(const std::string& /*text*/) OVERRIDE FINAL
    {
        if (depth++ == 0) {
            ++serial;
        }
    }

    virtual void closeStep() OVERRIDE FINAL
    {
        if (depth > 0) {
            --depth;
        }
    }

    virtual bool isCapturing() const OVERRIDE FINAL
    {
        return depth > 0;
    }

    virtual std::uint64_t getCaptureSerial() const OVERRIDE FINAL
    {
        return serial;
    }

    virtual void record(const UndoCommandPtr& command,
                        const UndoTouched& /*touched*/) OVERRIDE FINAL
    {
        commands.push_back(command);
    }

    int depth = 0;
    std::uint64_t serial = 0;
    std::vector<UndoCommandPtr> commands;
};
} // anonymous namespace

class USDExpandHierarchyTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        // The city is a centimetre scene by USD's fallback: a project in the same units reads it
        // without a units warning.
        KnobDouble* metersPerUnit = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("metersPerUnit").get());
        ASSERT_TRUE(metersPerUnit != NULL);
        metersPerUnit->setValue(0.01);
        ASSERT_TRUE(_dir.isValid());
        writeFile(_dir.filePath(QString::fromUtf8("building.usda")).toStdString(), kBuilding);
        _cityPath = _dir.filePath(QString::fromUtf8("city.usda")).toStdString();
        writeFile(_cityPath, kCity);
    }

    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    NodePtr createReader()
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));

        EXPECT_TRUE(bool(reader));
        if (!reader) {
            return reader;
        }
        KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
        EXPECT_TRUE(file != NULL);
        if (file) {
            file->setValue(_cityPath);
        }

        return reader;
    }

    NodePtr createExpand(const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEXPANDHIERARCHY));

        EXPECT_TRUE(bool(node));
        if (node && input) {
            EXPECT_TRUE(node->connectInput(input, 0));
        }

        return node;
    }

    static USDExpandHierarchy* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<USDExpandHierarchy*>(node->getEffectInstance().get()) : NULL;
    }

    static ScenePayload evaluate(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return payload;
    }

    PXR_NS::UsdStageRefPtr openCity(PXR_NS::UsdStage::InitialLoadSet load) const
    {
        return PXR_NS::UsdStage::Open(_cityPath, load);
    }

    QTemporaryDir _dir;
    std::string _cityPath;
};

// The node authors no layer: the stack and its hash are its input's, whatever the rules say,
// and editing them leaves the node's hash alone.
TEST_F(USDExpandHierarchyTest, TheStackThroughTheNodeIsItsInputs)
{
    NodePtr reader = createReader();
    NodePtr expand = createExpand(reader);
    USDExpandHierarchy* effect = effectOf(expand);
    ASSERT_TRUE(reader && expand && effect);

    const ScenePayload input = evaluate(reader);
    ASSERT_FALSE(input.isEmpty());
    const U64 nodeHash = expand->getHashValue();

    ScenePayload output = evaluate(expand);
    EXPECT_EQ(input.getStackHash(), output.getStackHash());
    EXPECT_TRUE(input.isSameStack(output));
    EXPECT_EQ(input.getDrawRules(), output.getDrawRules());

    effect->appendRules({ makeRule("/World", eSceneDrawStateLoadAll), makeRule("/Annex", eSceneDrawStateHidden, 2) });
    output = evaluate(expand);
    EXPECT_EQ(input.getStackHash(), output.getStackHash());
    EXPECT_TRUE(input.isSameStack(output));
    EXPECT_EQ(nodeHash, expand->getHashValue());
    EXPECT_EQ(input.getDrawRules().size() + 2, output.getDrawRules().size());
}

// The node's rules follow its input's, in row order, named after the node; a blank pattern sets
// none.
TEST_F(USDExpandHierarchyTest, RulesAppendInOrderAfterTheInputs)
{
    NodePtr reader = createReader();
    NodePtr expand = createExpand(reader);
    USDExpandHierarchy* effect = effectOf(expand);
    ASSERT_TRUE(reader && expand && effect);

    const SceneDrawRules inputRules = evaluate(reader).getDrawRules();
    ASSERT_EQ((std::size_t)1, inputRules.size());
    EXPECT_EQ(reader->getScriptName(), inputRules[0].origin);

    effect->appendRules({ makeRule("/World", eSceneDrawStateLoadAll), makeRule("  ", eSceneDrawStateBBox), makeRule("/Annex", eSceneDrawStateChildren, 1) });
    const SceneDrawRules rules = evaluate(expand).getDrawRules();
    ASSERT_EQ((std::size_t)3, rules.size());
    EXPECT_EQ(inputRules[0], rules[0]);
    EXPECT_EQ(std::string("/World"), rules[1].pattern);
    EXPECT_EQ(eSceneDrawStateLoadAll, rules[1].state);
    EXPECT_EQ(0, rules[1].levels);
    EXPECT_EQ(expand->getScriptName(), rules[1].origin);
    EXPECT_EQ(std::string("/Annex"), rules[2].pattern);
    EXPECT_EQ(eSceneDrawStateChildren, rules[2].state);
    EXPECT_EQ(1, rules[2].levels);
    EXPECT_EQ(expand->getScriptName(), rules[2].origin);

    // A later node's rules come after this one's.
    NodePtr below = createExpand(expand);
    ASSERT_TRUE(bool(below));
    effectOf(below)->appendRules({ makeRule("/World", eSceneDrawStateBBox) });
    const SceneDrawRules chained = evaluate(below).getDrawRules();
    ASSERT_EQ((std::size_t)4, chained.size());
    EXPECT_EQ(below->getScriptName(), chained[3].origin);

    SceneDrawStates::PolicyStates policy;
    ASSERT_TRUE(SceneOps::matchDrawRules(openCity(PXR_NS::UsdStage::LoadNone), chained, &policy, std::function<bool()>()));
    EXPECT_EQ(eSceneDrawStateBBox, policy["/World"].state);
    EXPECT_EQ(below->getScriptName(), policy["/World"].origin);
}

// Appending writes after the last row with a pattern, creating rows past the first chunk, and
// Rule Count hides the rows past it, which then set no rule.
TEST_F(USDExpandHierarchyTest, RowsGrowAndRuleCountHidesTheRest)
{
    NodePtr expand = createExpand(NodePtr());
    USDExpandHierarchy* effect = effectOf(expand);
    ASSERT_TRUE(expand && effect);

    const std::string first = std::string(kUSDExpandHierarchyParamPattern) + "0";
    ASSERT_TRUE(bool(expand->getKnobByName(first)));
    const std::string past = std::string(kUSDExpandHierarchyParamPattern) + std::to_string(kUSDExpandHierarchyRuleChunk);
    EXPECT_FALSE(bool(expand->getKnobByName(past)));
    EXPECT_TRUE(expand->getKnobByName(std::string(kUSDExpandHierarchyParamPattern) + "1")->getIsSecret());

    std::vector<USDExpandHierarchy::Rule> many;
    for (int i = 0; i <= kUSDExpandHierarchyRuleChunk; ++i) {
        many.push_back(makeRule("/P" + std::to_string(i), eSceneDrawStateFull));
    }
    effect->appendRules(many);
    ASSERT_TRUE(bool(expand->getKnobByName(past)));
    EXPECT_FALSE(expand->getKnobByName(past)->getIsSecret());
    ASSERT_EQ(many.size(), effect->getRules().size());
    EXPECT_EQ(std::string("/P0"), effect->getRules()[0].pattern);
    EXPECT_EQ(many.size(), effect->getDrawRules().size());

    effect->appendRules({ makeRule("/Last", eSceneDrawStateHidden) });
    ASSERT_EQ(many.size() + 1, effect->getRules().size());
    EXPECT_EQ(std::string("/Last"), effect->getRules().back().pattern);

    KnobInt* count = dynamic_cast<KnobInt*>(expand->getKnobByName(kUSDExpandHierarchyParamRuleCount).get());
    ASSERT_TRUE(count != NULL);
    count->setValue(1);
    EXPECT_TRUE(expand->getKnobByName(past)->getIsSecret());
    ASSERT_EQ((std::size_t)1, effect->getDrawRules().size());
    EXPECT_EQ(std::string("/P0"), effect->getDrawRules()[0].pattern);
}

// A bake turns a viewer's states into as few rules as keep their order: a path's rule before its
// ancestors', and one rule for consecutive paths of a state that hold no ancestor of each other.
TEST(USDExpandHierarchyRulesForStates, PathsShareARuleUntilAnAncestorOrAnotherState)
{
    const std::vector<std::pair<std::string, SceneDrawStateEnum>> states = {
        { "/A", eSceneDrawStateFull },
        { "/A/x", eSceneDrawStateHidden },
        { "/A/y", eSceneDrawStateHidden },
        { "/AB", eSceneDrawStateHidden },
        { "/C", eSceneDrawStateFull },
        { "/C/d", eSceneDrawStateFull },
    };
    const std::vector<USDExpandHierarchy::Rule> rules = USDExpandHierarchy::getRulesForStates(states);

    ASSERT_EQ((std::size_t)4, rules.size());
    EXPECT_EQ(std::string("/C/d"), rules[0].pattern);
    EXPECT_EQ(eSceneDrawStateFull, rules[0].state);
    EXPECT_EQ(std::string("/C"), rules[1].pattern);
    EXPECT_EQ(eSceneDrawStateFull, rules[1].state);
    EXPECT_EQ(std::string("/AB /A/y /A/x"), rules[2].pattern);
    EXPECT_EQ(eSceneDrawStateHidden, rules[2].state);
    EXPECT_EQ(std::string("/A"), rules[3].pattern);
    EXPECT_EQ(eSceneDrawStateFull, rules[3].state);
    for (const USDExpandHierarchy::Rule& rule : rules) {
        EXPECT_EQ(0, rule.levels);
    }

    const std::vector<USDExpandHierarchy::Rule> underRoot = USDExpandHierarchy::getRulesForStates({ { "/", eSceneDrawStateBBox }, { "/World", eSceneDrawStateBBox } });
    ASSERT_EQ((std::size_t)2, underRoot.size());
    EXPECT_EQ(std::string("/World"), underRoot[0].pattern);
    EXPECT_EQ(std::string("/"), underRoot[1].pattern);

    EXPECT_TRUE(USDExpandHierarchy::getRulesForStates({}).empty());
}

// More states than rows fit in one bake once those of a state share rules.
TEST(USDExpandHierarchyRulesForStates, UnrelatedPathsOfAStateTakeOneRule)
{
    std::vector<std::pair<std::string, SceneDrawStateEnum>> states;
    for (int i = 0; i < 3 * kUSDExpandHierarchyMaxRules; ++i) {
        states.push_back(std::make_pair("/P" + std::to_string(100000 + i), eSceneDrawStateHidden));
    }
    const std::vector<USDExpandHierarchy::Rule> rules = USDExpandHierarchy::getRulesForStates(states);

    ASSERT_EQ((std::size_t)1, rules.size());
    EXPECT_EQ(0u, rules[0].pattern.find("/P" + std::to_string(100000 + 3 * kUSDExpandHierarchyMaxRules - 1) + " "));
}

// Bake writes its rules through appendRules inside one undo step: undoing the step takes back the
// rows, the rows it had to create included, and the count; redoing it puts them back.
TEST_F(USDExpandHierarchyTest, AppendingRulesInAStepUndoesAsOne)
{
    NodePtr expand = createExpand(NodePtr());
    USDExpandHierarchy* effect = effectOf(expand);
    ASSERT_TRUE(expand && effect);
    effect->appendRules({ makeRule("/Kept", eSceneDrawStateBBox) });
    const int roomBefore = effect->getFreeRuleCount();
    const std::vector<USDExpandHierarchy::Rule> before = effect->getRules();

    std::vector<USDExpandHierarchy::Rule> many;
    for (int i = 0; i <= kUSDExpandHierarchyRuleChunk; ++i) {
        many.push_back(makeRule("/P" + std::to_string(i), eSceneDrawStateHidden, i % 3));
    }
    std::shared_ptr<CollectingRecorder> recorder = std::make_shared<CollectingRecorder>();
    getApp()->setUndoRecorder(recorder);
    recorder->openStep("Bake to Node");
    effect->appendRules(many);
    recorder->closeStep();
    ASSERT_FALSE(recorder->commands.empty());
    EXPECT_EQ(roomBefore - (int)many.size(), effect->getFreeRuleCount());
    const std::vector<USDExpandHierarchy::Rule> after = effect->getRules();
    ASSERT_EQ(before.size() + many.size(), after.size());

    const auto expectRules = [](const std::vector<USDExpandHierarchy::Rule>& expected,
                                const std::vector<USDExpandHierarchy::Rule>& actual,
                                const char* when) {
        ASSERT_EQ(expected.size(), actual.size()) << when;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(expected[i].pattern, actual[i].pattern) << when << " " << i;
            EXPECT_EQ(expected[i].state, actual[i].state) << when << " " << i;
            EXPECT_EQ(expected[i].levels, actual[i].levels) << when << " " << i;
        }
    };
    {
        UndoReplayGuard replaying(getApp());
        for (std::vector<UndoCommandPtr>::reverse_iterator it = recorder->commands.rbegin(); it != recorder->commands.rend(); ++it) {
            (*it)->undo();
        }
    }
    expectRules(before, effect->getRules(), "undone");
    EXPECT_EQ(roomBefore, effect->getFreeRuleCount());
    {
        UndoReplayGuard replaying(getApp());
        for (std::vector<UndoCommandPtr>::iterator it = recorder->commands.begin(); it != recorder->commands.end(); ++it) {
            (*it)->redo();
        }
    }
    expectRules(after, effect->getRules(), "redone");

    recorder->commands.clear();
    getApp()->setUndoRecorder(UndoRecorderPtr());
}

// The same rules decide differently in two viewers: a pattern matches the prims each has loaded,
// and a viewer's own state beats them.
TEST_F(USDExpandHierarchyTest, EachViewerResolvesTheRulesItself)
{
    NodePtr reader = createReader();
    NodePtr expand = createExpand(reader);
    USDExpandHierarchy* effect = effectOf(expand);
    ASSERT_TRUE(reader && expand && effect);
    effect->appendRules({ makeRule("/World", eSceneDrawStateLoadAll), makeRule("//Body", eSceneDrawStateHidden) });
    const SceneDrawRules rules = evaluate(expand).getDrawRules();
    ASSERT_EQ((std::size_t)3, rules.size());

    SceneDrawStates::PolicyStates unloaded;
    ASSERT_TRUE(SceneOps::matchDrawRules(openCity(PXR_NS::UsdStage::LoadNone), rules, &unloaded, std::function<bool()>()));
    SceneDrawStates::PolicyStates loaded;
    ASSERT_TRUE(SceneOps::matchDrawRules(openCity(PXR_NS::UsdStage::LoadAll), rules, &loaded, std::function<bool()>()));
    EXPECT_EQ(0u, unloaded.count("/World/B0/Body"));
    ASSERT_EQ(1u, loaded.count("/World/B0/Body"));
    EXPECT_EQ(eSceneDrawStateHidden, loaded["/World/B0/Body"].state);
    EXPECT_EQ(eSceneDrawStateLoadAll, unloaded["/World"].state);
    EXPECT_EQ(eSceneDrawStateBBox, unloaded["/Annex"].state);
    EXPECT_EQ(reader->getScriptName(), unloaded["/Annex"].origin);

    SceneDrawStates first;
    SceneDrawStates second;
    first.replacePolicyStates(unloaded);
    second.replacePolicyStates(unloaded);
    second.setState("/World", eSceneDrawStateBBox);
    const SceneDrawResolved fromGraph = first.resolve("/World");
    const SceneDrawResolved fromViewer = second.resolve("/World");
    EXPECT_EQ(eSceneDrawStateLoadAll, fromGraph.state);
    EXPECT_EQ(eSceneDrawOriginGraphPolicy, fromGraph.origin);
    EXPECT_EQ(eSceneDrawStateBBox, fromViewer.state);
    EXPECT_EQ(eSceneDrawOriginViewer, fromViewer.origin);
    EXPECT_FALSE(first.wantedLoadsUnder("/World").empty());
    EXPECT_TRUE(second.wantedLoadsUnder("/World").empty());
}

// A pattern that does not parse sets no rule in the viewers, which is no error.
TEST_F(USDExpandHierarchyTest, ABadPatternSetsNoRuleAndKeepsTheScene)
{
    NodePtr reader = createReader();
    NodePtr expand = createExpand(reader);
    USDExpandHierarchy* effect = effectOf(expand);
    ASSERT_TRUE(reader && expand && effect);
    effect->appendRules({ makeRule("/World/Box &", eSceneDrawStateHidden), makeRule("/Annex", eSceneDrawStateFull) });

    const ScenePayload input = evaluate(reader);
    const ScenePayload output = evaluate(expand);
    EXPECT_TRUE(input.isSameStack(output));
    QString message;
    int type = 0;
    expand->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/Box &"))) << message.toStdString();

    SceneDrawStates::PolicyStates policy;
    ASSERT_TRUE(SceneOps::matchDrawRules(openCity(PXR_NS::UsdStage::LoadNone), output.getDrawRules(), &policy, std::function<bool()>()));
    EXPECT_EQ(eSceneDrawStateFull, policy["/Annex"].state);
    EXPECT_EQ(reader->getScriptName(), policy["/World"].origin);
}

// Undo and redo put a rule's Draw back through a clone of a detached knob, which has no entries,
// and then announce the change for every dimension. The rule the viewers get has to be the entry
// the restored index names, as Python and a saved project read it.
TEST_F(USDExpandHierarchyTest, UndoAndRedoKeepTheDrawTheIndexNames)
{
    NodePtr reader = createReader();
    NodePtr expand = createExpand(reader);
    USDExpandHierarchy* effect = effectOf(expand);
    ASSERT_TRUE(reader && expand && effect);

    KnobIntPtr count = std::dynamic_pointer_cast<KnobInt>(expand->getKnobByName(kUSDExpandHierarchyParamRuleCount));
    KnobStringPtr pattern = std::dynamic_pointer_cast<KnobString>(expand->getKnobByName(std::string(kUSDExpandHierarchyParamPattern) + "0"));
    KnobChoicePtr draw = std::dynamic_pointer_cast<KnobChoice>(expand->getKnobByName(std::string(kUSDExpandHierarchyParamDraw) + "0"));
    ASSERT_TRUE(count && pattern && draw);
    const std::vector<ChoiceOption> entries = draw->getEntries_mt_safe();

    std::shared_ptr<CollectingRecorder> recorder = std::make_shared<CollectingRecorder>();
    getApp()->setUndoRecorder(recorder);
    recorder->openStep("Bake to Node");
    recordKnobEdit(count);
    recordKnobEdit(pattern);
    recordKnobEdit(draw);
    effect->beginChanges();
    pattern->setValue(std::string("/Annex"));
    draw->setValueFromID(sceneDrawStateName(eSceneDrawStateHidden), 0);
    effect->endChanges();
    recorder->closeStep();
    ASSERT_EQ((std::size_t)3, recorder->commands.size());

    // What a viewer is given and what Python reads, for Draw 0.
    const auto expectDraw = [&](SceneDrawStateEnum state,
                                const char* when) {
        const int index = draw->getValue();
        ASSERT_TRUE((index >= 0) && (index < (int)entries.size())) << when;
        EXPECT_EQ(std::string(sceneDrawStateName(state)), entries[index].id) << when;
        EXPECT_EQ(entries[index].id, draw->getActiveEntry().id) << when;
        ASSERT_FALSE(effect->getRules().empty()) << when;
        EXPECT_EQ(state, effect->getRules()[0].state) << when;
    };
    expectDraw(eSceneDrawStateHidden, "baked");

    {
        UndoReplayGuard replaying(getApp());
        for (std::vector<UndoCommandPtr>::reverse_iterator it = recorder->commands.rbegin(); it != recorder->commands.rend(); ++it) {
            (*it)->undo();
        }
    }
    expectDraw(eSceneDrawStateFull, "undone");

    int announced = 0;
    QObject::connect(draw->getSignalSlotHandler().get(), &KnobSignalSlotHandler::valueChanged, [&announced](ViewSpec, int, int) { ++announced; });
    {
        UndoReplayGuard replaying(getApp());
        for (std::vector<UndoCommandPtr>::iterator it = recorder->commands.begin(); it != recorder->commands.end(); ++it) {
            (*it)->redo();
        }
    }
    expectDraw(eSceneDrawStateHidden, "redone");
    EXPECT_GT(announced, 0);

    const SceneDrawRules rules = evaluate(expand).getDrawRules();
    ASSERT_FALSE(rules.empty());
    EXPECT_EQ(std::string("/Annex"), rules.back().pattern);
    EXPECT_EQ(eSceneDrawStateHidden, rules.back().state);
    EXPECT_EQ(expand->getScriptName(), rules.back().origin);

    recorder->commands.clear();
    getApp()->setUndoRecorder(UndoRecorderPtr());
}
