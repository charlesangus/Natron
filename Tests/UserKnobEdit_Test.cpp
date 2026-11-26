/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include <Python.h>

#include "Global/Macros.h"

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/Knob.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobLinkGroup.h"
#include "Engine/KnobTypes.h"
#include "Engine/LogEntry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoRecorder.h"
#include "Engine/UserKnobEdit.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

class StepRecorder
    : public UndoRecorderI {
public:
    StepRecorder()
        : UndoRecorderI()
        , depth(0)
        , serial(0)
        , stepTexts()
        , commands()
    {
    }

    virtual void openStep(const std::string& text) OVERRIDE FINAL
    {
        if (depth++ == 0) {
            ++serial;
            stepTexts.push_back(text);
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

    int userKnobCommands() const
    {
        int count = 0;
        for (const UndoCommandPtr& command : commands) {
            if (command->getText().rfind("Edit parameters of ", 0) == 0) {
                ++count;
            }
        }

        return count;
    }

    int depth;
    std::uint64_t serial;
    std::vector<std::string> stepTexts;
    std::vector<UndoCommandPtr> commands;
};

class UserKnobEdit
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        recorder = std::make_shared<StepRecorder>();
        getApp()->setUndoRecorder(recorder);
    }

    virtual void TearDown() OVERRIDE
    {
        stopRecordingGraph();
        recorder->commands.clear();
        getApp()->setUndoRecorder(UndoRecorderPtr());
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createBlur()
    {
        CreateNodeArgs args(PLUGINID_NATRON_BLUR, getApp()->getProject());
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        NodePtr node = getApp()->createNode(args);
        recorder->commands.clear();

        return node;
    }

    static KnobHolder* holderOf(const NodePtr& node)
    {
        return node->getEffectInstance().get();
    }

    static UserKnobSpec makeSpec(const std::string& typeName,
                                 const std::string& name,
                                 int dimension = 1,
                                 const std::string& parentName = std::string())
    {
        UserKnobSpec spec;
        spec.typeName = typeName;
        spec.dimension = dimension;
        spec.name = name;
        spec.label = name;
        spec.parentName = parentName;

        return spec;
    }

    static KnobIPtr add(const NodePtr& node,
                        const UserKnobSpec& spec)
    {
        std::string error;
        KnobIPtr knob = createUserKnob(holderOf(node), spec, &error);
        EXPECT_TRUE(knob != nullptr) << spec.name << ": " << error;

        return knob;
    }

    template <typename K>
    static std::shared_ptr<K> addTyped(const NodePtr& node,
                                       const std::string& name,
                                       int dimension = 1,
                                       const std::string& parentName = std::string())
    {
        return std::dynamic_pointer_cast<K>(add(node, makeSpec(K::typeNameStatic(), name, dimension, parentName)));
    }

    static double keyValue(const KnobIPtr& knob,
                           int dimension,
                           int index)
    {
        CurvePtr curve = knob->getCurve(ViewIdx(0), dimension);
        KeyFrame key;
        EXPECT_TRUE(bool(curve && curve->getKeyFrameWithIndex(index, &key)));

        return key.getValue();
    }

    static int keyCount(const KnobIPtr& knob,
                        int dimension)
    {
        CurvePtr curve = knob->getCurve(ViewIdx(0), dimension);

        return curve ? curve->getKeyFramesCount() : 0;
    }

    static bool isLinkedTo(const KnobIPtr& knob,
                           const KnobIPtr& other)
    {
        for (const KnobLinkedDimension& linked : knob->getLinkedKnobs()) {
            if (linked.knob == other) {
                return true;
            }
        }

        return false;
    }

    static std::string differences(const UserKnobSpec& a,
                                   const UserKnobSpec& b)
    {
        std::string ret;
        const auto check = [&](bool same, const char* field) {
            if (!same) {
                ret += std::string(ret.empty() ? "" : ", ") + field;
            }
        };
        check(a.typeName == b.typeName, "typeName");
        check(a.dimension == b.dimension, "dimension");
        check(a.name == b.name, "name");
        check(a.label == b.label, "label");
        check(a.tooltip == b.tooltip, "tooltip");
        check(a.secret == b.secret, "secret");
        check(a.newLine == b.newLine, "newLine");
        check(a.animates == b.animates, "animates");
        check(a.evaluatesOnChange == b.evaluatesOnChange, "evaluatesOnChange");
        check(a.parentName == b.parentName, "parentName");
        check(a.index == b.index, "index");
        check(a.defaults == b.defaults, "defaults");
        check(a.extra == b.extra, "extra");
        check(a.groupAsTab == b.groupAsTab, "groupAsTab");
        check(a.useHostOverlayHandle == b.useHostOverlayHandle, "useHostOverlayHandle");

        return ret;
    }

    // The user knobs of node as copy text, which holds their values, curves, expressions and links.
    static std::string userKnobState(const NodePtr& node)
    {
        KnobHolder* holder = holderOf(node);
        std::list<KnobPage*> pages;
        holder->getUserPages(pages);
        std::list<KnobIPtr> knobs;
        for (KnobPage* page : pages) {
            knobs.push_back(holder->getKnobByName(page->getName()));
        }

        return copyUserKnobs(holder, knobs);
    }

    bool run(const std::string& script)
    {
        std::string error;
        std::string output;
        const bool ok = interpretPythonScript(script, &error, &output);
        if (!ok) {
            ADD_FAILURE() << script << "\n"
                          << error;
        }

        return ok;
    }

    void undoAll()
    {
        UndoReplayGuard replaying(getApp());
        for (std::vector<UndoCommandPtr>::reverse_iterator it = recorder->commands.rbegin(); it != recorder->commands.rend(); ++it) {
            (*it)->undo();
        }
    }

    void redoAll()
    {
        UndoReplayGuard replaying(getApp());
        for (std::vector<UndoCommandPtr>::iterator it = recorder->commands.begin(); it != recorder->commands.end(); ++it) {
            (*it)->redo();
        }
    }

    GraphEditRecorder* graphRecorder() const
    {
        return getApp()->getProject()->getGraphEditRecorder();
    }

    void startRecordingGraph()
    {
        _subscription = graphRecorder()->subscribe([this](const std::vector<Serialization::GraphOp>& ops) {
            _ops.insert(_ops.end(), ops.begin(), ops.end());
        });
    }

    void stopRecordingGraph()
    {
        if (_subscription != -1) {
            graphRecorder()->unsubscribe(_subscription);
            _subscription = -1;
        }
    }

    int takeSetUserKnobsOps(const NodePtr& node)
    {
        graphRecorder()->flush();
        int count = 0;
        for (const Serialization::GraphOp& op : _ops) {
            const Serialization::SetUserKnobsOp* user = std::get_if<Serialization::SetUserKnobsOp>(&op);
            if (user && (user->nodeId == node->getNodeId())) {
                ++count;
            }
        }
        _ops.clear();

        return count;
    }

    // A knob of other that reads source.name dimension through an expression, and one linked to it.
    struct Dependents {
        KnobDoublePtr follower;
        KnobDoublePtr linked;
    };

    Dependents addDependents(const NodePtr& source,
                             const NodePtr& other,
                             const std::string& name,
                             int dimension)
    {
        Dependents ret;
        ret.follower = addTyped<KnobDouble>(other, "follower");
        ret.linked = addTyped<KnobDouble>(other, "linked");
        if (!ret.follower || !ret.linked) {
            return ret;
        }
        const std::string expression = source->getScriptName() + "." + name + ".getValueAtTime(frame, " + std::to_string(dimension) + ") + 1";
        EXPECT_NO_THROW(ret.follower->setExpression(0, expression, false, true));
        EXPECT_TRUE(ret.linked->linkTo(0, source->getKnobByName(name), dimension));

        return ret;
    }

    std::shared_ptr<StepRecorder> recorder;
    int _subscription = -1;
    std::vector<Serialization::GraphOp> _ops;
};
} // anon namespace

TEST_F(UserKnobEdit, ApplySpecEditsEveryPropertyInPlace)
{
    NodePtr blur = createBlur();
    NodePtr other = createBlur();
    ASSERT_TRUE(bool(blur && other));
    ASSERT_TRUE(bool(add(blur, makeSpec(KnobPage::typeNameStatic(), "extraPage"))));
    ASSERT_TRUE(bool(add(blur, makeSpec(KnobGroup::typeNameStatic(), "box", 1, "extraPage"))));
    KnobDoublePtr amount = addTyped<KnobDouble>(blur, "amount", 2);
    ASSERT_TRUE(bool(amount));
    amount->setValueAtTime(1., 1., ViewSpec::all(), 0);
    amount->setValueAtTime(10., 3., ViewSpec::all(), 0);
    ASSERT_NO_THROW(amount->setExpression(1, "frame * 2", false, true));
    const Dependents dependents = addDependents(blur, other, "amount", 0);
    ASSERT_TRUE(bool(dependents.follower && dependents.linked));

    UserKnobSpec spec = specFromKnob(amount);
    spec.name = "gain";
    spec.label = "Gain";
    spec.tooltip = "How much.";
    spec.secret = true;
    spec.newLine = false;
    spec.evaluatesOnChange = false;
    spec.parentName = "box";
    spec.index = 0;
    spec.defaults = { Serialization::KnobValueSerialization(std::in_place_type<double>, 0.5),
                      Serialization::KnobValueSerialization(std::in_place_type<double>, 0.25) };
    Serialization::KnobValueRangeSerialization range;
    range.minimum = -1.;
    range.maximum = 5.;
    range.displayMinimum = 0.;
    range.displayMaximum = 2.;
    spec.extra = range;
    spec.useHostOverlayHandle = true;
    std::string error;
    ASSERT_TRUE(applySpec(holderOf(blur), amount, spec, &error)) << error;

    EXPECT_EQ(amount, blur->getKnobByName("gain"));
    EXPECT_FALSE(bool(blur->getKnobByName("amount")));
    const UserKnobSpec now = specFromKnob(amount);
    EXPECT_TRUE(now == spec) << differences(now, spec);

    EXPECT_EQ(2, keyCount(amount, 0));
    EXPECT_DOUBLE_EQ(3., amount->getValueAtTime(10., 0));
    EXPECT_EQ("frame * 2", amount->getExpression(1));
    const std::string followerExpression = dependents.follower->getExpression(0);
    EXPECT_NE(std::string::npos, followerExpression.find(blur->getScriptName() + ".gain")) << followerExpression;
    EXPECT_EQ(std::string::npos, followerExpression.find("amount")) << followerExpression;
    EXPECT_DOUBLE_EQ(4., dependents.follower->getValueAtTime(10., 0));
    EXPECT_TRUE(isLinkedTo(amount, dependents.linked));
    EXPECT_DOUBLE_EQ(3., dependents.linked->getValueAtTime(10., 0));
}

TEST_F(UserKnobEdit, ValidationRejectsBadNamesAndChoiceDefaults)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr amount = addTyped<KnobDouble>(blur, "amount");
    ASSERT_TRUE(bool(addTyped<KnobDouble>(blur, "taken")));
    ASSERT_TRUE(bool(amount));

    for (const std::string& name : { std::string(), std::string("1st"), std::string("has space"), std::string("for"), std::string("taken") }) {
        UserKnobSpec spec = specFromKnob(amount);
        spec.name = name;
        EXPECT_FALSE(validateSpec(holderOf(blur), amount, spec).empty()) << '"' << name << '"';
        std::string error;
        EXPECT_FALSE(applySpec(holderOf(blur), amount, spec, &error)) << name;
        EXPECT_FALSE(error.empty()) << name;
        EXPECT_EQ("amount", amount->getName());
    }

    UserKnobSpec misplaced = specFromKnob(amount);
    misplaced.parentName = "noSuchPage";
    EXPECT_FALSE(validateSpec(holderOf(blur), amount, misplaced).empty());

    UserKnobSpec choice = makeSpec(KnobChoice::typeNameStatic(), "mode");
    Serialization::KnobChoiceExtraSerialization entries;
    entries.entries = { "a", "b" };
    choice.extra = entries;
    choice.defaults = { Serialization::KnobValueSerialization(std::in_place_type<std::string>, "z") };
    EXPECT_FALSE(validateSpec(holderOf(blur), KnobIPtr(), choice).empty());
    std::string error;
    EXPECT_FALSE(bool(createUserKnob(holderOf(blur), choice, &error)));
    EXPECT_FALSE(bool(blur->getKnobByName("mode")));
    choice.defaults = { Serialization::KnobValueSerialization(std::in_place_type<std::string>, "b") };
    EXPECT_TRUE(validateSpec(holderOf(blur), KnobIPtr(), choice).empty());

    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    groupArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    groupArgs.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    NodePtr group = getApp()->createNode(groupArgs);
    ASSERT_TRUE(bool(group));
    NodeGroupPtr collection = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_TRUE(bool(collection));
    CreateNodeArgs innerArgs(PLUGINID_NATRON_BLUR, collection);
    innerArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    NodePtr inner = getApp()->createNode(innerArgs);
    ASSERT_TRUE(bool(inner));
    UserKnobSpec innerName = makeSpec(KnobDouble::typeNameStatic(), inner->getScriptName());
    EXPECT_FALSE(validateSpec(holderOf(group), KnobIPtr(), innerName).empty());
}

TEST_F(UserKnobEdit, AnimatedIntToDoubleKeepsKeysAndDependents)
{
    NodePtr blur = createBlur();
    NodePtr other = createBlur();
    ASSERT_TRUE(bool(blur && other));
    KnobIntPtr count = addTyped<KnobInt>(blur, "count");
    ASSERT_TRUE(bool(count));
    count->setValueAtTime(1., 1, ViewSpec::all(), 0);
    count->setValueAtTime(10., 4, ViewSpec::all(), 0);
    const Dependents dependents = addDependents(blur, other, "count", 0);
    ASSERT_TRUE(bool(dependents.follower && dependents.linked));
    const UserKnobSpec before = specFromKnob(count);

    KnobIPtr changed;
    std::string error;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), count, KnobDouble::typeNameStatic(), 1, false, &changed, &error)) << error;
    KnobDoublePtr asDouble = std::dynamic_pointer_cast<KnobDouble>(changed);
    ASSERT_TRUE(bool(asDouble));
    EXPECT_EQ(changed, blur->getKnobByName("count"));
    const UserKnobSpec after = specFromKnob(changed);
    EXPECT_EQ(before.parentName, after.parentName);
    EXPECT_EQ(before.index, after.index);
    EXPECT_EQ(2, keyCount(changed, 0));
    EXPECT_DOUBLE_EQ(1., keyValue(changed, 0, 0));
    EXPECT_DOUBLE_EQ(4., keyValue(changed, 0, 1));
    EXPECT_DOUBLE_EQ(5., dependents.follower->getValueAtTime(10., 0));

    asDouble->setValueAtTime(10., 6.5, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(7.5, dependents.follower->getValueAtTime(10., 0));
    EXPECT_TRUE(isLinkedTo(changed, dependents.linked));
    EXPECT_DOUBLE_EQ(6.5, dependents.linked->getValueAtTime(10., 0));
}

TEST_F(UserKnobEdit, DoubleToIntRoundsValuesAndKeys)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr amount = addTyped<KnobDouble>(blur, "amount", 2);
    ASSERT_TRUE(bool(amount));
    amount->setValue(2.6, ViewSpec::all(), 0);
    amount->setValueAtTime(1., 1.4, ViewSpec::all(), 1);
    amount->setValueAtTime(10., 2.6, ViewSpec::all(), 1);

    KnobIPtr changed;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), amount, KnobInt::typeNameStatic(), 2, false, &changed));
    KnobIntPtr asInt = std::dynamic_pointer_cast<KnobInt>(changed);
    ASSERT_TRUE(bool(asInt));
    EXPECT_EQ(3, asInt->getValue(0));
    EXPECT_EQ(2, keyCount(changed, 1));
    EXPECT_DOUBLE_EQ(1., keyValue(changed, 1, 0));
    EXPECT_DOUBLE_EQ(3., keyValue(changed, 1, 1));
}

TEST_F(UserKnobEdit, Double3DToColorRGBAWidensWithTheDefaultAlpha)
{
    NodePtr blur = createBlur();
    NodePtr other = createBlur();
    ASSERT_TRUE(bool(blur && other));
    KnobDoublePtr position = addTyped<KnobDouble>(blur, "position", 3);
    ASSERT_TRUE(bool(position));
    for (int i = 0; i < 3; ++i) {
        position->setValue(0.1 * (i + 1), ViewSpec::all(), i);
    }
    const Dependents dependents = addDependents(blur, other, "position", 2);
    ASSERT_TRUE(bool(dependents.follower && dependents.linked));

    KnobIPtr changed;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), position, KnobColor::typeNameStatic(), 4, false, &changed));
    KnobColorPtr color = std::dynamic_pointer_cast<KnobColor>(changed);
    ASSERT_TRUE(bool(color));
    ASSERT_EQ(4, color->getDimension());
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(0.1 * (i + 1), color->getValue(i)) << i;
    }
    EXPECT_DOUBLE_EQ(color->getDefaultValue(3), color->getValue(3));
    EXPECT_DOUBLE_EQ(1.3, dependents.follower->getValue(0));
    color->setValue(0.9, ViewSpec::all(), 2);
    EXPECT_DOUBLE_EQ(1.9, dependents.follower->getValue(0));
    EXPECT_TRUE(isLinkedTo(changed, dependents.linked));
    EXPECT_DOUBLE_EQ(0.9, dependents.linked->getValue(0));
}

TEST_F(UserKnobEdit, ColorRGBAToDoubleNarrows)
{
    NodePtr blur = createBlur();
    NodePtr other = createBlur();
    ASSERT_TRUE(bool(blur && other));
    KnobColorPtr tint = addTyped<KnobColor>(blur, "tint", 4);
    ASSERT_TRUE(bool(tint));
    for (int i = 0; i < 4; ++i) {
        tint->setValue(0.5 + 0.1 * i, ViewSpec::all(), i);
    }
    tint->setValueAtTime(5., 0.2, ViewSpec::all(), 3);
    const Dependents dependents = addDependents(blur, other, "tint", 0);
    ASSERT_TRUE(bool(dependents.follower && dependents.linked));

    KnobIPtr changed;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), tint, KnobDouble::typeNameStatic(), 1, false, &changed));
    KnobDoublePtr asDouble = std::dynamic_pointer_cast<KnobDouble>(changed);
    ASSERT_TRUE(bool(asDouble));
    EXPECT_EQ(1, asDouble->getDimension());
    EXPECT_DOUBLE_EQ(0.5, asDouble->getValue(0));
    EXPECT_EQ(0, keyCount(changed, 0));
    asDouble->setValue(2., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(3., dependents.follower->getValue(0));
    EXPECT_TRUE(isLinkedTo(changed, dependents.linked));
    EXPECT_DOUBLE_EQ(2., dependents.linked->getValue(0));
}

TEST_F(UserKnobEdit, TextToIntNeedsAResetUnlessAllowed)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobStringPtr text = addTyped<KnobString>(blur, "text");
    ASSERT_TRUE(bool(text));
    text->setValue(std::string("hello"), ViewSpec::all(), 0);
    EXPECT_FALSE(isUserKnobTypeChangeConvertible(KnobString::typeNameStatic(), 1, KnobInt::typeNameStatic(), 1));

    KnobIPtr changed;
    EXPECT_EQ(eUserKnobTypeChangeNeedsReset, changeUserKnobType(holderOf(blur), text, KnobInt::typeNameStatic(), 1, false, &changed));
    EXPECT_EQ(text, blur->getKnobByName("text"));
    EXPECT_EQ("hello", text->getValue(0));

    ASSERT_EQ(eUserKnobTypeChangeReset, changeUserKnobType(holderOf(blur), text, KnobInt::typeNameStatic(), 1, true, &changed));
    KnobIntPtr asInt = std::dynamic_pointer_cast<KnobInt>(changed);
    ASSERT_TRUE(bool(asInt));
    EXPECT_EQ(asInt, blur->getKnobByName("text"));
    EXPECT_EQ(asInt->getDefaultValue(0), asInt->getValue(0));
}

TEST_F(UserKnobEdit, EachOperationIsOneUndoEntry)
{
    NodePtr blur = createBlur();
    NodePtr other = createBlur();
    ASSERT_TRUE(bool(blur && other));
    KnobDoublePtr amount = addTyped<KnobDouble>(blur, "amount");
    ASSERT_TRUE(bool(amount));
    amount->setValueAtTime(1., 1., ViewSpec::all(), 0);
    amount->setValueAtTime(10., 3., ViewSpec::all(), 0);
    const Dependents dependents = addDependents(blur, other, "amount", 0);
    ASSERT_TRUE(bool(dependents.follower && dependents.linked));
    startRecordingGraph();

    const auto state = [&]() {
        return userKnobState(blur) + "\n--\n" + dependents.follower->getExpression(0) + "\n--\n" + userKnobState(other);
    };
    const auto check = [&](const std::string& label, const std::function<void()>& operation) {
        SCOPED_TRACE(label);
        recorder->commands.clear();
        takeSetUserKnobsOps(blur);
        const std::uint64_t serial = recorder->serial;
        const std::string before = state();

        operation();

        EXPECT_EQ(serial + 1, recorder->serial);
        ASSERT_FALSE(recorder->stepTexts.empty());
        EXPECT_EQ(label, recorder->stepTexts.back());
        EXPECT_EQ(1, recorder->userKnobCommands());
        EXPECT_EQ(1, takeSetUserKnobsOps(blur));
        const std::string after = state();
        EXPECT_NE(before, after);

        undoAll();
        EXPECT_EQ(before, state());
        redoAll();
        EXPECT_EQ(after, state());
    };

    check("Add parameter extra", [&]() {
        EXPECT_TRUE(bool(createUserKnob(holderOf(blur), makeSpec(KnobDouble::typeNameStatic(), "extra"))));
    });
    check("Edit parameter gain", [&]() {
        UserKnobSpec spec = specFromKnob(blur->getKnobByName("amount"));
        spec.name = "gain";
        spec.label = "Gain";
        EXPECT_TRUE(applySpec(holderOf(blur), blur->getKnobByName("amount"), spec));
    });
    check("Move parameter extra", [&]() {
        EXPECT_TRUE(moveUserKnob(holderOf(blur), blur->getKnobByName("extra"), true));
    });
    check("Change type of gain", [&]() {
        EXPECT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), blur->getKnobByName("gain"), KnobColor::typeNameStatic(), 3, false));
    });
    check("Paste parameters", [&]() {
        const std::string text = copyUserKnobs(holderOf(blur), { blur->getKnobByName("extra") });
        EXPECT_FALSE(pasteUserKnobs(holderOf(blur), text).empty());
    });
    check("Remove parameter gain", [&]() {
        EXPECT_FALSE(getUserKnobDependents(blur->getKnobByName("gain")).empty());
        EXPECT_TRUE(deleteUserKnob(holderOf(blur), blur->getKnobByName("gain")));
    });
}

TEST_F(UserKnobEdit, NestedScopesRecordOneEntry)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    startRecordingGraph();
    takeSetUserKnobsOps(blur);
    const std::uint64_t serial = recorder->serial;
    const std::string before = userKnobState(blur);
    {
        UserKnobsUndoScope outer(holderOf(blur), "Outer");
        KnobIPtr first = createUserKnob(holderOf(blur), makeSpec(KnobDouble::typeNameStatic(), "first"));
        ASSERT_TRUE(bool(first));
        UserKnobSpec spec = specFromKnob(first);
        spec.label = "First";
        EXPECT_TRUE(applySpec(holderOf(blur), first, spec));
        EXPECT_TRUE(bool(createUserKnob(holderOf(blur), makeSpec(KnobInt::typeNameStatic(), "second"))));
    }
    EXPECT_EQ(serial + 1, recorder->serial);
    EXPECT_EQ(1, recorder->userKnobCommands());
    EXPECT_EQ(1, takeSetUserKnobsOps(blur));

    undoAll();
    EXPECT_EQ(before, userKnobState(blur));
    EXPECT_FALSE(bool(blur->getKnobByName("first")));
    EXPECT_FALSE(bool(blur->getKnobByName("second")));
}

TEST_F(UserKnobEdit, ButtonScriptRunsAndSurvivesSaveAndLoad)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr target = addTyped<KnobDouble>(blur, "target");
    ASSERT_TRUE(bool(target));
    UserKnobSpec spec = makeSpec(KnobButton::typeNameStatic(), "press");
    Serialization::KnobButtonExtraSerialization script;
    script.script = "thisNode.target.setValue(4.5)\nassert thisParam.getScriptName() == 'press'\n";
    spec.extra = script;
    KnobButtonPtr button = std::dynamic_pointer_cast<KnobButton>(add(blur, spec));
    ASSERT_TRUE(bool(button));
    EXPECT_EQ(script.script, button->getScript());

    button->trigger();
    EXPECT_DOUBLE_EQ(4.5, target->getValue(0));

    const std::string nodeName = blur->getScriptName();
    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("button-script.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(nodeName);
    ASSERT_TRUE(bool(loaded));
    KnobButtonPtr loadedButton = std::dynamic_pointer_cast<KnobButton>(loaded->getKnobByName("press"));
    KnobDoublePtr loadedTarget = std::dynamic_pointer_cast<KnobDouble>(loaded->getKnobByName("target"));
    ASSERT_TRUE(bool(loadedButton && loadedTarget));
    EXPECT_EQ(script.script, loadedButton->getScript());
    loadedTarget->setValue(0., ViewSpec::all(), 0);
    loadedButton->trigger();
    EXPECT_DOUBLE_EQ(4.5, loadedTarget->getValue(0));
}

TEST_F(UserKnobEdit, ButtonScriptErrorIsReported)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr target = addTyped<KnobDouble>(blur, "target");
    ASSERT_TRUE(bool(target));
    UserKnobSpec failing = makeSpec(KnobButton::typeNameStatic(), "failing");
    Serialization::KnobButtonExtraSerialization raising;
    raising.script = "raise RuntimeError('button script failed on purpose')\n";
    failing.extra = raising;
    KnobButtonPtr failingButton = std::dynamic_pointer_cast<KnobButton>(add(blur, failing));
    UserKnobSpec working = makeSpec(KnobButton::typeNameStatic(), "working");
    Serialization::KnobButtonExtraSerialization setting;
    setting.script = "thisNode.target.setValue(2.0)\n";
    working.extra = setting;
    KnobButtonPtr workingButton = std::dynamic_pointer_cast<KnobButton>(add(blur, working));
    ASSERT_TRUE(bool(failingButton && workingButton));

    appPTR->clearErrorLog_mt_safe();
    EXPECT_FALSE(bool(failingButton->runScript()));
    std::list<LogEntry> entries;
    appPTR->getErrorLog_mt_safe(&entries);
    bool reported = false;
    for (const LogEntry& entry : entries) {
        reported = reported || entry.message.contains(QString::fromUtf8("button script failed on purpose"));
    }
    EXPECT_TRUE(reported);

    failingButton->trigger();
    workingButton->trigger();
    EXPECT_DOUBLE_EQ(2., target->getValue(0));
}

TEST_F(UserKnobEdit, EveryCreatableTypeRoundTripsThroughAProject)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(bool(add(blur, makeSpec(KnobPage::typeNameStatic(), "settings"))));
    UserKnobSpec group = makeSpec(KnobGroup::typeNameStatic(), "box", 1, "settings");
    group.label = "Box";
    group.groupAsTab = true;
    ASSERT_TRUE(bool(add(blur, group)));

    std::vector<UserKnobSpec> specs;
    const auto range = [](double minimum, double maximum, double displayMinimum, double displayMaximum) {
        Serialization::KnobValueRangeSerialization ret;
        ret.minimum = minimum;
        ret.maximum = maximum;
        ret.displayMinimum = displayMinimum;
        ret.displayMaximum = displayMaximum;

        return Serialization::KnobExtraSerialization(ret);
    };
    const auto ints = [](std::vector<int> values) {
        std::vector<Serialization::KnobValueSerialization> ret;
        for (int value : values) {
            ret.push_back(Serialization::KnobValueSerialization(std::in_place_type<int>, value));
        }

        return ret;
    };
    const auto doubles = [](std::vector<double> values) {
        std::vector<Serialization::KnobValueSerialization> ret;
        for (double value : values) {
            ret.push_back(Serialization::KnobValueSerialization(std::in_place_type<double>, value));
        }

        return ret;
    };
    for (int dimension = 1; dimension <= 3; ++dimension) {
        UserKnobSpec spec = makeSpec(KnobInt::typeNameStatic(), "int" + std::to_string(dimension), dimension, "settings");
        spec.extra = range(-10, 20, 0, 10);
        spec.defaults = ints(std::vector<int>(dimension, 3));
        spec.tooltip = "An integer.";
        specs.push_back(spec);
    }
    for (int dimension = 1; dimension <= 3; ++dimension) {
        UserKnobSpec spec = makeSpec(KnobDouble::typeNameStatic(), "double" + std::to_string(dimension), dimension, "box");
        spec.extra = range(-1, 2, 0, 1.5);
        spec.defaults = doubles(std::vector<double>(dimension, 0.75));
        spec.useHostOverlayHandle = (dimension == 2);
        spec.newLine = (dimension != 1);
        specs.push_back(spec);
    }
    {
        UserKnobSpec spec = makeSpec(KnobColor::typeNameStatic(), "rgb", 3, "settings");
        spec.extra = range(0, 4, 0, 2);
        spec.defaults = doubles({ 0.1, 0.2, 0.3 });
        spec.animates = false;
        specs.push_back(spec);
        spec = makeSpec(KnobColor::typeNameStatic(), "rgba", 4, "settings");
        spec.extra = range(0, 1, 0, 1);
        spec.defaults = doubles({ 0.1, 0.2, 0.3, 0.4 });
        specs.push_back(spec);
    }
    {
        UserKnobSpec spec = makeSpec(KnobBool::typeNameStatic(), "toggle", 1, "settings");
        spec.defaults = { Serialization::KnobValueSerialization(std::in_place_type<bool>, true) };
        spec.secret = true;
        specs.push_back(spec);
    }
    {
        UserKnobSpec spec = makeSpec(KnobChoice::typeNameStatic(), "mode", 1, "settings");
        Serialization::KnobChoiceExtraSerialization choice;
        choice.entries = { "first", "second", "third" };
        choice.helpStrings = { "The first.", "The second.", "The third." };
        spec.extra = choice;
        spec.defaults = { Serialization::KnobValueSerialization(std::in_place_type<std::string>, "second") };
        spec.evaluatesOnChange = false;
        specs.push_back(spec);
    }
    {
        UserKnobSpec spec = makeSpec(KnobString::typeNameStatic(), "note", 1, "settings");
        Serialization::KnobTextExtraSerialization text;
        text.isMultiLine = true;
        text.usesRichText = true;
        spec.extra = text;
        spec.defaults = { Serialization::KnobValueSerialization(std::in_place_type<std::string>, "hello") };
        specs.push_back(spec);
    }
    {
        UserKnobSpec spec = makeSpec(KnobFile::typeNameStatic(), "source", 1, "settings");
        Serialization::KnobFileExtraSerialization file;
        file.useSequences = true;
        spec.extra = file;
        specs.push_back(spec);
        spec = makeSpec(KnobOutputFile::typeNameStatic(), "destination", 1, "settings");
        spec.extra = file;
        specs.push_back(spec);
    }
    {
        UserKnobSpec spec = makeSpec(KnobPath::typeNameStatic(), "searchPath", 1, "settings");
        Serialization::KnobPathExtraSerialization path;
        path.multiPath = true;
        spec.extra = path;
        specs.push_back(spec);
    }
    {
        // Not channels or layer: a node can already have its own knobs by those names.
        Serialization::KnobLayerSelectExtraSerialization buttons;
        buttons.withChannelButtons = true;
        UserKnobSpec spec = makeSpec(KnobChannelSet::typeNameStatic(), "userChannels", 1, "settings");
        spec.extra = buttons;
        specs.push_back(spec);
        spec = makeSpec(KnobLayerSelect::typeNameStatic(), "userLayer", 1, "settings");
        spec.extra = buttons;
        specs.push_back(spec);
        specs.push_back(makeSpec(KnobChannelSelect::typeNameStatic(), "userChannel", 1, "settings"));
    }
    {
        UserKnobSpec spec = makeSpec(KnobButton::typeNameStatic(), "press", 1, "settings");
        Serialization::KnobButtonExtraSerialization button;
        button.script = "thisNode.toggle.setValue(False)\n";
        spec.extra = button;
        specs.push_back(spec);
        specs.push_back(makeSpec(KnobSeparator::typeNameStatic(), "divider", 1, "settings"));
    }
    {
        UserKnobSpec spec = makeSpec(KnobParametric::typeNameStatic(), "curves", 2, "box");
        Serialization::KnobParametricExtraSerialization parametric;
        parametric.curveLabels = { "lift", "gamma" };
        parametric.curveColors = { { 1., 0., 0. }, { 0., 0.5, 1. } };
        parametric.rangeMinimum = -1.;
        parametric.rangeMaximum = 2.;
        spec.extra = parametric;
        specs.push_back(spec);
    }

    std::map<std::string, UserKnobSpec> before;
    for (const UserKnobSpec& spec : specs) {
        KnobIPtr knob = add(blur, spec);
        ASSERT_TRUE(bool(knob)) << spec.name;
    }
    for (const std::string& name : { std::string("settings"), std::string("box") }) {
        before[name] = specFromKnob(blur->getKnobByName(name));
    }
    for (const UserKnobSpec& spec : specs) {
        before[spec.name] = specFromKnob(blur->getKnobByName(spec.name));
        const UserKnobSpec& made = before[spec.name];
        EXPECT_TRUE(made.extra == spec.extra) << spec.name;
        if (!spec.defaults.empty()) {
            EXPECT_TRUE(made.defaults == spec.defaults) << spec.name;
        }
    }

    const std::string nodeName = blur->getScriptName();
    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("user-knob-types.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));
    NodePtr loaded = project->getNodeByName(nodeName);
    ASSERT_TRUE(bool(loaded));

    for (const std::pair<const std::string, UserKnobSpec>& entry : before) {
        KnobIPtr knob = loaded->getKnobByName(entry.first);
        ASSERT_TRUE(bool(knob)) << entry.first;
        EXPECT_TRUE(knob->isUserKnob()) << entry.first;
        const UserKnobSpec after = specFromKnob(knob);
        EXPECT_TRUE(after == entry.second) << entry.first << ": " << differences(after, entry.second);
    }
}

TEST_F(UserKnobEdit, CopyAndPasteAPageOntoAnotherNode)
{
    NodePtr source = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(bool(source && target));
    ASSERT_TRUE(bool(add(source, makeSpec(KnobPage::typeNameStatic(), "tools"))));
    ASSERT_TRUE(bool(add(source, makeSpec(KnobGroup::typeNameStatic(), "box", 1, "tools"))));
    KnobDoublePtr animated = addTyped<KnobDouble>(source, "animated", 1, "box");
    KnobDoublePtr expressed = addTyped<KnobDouble>(source, "expressed", 1, "tools");
    KnobIntPtr plain = addTyped<KnobInt>(source, "plain", 1, "tools");
    ASSERT_TRUE(bool(animated && expressed && plain));
    animated->setValueAtTime(1., 2., ViewSpec::all(), 0);
    animated->setValueAtTime(20., 8., ViewSpec::all(), 0);
    ASSERT_NO_THROW(expressed->setExpression(0, "frame * 3", false, true));
    plain->setValue(7, ViewSpec::all(), 0);
    KnobIntPtr colliding = addTyped<KnobInt>(target, "plain");
    ASSERT_TRUE(bool(colliding));

    const std::string text = copyUserKnobs(holderOf(source), { source->getKnobByName("tools") });
    ASSERT_FALSE(text.empty());
    const std::string targetBefore = userKnobState(target);
    recorder->commands.clear();
    const std::uint64_t serial = recorder->serial;
    std::string error;
    const std::list<KnobIPtr> created = pasteUserKnobs(holderOf(target), text, KnobIPtr(), -1, &error);
    ASSERT_EQ(1u, created.size()) << error;
    EXPECT_EQ("tools", created.front()->getName());
    EXPECT_EQ(serial + 1, recorder->serial);

    for (const std::string& name : { std::string("box"), std::string("animated"), std::string("expressed") }) {
        KnobIPtr pasted = target->getKnobByName(name);
        ASSERT_TRUE(bool(pasted)) << name;
        const UserKnobSpec copied = specFromKnob(source->getKnobByName(name));
        const UserKnobSpec made = specFromKnob(pasted);
        EXPECT_TRUE(made == copied) << name << ": " << differences(made, copied);
    }
    KnobIPtr pastedAnimated = target->getKnobByName("animated");
    EXPECT_EQ(2, keyCount(pastedAnimated, 0));
    EXPECT_DOUBLE_EQ(8., keyValue(pastedAnimated, 0, 1));
    EXPECT_EQ("frame * 3", target->getKnobByName("expressed")->getExpression(0));

    KnobIntPtr renamed = std::dynamic_pointer_cast<KnobInt>(target->getKnobByName("plain2"));
    ASSERT_TRUE(bool(renamed));
    EXPECT_EQ(7, renamed->getValue(0));
    UserKnobSpec renamedSpec = specFromKnob(renamed);
    renamedSpec.name = "plain";
    const UserKnobSpec plainSpec = specFromKnob(plain);
    EXPECT_TRUE(renamedSpec == plainSpec) << differences(renamedSpec, plainSpec);
    EXPECT_EQ(colliding, target->getKnobByName("plain"));

    undoAll();
    EXPECT_EQ(targetBefore, userKnobState(target));
    for (const std::string& name : { std::string("tools"), std::string("box"), std::string("animated"), std::string("expressed"), std::string("plain2") }) {
        EXPECT_FALSE(bool(target->getKnobByName(name))) << name;
    }
    EXPECT_TRUE(bool(target->getKnobByName("plain")));
}

TEST_F(UserKnobEdit, PythonChangesAnAnimatedIntToDouble)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobIntPtr count = addTyped<KnobInt>(blur, "count");
    ASSERT_TRUE(bool(count));
    count->setValueAtTime(1., 2, ViewSpec::all(), 0);
    count->setValueAtTime(10., 5, ViewSpec::all(), 0);

    const std::string node = getApp()->getAppIDString() + "." + blur->getFullyQualifiedName();
    ASSERT_TRUE(run("thisNode = " + node + "\n"
                                           "changed = thisNode.changeParamType(thisNode.count, 'Double')\n"
                                           "assert type(changed).__name__ == 'DoubleParam', type(changed).__name__\n"
                                           "assert changed.getNumKeys(0) == 2\n"
                                           "assert changed.getKeyTime(0, 0) == (True, 1.0), changed.getKeyTime(0, 0)\n"
                                           "assert changed.getValueAtTime(10, 0) == 5.0\n"
                                           "assert type(thisNode.count).__name__ == 'DoubleParam', type(thisNode.count).__name__\n"
                                           "assert thisNode.count.getNumKeys(0) == 2\n"
                                           "assert thisNode.count.getKeyTime(1, 0) == (True, 10.0), thisNode.count.getKeyTime(1, 0)\n"
                                           "words = thisNode.createStringParam('words', 'Words')\n"
                                           "assert thisNode.changeParamType(words, 'Int') is None\n"
                                           "assert type(thisNode.words).__name__ == 'StringParam'\n"
                                           "assert type(thisNode.changeParamType(words, 'Int', True)).__name__ == 'IntParam'\n"));
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName("count"))));
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobInt>(blur->getKnobByName("words"))));
}

TEST_F(UserKnobEdit, UndoingALabelEditKeepsTheKnobAndItsReaders)
{
    NodePtr blur = createBlur();
    NodePtr other = createBlur();
    ASSERT_TRUE(bool(blur && other));
    KnobDoublePtr amount = addTyped<KnobDouble>(blur, "amount");
    ASSERT_TRUE(bool(amount));
    const Dependents dependents = addDependents(blur, other, "amount", 0);
    ASSERT_TRUE(bool(dependents.follower && dependents.linked));
    const std::string expression = dependents.follower->getExpression(0);
    ASSERT_FALSE(expression.empty());

    recorder->commands.clear();
    UserKnobSpec spec = specFromKnob(amount);
    spec.label = "How Much";
    spec.tooltip = "The amount.";
    ASSERT_TRUE(applySpec(holderOf(blur), amount, spec));
    EXPECT_EQ(1, recorder->userKnobCommands());

    undoAll();
    EXPECT_EQ(amount, blur->getKnobByName("amount"));
    EXPECT_EQ("amount", amount->getLabel());
    EXPECT_EQ(std::string(), amount->getHintToolTip());
    EXPECT_EQ(expression, dependents.follower->getExpression(0));
    EXPECT_TRUE(isLinkedTo(amount, dependents.linked));
    amount->setValue(5., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(6., dependents.follower->getValue(0));
    EXPECT_DOUBLE_EQ(5., dependents.linked->getValue(0));

    redoAll();
    EXPECT_EQ(amount, blur->getKnobByName("amount"));
    EXPECT_EQ("How Much", amount->getLabel());
    EXPECT_EQ(expression, dependents.follower->getExpression(0));
}

TEST_F(UserKnobEdit, AnEmptyParentMovesTheKnobBackToTheUserPage)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr amount = addTyped<KnobDouble>(blur, "amount");
    ASSERT_TRUE(bool(add(blur, makeSpec(KnobGroup::typeNameStatic(), "box"))));
    ASSERT_TRUE(bool(amount));
    KnobPagePtr userPage = holderOf(blur)->getUserPageKnob();
    ASSERT_TRUE(userPage != nullptr);

    UserKnobSpec spec = specFromKnob(amount);
    spec.parentName = "box";
    spec.index = -1;
    std::string error;
    ASSERT_TRUE(applySpec(holderOf(blur), amount, spec, &error)) << error;
    ASSERT_EQ(blur->getKnobByName("box"), amount->getParentKnob());

    recorder->commands.clear();
    spec.parentName.clear();
    ASSERT_TRUE(applySpec(holderOf(blur), amount, spec, &error)) << error;
    EXPECT_EQ(KnobIPtr(userPage), amount->getParentKnob());
    EXPECT_EQ(1, recorder->userKnobCommands());

    undoAll();
    EXPECT_EQ(amount, blur->getKnobByName("amount"));
    EXPECT_EQ(blur->getKnobByName("box"), amount->getParentKnob());
}

TEST_F(UserKnobEdit, ARejectedMoveLeavesTheKnobInPlace)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(bool(add(blur, makeSpec(KnobPage::typeNameStatic(), "tools"))));
    KnobGroupPtr vars = std::dynamic_pointer_cast<KnobGroup>(add(blur, makeSpec(KnobGroup::typeNameStatic(), "vars", 1, "tools")));
    KnobDoublePtr first = addTyped<KnobDouble>(blur, "first", 1, "tools");
    KnobIPtr press = add(blur, makeSpec(KnobButton::typeNameStatic(), "press", 1, "tools"));
    ASSERT_TRUE(bool(vars && first && press));
    vars->setAsRFVGroup(true);
    const KnobIPtr parent = press->getParentKnob();
    const std::vector<KnobIPtr> before = std::dynamic_pointer_cast<KnobPage>(parent)->getChildren();
    const std::string state = userKnobState(blur);
    recorder->commands.clear();
    startRecordingGraph();
    takeSetUserKnobsOps(blur);

    UserKnobSpec spec = specFromKnob(press);
    spec.parentName = "vars";
    spec.index = 0;
    EXPECT_FALSE(validateSpec(holderOf(blur), press, spec).empty());
    std::string error;
    EXPECT_FALSE(applySpec(holderOf(blur), press, spec, &error));
    EXPECT_FALSE(error.empty());

    EXPECT_EQ(parent, press->getParentKnob());
    EXPECT_EQ(before, std::dynamic_pointer_cast<KnobPage>(parent)->getChildren());
    EXPECT_TRUE(vars->getChildren().empty());
    EXPECT_EQ(state, userKnobState(blur));
    EXPECT_EQ(0, recorder->userKnobCommands());
    EXPECT_EQ(0, takeSetUserKnobsOps(blur));
}

TEST_F(UserKnobEdit, PastedExpressionsAndLinksPointAtThePastedKnobs)
{
    NodePtr source = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(bool(source && target));
    ASSERT_TRUE(bool(add(source, makeSpec(KnobPage::typeNameStatic(), "tools"))));
    KnobDoublePtr x = addTyped<KnobDouble>(source, "x", 1, "tools");
    KnobDoublePtr x2 = addTyped<KnobDouble>(source, "x2", 1, "tools");
    KnobDoublePtr sum = addTyped<KnobDouble>(source, "sum", 1, "tools");
    KnobDoublePtr a = addTyped<KnobDouble>(source, "a", 1, "tools");
    KnobDoublePtr b = addTyped<KnobDouble>(source, "b", 1, "tools");
    ASSERT_TRUE(bool(x && x2 && sum && a && b));
    x->setValue(1., ViewSpec::all(), 0);
    x2->setValue(10., ViewSpec::all(), 0);
    ASSERT_NO_THROW(sum->setExpression(0, "thisNode.x.get() + thisNode.x2.get()", false, true));
    ASSERT_TRUE(b->linkTo(0, a, 0));
    KnobDoublePtr taken = addTyped<KnobDouble>(target, "x");
    ASSERT_TRUE(bool(taken));
    taken->setValue(100., ViewSpec::all(), 0);

    const std::string text = copyUserKnobs(holderOf(source), { source->getKnobByName("tools") });
    std::string error;
    ASSERT_FALSE(pasteUserKnobs(holderOf(target), text, KnobIPtr(), -1, &error).empty()) << error;

    KnobIPtr pastedX = target->getKnobByName("x2");
    KnobIPtr pastedX2 = target->getKnobByName("x22");
    KnobIPtr pastedSum = target->getKnobByName("sum");
    KnobIPtr pastedA = target->getKnobByName("a");
    KnobIPtr pastedB = target->getKnobByName("b");
    ASSERT_TRUE(bool(pastedX && pastedX2 && pastedSum && pastedA && pastedB));
    EXPECT_EQ("thisNode.x2.get() + thisNode.x22.get()", pastedSum->getExpression(0));
    EXPECT_DOUBLE_EQ(11., std::dynamic_pointer_cast<KnobDouble>(pastedSum)->getValue(0));

    EXPECT_TRUE(isLinkedTo(pastedB, pastedA));
    EXPECT_FALSE(isLinkedTo(pastedB, a));
    EXPECT_FALSE(isLinkedTo(pastedA, a));
    EXPECT_FALSE(isLinkedTo(pastedA, b));
    EXPECT_TRUE(isLinkedTo(b, a));
}

TEST_F(UserKnobEdit, ContainerTooltipsRoundTrip)
{
    NodePtr source = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(bool(source && target));
    UserKnobSpec pageSpec = makeSpec(KnobPage::typeNameStatic(), "tools");
    pageSpec.tooltip = "Tools of the node.";
    UserKnobSpec groupSpec = makeSpec(KnobGroup::typeNameStatic(), "box", 1, "tools");
    groupSpec.tooltip = "What is in the box.";
    ASSERT_TRUE(bool(add(source, pageSpec)));
    KnobIPtr box = add(source, groupSpec);
    ASSERT_TRUE(bool(box && addTyped<KnobDouble>(source, "inside", 1, "box")));

    const std::string text = copyUserKnobs(holderOf(source), { source->getKnobByName("tools") });
    ASSERT_FALSE(pasteUserKnobs(holderOf(target), text).empty());
    ASSERT_TRUE(bool(target->getKnobByName("tools") && target->getKnobByName("box")));
    EXPECT_EQ(pageSpec.tooltip, target->getKnobByName("tools")->getHintToolTip());
    EXPECT_EQ(groupSpec.tooltip, target->getKnobByName("box")->getHintToolTip());

    recorder->commands.clear();
    UserKnobSpec edited = specFromKnob(box);
    edited.tooltip = "Another box.";
    ASSERT_TRUE(applySpec(holderOf(source), box, edited));
    undoAll();
    EXPECT_EQ(groupSpec.tooltip, box->getHintToolTip());
    redoAll();
    EXPECT_EQ(edited.tooltip, box->getHintToolTip());

    const std::string nodeName = source->getScriptName();
    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("container-tooltips.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(nodeName);
    ASSERT_TRUE(bool(loaded));
    ASSERT_TRUE(bool(loaded->getKnobByName("tools") && loaded->getKnobByName("box")));
    EXPECT_EQ(pageSpec.tooltip, loaded->getKnobByName("tools")->getHintToolTip());
    EXPECT_EQ(edited.tooltip, loaded->getKnobByName("box")->getHintToolTip());
}

TEST_F(UserKnobEdit, PastingOneMemberOfALinkGroupDropsTheLink)
{
    NodePtr source = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(bool(source && target));
    KnobDoublePtr a = addTyped<KnobDouble>(source, "a");
    KnobDoublePtr b = addTyped<KnobDouble>(source, "b");
    ASSERT_TRUE(bool(a && b));
    ASSERT_TRUE(b->linkTo(0, a, 0));
    ASSERT_TRUE(isLinkedTo(a, b) || isLinkedTo(b, a));

    const std::string text = copyUserKnobs(holderOf(source), { source->getKnobByName("a") });
    std::string error;
    ASSERT_FALSE(pasteUserKnobs(holderOf(target), text, KnobIPtr(), -1, &error).empty()) << error;
    KnobIPtr pastedA = target->getKnobByName("a");
    ASSERT_TRUE(bool(pastedA));
    EXPECT_FALSE(isLinkedTo(pastedA, a));
    EXPECT_FALSE(isLinkedTo(pastedA, b));
    EXPECT_FALSE(isLinkedTo(a, pastedA));
    EXPECT_FALSE(isLinkedTo(b, pastedA));
    EXPECT_TRUE(isLinkedTo(b, a));
}

TEST_F(UserKnobEdit, PlaceholderLikeKnobNamesSurviveAPasteRename)
{
    NodePtr source = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(bool(source && target));
    ASSERT_TRUE(bool(add(source, makeSpec(KnobPage::typeNameStatic(), "tools"))));
    KnobDoublePtr x = addTyped<KnobDouble>(source, "x", 1, "tools");
    KnobDoublePtr odd = addTyped<KnobDouble>(source, "natronPastedKnob0_", 1, "tools");
    KnobDoublePtr sum = addTyped<KnobDouble>(source, "sum", 1, "tools");
    ASSERT_TRUE(bool(x && odd && sum));
    ASSERT_NO_THROW(sum->setExpression(0, "thisNode.x.get() + thisNode.natronPastedKnob0_.get()", false, true));
    ASSERT_TRUE(bool(addTyped<KnobDouble>(target, "x")));

    const std::string text = copyUserKnobs(holderOf(source), { source->getKnobByName("tools") });
    std::string error;
    ASSERT_FALSE(pasteUserKnobs(holderOf(target), text, KnobIPtr(), -1, &error).empty()) << error;
    KnobIPtr pastedSum = target->getKnobByName("sum");
    ASSERT_TRUE(bool(pastedSum && target->getKnobByName("x2") && target->getKnobByName("natronPastedKnob0_")));
    EXPECT_EQ("thisNode.x2.get() + thisNode.natronPastedKnob0_.get()", pastedSum->getExpression(0));
}

TEST_F(UserKnobEdit, HiddenPagesAndGroupsStayHiddenAfterReload)
{
    NodePtr node = createBlur();
    ASSERT_TRUE(bool(node));
    UserKnobSpec pageSpec = makeSpec(KnobPage::typeNameStatic(), "tools");
    pageSpec.secret = true;
    UserKnobSpec groupSpec = makeSpec(KnobGroup::typeNameStatic(), "box", 1, "tools");
    groupSpec.secret = true;
    ASSERT_TRUE(bool(add(node, pageSpec)));
    ASSERT_TRUE(bool(add(node, groupSpec)));
    ASSERT_TRUE(bool(addTyped<KnobDouble>(node, "inside", 1, "box")));
    ASSERT_TRUE(node->getKnobByName("tools")->getIsSecret());
    ASSERT_TRUE(node->getKnobByName("box")->getIsSecret());

    const std::string nodeName = node->getScriptName();
    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("hidden-containers.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(nodeName);
    ASSERT_TRUE(bool(loaded));
    ASSERT_TRUE(bool(loaded->getKnobByName("tools") && loaded->getKnobByName("box")));
    EXPECT_TRUE(loaded->getKnobByName("tools")->getIsSecret());
    EXPECT_TRUE(loaded->getKnobByName("box")->getIsSecret());
}

TEST_F(UserKnobEdit, LinksSurviveATypeChange)
{
    NodePtr blur = createBlur();
    NodePtr other = createBlur();
    ASSERT_TRUE(bool(blur && other));
    KnobDoublePtr amount = addTyped<KnobDouble>(blur, "amount");
    KnobDoublePtr partner = addTyped<KnobDouble>(other, "partner");
    KnobDoublePtr master = addTyped<KnobDouble>(blur, "master");
    KnobDoublePtr alias = addTyped<KnobDouble>(other, "alias");
    ASSERT_TRUE(bool(amount && partner && master && alias));
    ASSERT_TRUE(amount->linkTo(0, partner, 0));
    KnobLinkGroupPtr group = amount->getLinkGroup(0);
    ASSERT_TRUE(group != nullptr);
    group->setMemberNodeLink(amount.get(), 0, true);
    ASSERT_TRUE(alias->setKnobAsAliasOfThis(master, true));

    KnobIPtr replacement;
    std::string error;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), amount, KnobInt::typeNameStatic(), 1, false, &replacement, &error)) << error;
    ASSERT_TRUE(replacement != nullptr);
    EXPECT_TRUE(isLinkedTo(replacement, partner));
    KnobLinkGroupPtr replacementGroup = replacement->getLinkGroup(0);
    ASSERT_TRUE(replacementGroup != nullptr);
    EXPECT_TRUE(replacementGroup->isMemberNodeLink(replacement.get(), 0));

    recorder->commands.clear();
    for (const KnobIPtr& aliased : { KnobIPtr(master), KnobIPtr(alias) }) {
        KnobHolder* holder = aliased->getHolder();
        error.clear();
        EXPECT_EQ(eUserKnobTypeChangeFailed, changeUserKnobType(holder, aliased, KnobInt::typeNameStatic(), 1, true, nullptr, &error)) << aliased->getName();
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(aliased, holder->getKnobByName(aliased->getName()));
    }
    EXPECT_EQ(KnobIPtr(master), alias->getAliasMaster());
    EXPECT_EQ(0, recorder->userKnobCommands());
}

TEST_F(UserKnobEdit, NoOpEditsAddNoUndoEntry)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr first = addTyped<KnobDouble>(blur, "first");
    UserKnobSpec buttonSpec = makeSpec(KnobButton::typeNameStatic(), "press");
    Serialization::KnobButtonExtraSerialization script;
    script.script = "pass\n";
    buttonSpec.extra = script;
    KnobButtonPtr press = std::dynamic_pointer_cast<KnobButton>(add(blur, buttonSpec));
    ASSERT_TRUE(bool(first && press));
    startRecordingGraph();
    takeSetUserKnobsOps(blur);
    recorder->commands.clear();
    const std::uint64_t serial = recorder->serial;

    EXPECT_TRUE(applySpec(holderOf(blur), first, specFromKnob(first)));
    UserKnobSpec onUserPage = specFromKnob(first);
    onUserPage.parentName.clear();
    onUserPage.index = -1;
    onUserPage.defaults.clear();
    EXPECT_TRUE(applySpec(holderOf(blur), first, onUserPage));
    EXPECT_EQ(serial, recorder->serial);
    EXPECT_FALSE(moveUserKnob(holderOf(blur), first, true));

    const std::string node = getApp()->getAppIDString() + "." + blur->getFullyQualifiedName();
    ASSERT_TRUE(run("thisNode = " + node + "\n"
                                           "thisNode.press.setScript('pass\\n')\n"));
    EXPECT_EQ(script.script, press->getScript());
    EXPECT_EQ(0, recorder->userKnobCommands());
    EXPECT_EQ(0, takeSetUserKnobsOps(blur));

    ASSERT_TRUE(run("thisNode = " + node + "\n"
                                           "thisNode.press.setScript('x = 1\\n')\n"));
    EXPECT_EQ(1, recorder->userKnobCommands());
    undoAll();
    EXPECT_EQ(press, blur->getKnobByName("press"));
    EXPECT_EQ(script.script, press->getScript());
}

TEST_F(UserKnobEdit, LinkedReadersFollowATypeChangeAndItsUndo)
{
    NodePtr blur = createBlur();
    NodePtr other = createBlur();
    ASSERT_TRUE(bool(blur && other));
    KnobIntPtr amount = addTyped<KnobInt>(blur, "amount");
    KnobDoublePtr m1 = addTyped<KnobDouble>(other, "m1");
    KnobDoublePtr m2 = addTyped<KnobDouble>(other, "m2");
    ASSERT_TRUE(bool(amount && m1 && m2));
    ASSERT_TRUE(m1->linkTo(0, m2, 0));
    EXPECT_NO_THROW(m1->setExpression(0, blur->getScriptName() + ".amount.get()", false, true));
    ASSERT_EQ(m1->getExpression(0), m2->getExpression(0));

    const auto follows = [&](const std::string& typeName,
                             double value) {
        KnobIPtr current = blur->getKnobByName("amount");
        ASSERT_TRUE(current != nullptr);
        EXPECT_EQ(typeName, current->typeName());
        KnobIntPtr asInt = std::dynamic_pointer_cast<KnobInt>(current);
        KnobDoublePtr asDouble = std::dynamic_pointer_cast<KnobDouble>(current);
        if (asInt) {
            asInt->setValue((int)value, ViewSpec::all(), 0);
        } else {
            ASSERT_TRUE(asDouble != nullptr);
            asDouble->setValue(value, ViewSpec::all(), 0);
        }
        EXPECT_TRUE(m1->isLinked(0));
        EXPECT_TRUE(isLinkedTo(m1, m2));
        EXPECT_DOUBLE_EQ(value, m1->getValue(0));
        EXPECT_DOUBLE_EQ(value, m2->getValue(0));
    };
    {
        SCOPED_TRACE("before");
        follows(KnobInt::typeNameStatic(), 2.);
    }

    recorder->commands.clear();
    std::string error;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), amount, KnobDouble::typeNameStatic(), 1, false, nullptr, &error)) << error;
    {
        SCOPED_TRACE("changed");
        follows(KnobDouble::typeNameStatic(), 3.5);
    }
    undoAll();
    {
        SCOPED_TRACE("undone");
        follows(KnobInt::typeNameStatic(), 4.);
    }
    redoAll();
    {
        SCOPED_TRACE("redone");
        follows(KnobDouble::typeNameStatic(), 5.5);
    }
}

TEST_F(UserKnobEdit, ALinkedReaderOfARetypedKnobStaysValidThroughUndo)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(blur != nullptr);
    KnobDoublePtr p = addTyped<KnobDouble>(blur, "P");
    KnobIntPtr u = addTyped<KnobInt>(blur, "U");
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName("size"));
    ASSERT_TRUE(bool(p && u && size));
    ASSERT_TRUE(size->linkTo(0, p, 0));
    EXPECT_NO_THROW(size->setExpression(0, "thisNode.U.get()", false, true));
    ASSERT_EQ(size->getExpression(0), p->getExpression(0));

    const auto follows = [&](const std::string& typeName,
                             double value) {
        KnobIPtr current = blur->getKnobByName("U");
        ASSERT_TRUE(current != nullptr);
        EXPECT_EQ(typeName, current->typeName());
        KnobIntPtr asInt = std::dynamic_pointer_cast<KnobInt>(current);
        KnobDoublePtr asDouble = std::dynamic_pointer_cast<KnobDouble>(current);
        if (asInt) {
            asInt->setValue((int)value, ViewSpec::all(), 0);
        } else {
            ASSERT_TRUE(asDouble != nullptr);
            asDouble->setValue(value, ViewSpec::all(), 0);
        }
        std::string error;
        EXPECT_TRUE(size->isExpressionValid(0, &error)) << error;
        EXPECT_TRUE(size->isLinked(0));
        EXPECT_FALSE(size->getExpression(0).empty());
        EXPECT_DOUBLE_EQ(value, size->getValue(0));
    };
    {
        SCOPED_TRACE("before");
        follows(KnobInt::typeNameStatic(), 2.);
    }

    recorder->commands.clear();
    std::string error;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), u, KnobDouble::typeNameStatic(), 1, false, nullptr, &error)) << error;
    {
        SCOPED_TRACE("changed");
        follows(KnobDouble::typeNameStatic(), 3.5);
    }
    undoAll();
    {
        SCOPED_TRACE("undone");
        follows(KnobInt::typeNameStatic(), 4.);
    }
    redoAll();
    {
        SCOPED_TRACE("redone");
        follows(KnobDouble::typeNameStatic(), 5.5);
    }
}

TEST_F(UserKnobEdit, NodeLinksSurviveATypeChangeAndItsUndo)
{
    NodePtr blur = createBlur();
    NodePtr other = createBlur();
    ASSERT_TRUE(bool(blur && other));
    KnobDoublePtr amount = addTyped<KnobDouble>(blur, "amount");
    KnobDoublePtr partner = addTyped<KnobDouble>(other, "partner");
    ASSERT_TRUE(bool(amount && partner));
    ASSERT_TRUE(amount->linkTo(0, partner, 0));
    KnobLinkGroupPtr group = amount->getLinkGroup(0);
    ASSERT_TRUE(group != nullptr);
    group->setMemberNodeLink(amount.get(), 0, true);
    group->setMemberNodeLink(partner.get(), 0, true);

    const auto check = [&](const std::string& typeName) {
        KnobIPtr current = blur->getKnobByName("amount");
        ASSERT_TRUE(current != nullptr);
        EXPECT_EQ(typeName, current->typeName());
        EXPECT_TRUE(isLinkedTo(current, partner));
        KnobLinkGroupPtr currentGroup = current->getLinkGroup(0);
        ASSERT_TRUE(currentGroup != nullptr);
        EXPECT_EQ(currentGroup, partner->getLinkGroup(0));
        EXPECT_TRUE(currentGroup->isMemberLinked(current.get(), 0));
        EXPECT_TRUE(currentGroup->isMemberLinked(partner.get(), 0));
        EXPECT_TRUE(currentGroup->isMemberNodeLink(current.get(), 0));
        EXPECT_TRUE(currentGroup->isMemberNodeLink(partner.get(), 0));
    };

    recorder->commands.clear();
    std::string error;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), amount, KnobInt::typeNameStatic(), 1, false, nullptr, &error)) << error;
    {
        SCOPED_TRACE("changed");
        check(KnobInt::typeNameStatic());
    }
    undoAll();
    {
        SCOPED_TRACE("undone");
        check(KnobDouble::typeNameStatic());
    }
    redoAll();
    {
        SCOPED_TRACE("redone");
        check(KnobInt::typeNameStatic());
    }
}

TEST_F(UserKnobEdit, AnUnlinkedGroupMemberSurvivesATypeChange)
{
    NodePtr blur = createBlur();
    NodePtr other = createBlur();
    ASSERT_TRUE(bool(blur && other));
    KnobDoublePtr amount = addTyped<KnobDouble>(blur, "amount");
    KnobDoublePtr partner = addTyped<KnobDouble>(other, "partner");
    ASSERT_TRUE(bool(amount && partner));
    ASSERT_TRUE(amount->linkTo(0, partner, 0));
    ASSERT_TRUE(amount->unlinkFromGroups());
    amount->setValue(2., ViewSpec::all(), 0);
    partner->setValue(7., ViewSpec::all(), 0);

    KnobIPtr replacement;
    std::string error;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), amount, KnobInt::typeNameStatic(), 1, false, &replacement, &error)) << error;
    KnobIntPtr asInt = std::dynamic_pointer_cast<KnobInt>(replacement);
    ASSERT_TRUE(asInt != nullptr);
    KnobLinkGroupPtr group = replacement->getLinkGroup(0);
    ASSERT_TRUE(group != nullptr);
    EXPECT_EQ(group, partner->getLinkGroup(0));
    EXPECT_FALSE(replacement->isLinked(0));
    EXPECT_TRUE(partner->isLinked(0));
    EXPECT_EQ(2, asInt->getValue(0));
    EXPECT_DOUBLE_EQ(7., partner->getValue(0));

    ASSERT_TRUE(replacement->relinkToGroups());
    EXPECT_TRUE(replacement->isLinked(0));
    EXPECT_EQ(7, asInt->getValue(0));
    partner->setValue(9., ViewSpec::all(), 0);
    EXPECT_EQ(9, asInt->getValue(0));
}

TEST_F(UserKnobEdit, AKnobNamedLikeAnInnerNodeCanStillBeEdited)
{
    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    groupArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    groupArgs.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    NodePtr group = getApp()->createNode(groupArgs);
    ASSERT_TRUE(bool(group));
    NodeGroupPtr collection = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_TRUE(bool(collection));
    CreateNodeArgs innerArgs(PLUGINID_NATRON_BLUR, collection);
    innerArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    NodePtr inner = getApp()->createNode(innerArgs);
    ASSERT_TRUE(bool(inner));
    inner->setScriptName("shared");
    ASSERT_TRUE(bool(collection->getNodeByName("shared")));
    // Renaming the node refuses a name a parameter of the group has, and the editor refuses a new
    // parameter named like a node, but the Python creators do not look at the group's nodes.
    ASSERT_TRUE(run(getApp()->getAppIDString() + "." + group->getFullyQualifiedName() + ".createGroupParam('shared', 'Shared')\n"));
    KnobIPtr box = group->getKnobByName("shared");
    ASSERT_TRUE(box != nullptr);

    UserKnobSpec spec = specFromKnob(box);
    spec.label = "Shared Box";
    EXPECT_TRUE(validateSpec(holderOf(group), box, spec).empty()) << validateSpec(holderOf(group), box, spec);
    std::string error;
    EXPECT_TRUE(applySpec(holderOf(group), box, spec, &error)) << error;
    EXPECT_EQ("Shared Box", box->getLabel());
    EXPECT_EQ(box, group->getKnobByName("shared"));
}

TEST_F(UserKnobEdit, ChangingATypeToALabelIsOneUndoEntry)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobIntPtr count = addTyped<KnobInt>(blur, "count");
    ASSERT_TRUE(bool(count));
    count->setValue(3, ViewSpec::all(), 0);
    const std::string before = userKnobState(blur);
    recorder->commands.clear();
    const std::uint64_t serial = recorder->serial;

    Serialization::KnobTextExtraSerialization label;
    label.isLabel = true;
    KnobIPtr changed;
    std::string error;
    ASSERT_EQ(eUserKnobTypeChangeReset, changeUserKnobType(holderOf(blur), count, KnobString::typeNameStatic(), 1, true, &changed, &error, label)) << error;
    KnobStringPtr asLabel = std::dynamic_pointer_cast<KnobString>(changed);
    ASSERT_TRUE(bool(asLabel));
    EXPECT_TRUE(asLabel->isLabel());
    EXPECT_EQ(asLabel, blur->getKnobByName("count"));
    EXPECT_EQ(serial + 1, recorder->serial);
    EXPECT_EQ(1, recorder->userKnobCommands());

    undoAll();
    KnobIntPtr restored = std::dynamic_pointer_cast<KnobInt>(blur->getKnobByName("count"));
    ASSERT_TRUE(bool(restored));
    EXPECT_EQ(3, restored->getValue(0));
    EXPECT_EQ(before, userKnobState(blur));

    KnobStringPtr words = addTyped<KnobString>(blur, "words");
    ASSERT_TRUE(bool(words));
    recorder->commands.clear();
    const std::uint64_t wordsSerial = recorder->serial;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), words, KnobString::typeNameStatic(), 1, false, &changed, &error, label)) << error;
    EXPECT_EQ(KnobIPtr(words), changed);
    EXPECT_TRUE(words->isLabel());
    EXPECT_EQ(wordsSerial + 1, recorder->serial);
    undoAll();
    EXPECT_FALSE(words->isLabel());

    const std::string node = getApp()->getAppIDString() + "." + blur->getFullyQualifiedName();
    ASSERT_TRUE(run("thisNode = " + node + "\n"
                                           "assert thisNode.changeParamType(thisNode.count, 'Label') is None\n"
                                           "assert type(thisNode.changeParamType(thisNode.count, 'Label', True)).__name__ == 'StringParam'\n"
                                           "assert type(thisNode.changeParamType(thisNode.words, 'Label')).__name__ == 'StringParam'\n"));
    KnobStringPtr countLabel = std::dynamic_pointer_cast<KnobString>(blur->getKnobByName("count"));
    ASSERT_TRUE(bool(countLabel));
    EXPECT_TRUE(countLabel->isLabel());
    EXPECT_TRUE(words->isLabel());
    ASSERT_TRUE(run("thisNode = " + node + "\n"
                                           "assert type(thisNode.changeParamType(thisNode.words, 'String')).__name__ == 'StringParam'\n"));
    EXPECT_FALSE(words->isLabel());
}

TEST_F(UserKnobEdit, CopyingAKnobOfAnotherNodeLeavesTheSourceAsItIs)
{
    NodePtr source = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(bool(source && target));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(source->getKnobByName(kBlurParamSize));
    ASSERT_TRUE(bool(size));
    ASSERT_FALSE(size->isUserKnob());
    size->setValue(3., ViewSpec::all(), 0);
    size->setValue(4., ViewSpec::all(), 1);
    ASSERT_NO_THROW(size->setExpression(1, "2 + 3", false, true));
    Serialization::KnobSerialization sourceBefore;
    size->toSerialization(&sourceBefore);
    const std::string targetBefore = userKnobState(target);
    recorder->commands.clear();
    const std::uint64_t serial = recorder->serial;

    std::string error;
    KnobIPtr copy = copyKnobToHolder(holderOf(target), size, KnobIPtr(), -1, &error);
    ASSERT_TRUE(copy != nullptr) << error;
    EXPECT_EQ(serial + 1, recorder->serial);
    ASSERT_FALSE(recorder->stepTexts.empty());
    EXPECT_EQ("Copy parameter size2", recorder->stepTexts.back());
    EXPECT_EQ(1, recorder->userKnobCommands());

    Serialization::KnobSerialization sourceAfter;
    size->toSerialization(&sourceAfter);
    EXPECT_TRUE(sourceBefore == sourceAfter);
    EXPECT_EQ(std::string(), size->getExpression(0));
    EXPECT_EQ("2 + 3", size->getExpression(1));
    KnobI::ListenerDimsMap listeners;
    size->getListeners(listeners);
    EXPECT_TRUE(listeners.empty());

    KnobDoublePtr copied = std::dynamic_pointer_cast<KnobDouble>(copy);
    ASSERT_TRUE(bool(copied));
    EXPECT_EQ(copy, target->getKnobByName("size2"));
    EXPECT_TRUE(copied->isUserKnob());
    EXPECT_EQ(2, copied->getDimension());
    EXPECT_DOUBLE_EQ(3., copied->getValue(0));
    EXPECT_EQ("2 + 3", copied->getExpression(1));
    EXPECT_DOUBLE_EQ(5., copied->getValue(1));
    EXPECT_DOUBLE_EQ(1000., copied->getMaximum(0));
    EXPECT_DOUBLE_EQ(100., copied->getDisplayMaximum(0));
    EXPECT_FALSE(isLinkedTo(copy, size));
    EXPECT_FALSE(isLinkedTo(size, copy));

    undoAll();
    EXPECT_FALSE(bool(target->getKnobByName("size2")));
    EXPECT_EQ(targetBefore, userKnobState(target));
    Serialization::KnobSerialization sourceUndone;
    size->toSerialization(&sourceUndone);
    EXPECT_TRUE(sourceBefore == sourceUndone);
}

TEST_F(UserKnobEdit, ACopyDoesNotFollowTheSourceOrWhatItIsLinkedTo)
{
    NodePtr source = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(bool(source && target));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(source->getKnobByName(kBlurParamSize));
    ASSERT_TRUE(bool(size));
    KnobIPtr linked = size->createDuplicateOnHolder(holderOf(target), KnobPagePtr(), KnobGroupPtr(), -1, true, "size",
                                                    size->getLabel(), size->getHintToolTip(), true, true);
    ASSERT_TRUE(bool(linked));
    ASSERT_EQ(linked, size->getAliasMaster());
    size->setValue(3., ViewSpec::all(), 0);
    size->setValue(4., ViewSpec::all(), 1);
    KnobDoublePtr linkedSize = std::dynamic_pointer_cast<KnobDouble>(linked);
    ASSERT_TRUE(bool(linkedSize));
    ASSERT_DOUBLE_EQ(3., linkedSize->getValue(0));

    std::string error;
    KnobIPtr copy = copyKnobToHolder(holderOf(target), size, KnobIPtr(), -1, &error);
    ASSERT_TRUE(copy != nullptr) << error;
    KnobDoublePtr copied = std::dynamic_pointer_cast<KnobDouble>(copy);
    ASSERT_TRUE(bool(copied));
    EXPECT_EQ(copy, target->getKnobByName("size2"));
    EXPECT_DOUBLE_EQ(3., copied->getValue(0));
    EXPECT_DOUBLE_EQ(4., copied->getValue(1));
    EXPECT_FALSE(bool(copied->getAliasMaster()));
    for (int d = 0; d < copied->getDimension(); ++d) {
        EXPECT_FALSE(bool(copied->getMaster(d).second));
        EXPECT_EQ(std::string(), copied->getExpression(d));
    }
    EXPECT_FALSE(isLinkedTo(copy, size));
    EXPECT_FALSE(isLinkedTo(copy, linked));
    EXPECT_FALSE(isLinkedTo(size, copy));
    EXPECT_FALSE(isLinkedTo(linked, copy));

    size->setValue(11., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(11., linkedSize->getValue(0));
    EXPECT_DOUBLE_EQ(3., copied->getValue(0));
    linkedSize->setValue(13., ViewSpec::all(), 1);
    EXPECT_DOUBLE_EQ(13., size->getValue(1));
    EXPECT_DOUBLE_EQ(4., copied->getValue(1));
}

TEST_F(UserKnobEdit, ACopyKeepsOnlyTheExpressionsReadingItself)
{
    NodePtr source = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(bool(source && target));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(source->getKnobByName(kBlurParamSize));
    KnobDoublePtr amount = addTyped<KnobDouble>(source, "amount", 3);
    KnobDoublePtr other = addTyped<KnobDouble>(source, "other");
    ASSERT_TRUE(bool(size && amount && other));
    size->setValue(6., ViewSpec::all(), 0);
    amount->setValue(2., ViewSpec::all(), 0);
    ASSERT_NO_THROW(amount->setExpression(1, "thisNode.amount.getValue(0) * 2", false, true));
    ASSERT_NO_THROW(amount->setExpression(2, source->getScriptName_mt_safe() + ".amount.getValue(0) + 1", false, true));
    ASSERT_NO_THROW(other->setExpression(0, "thisNode.size.getValue(0)", false, true));
    ASSERT_DOUBLE_EQ(6., other->getValue(0));

    std::string error;
    KnobDoublePtr amountCopy = std::dynamic_pointer_cast<KnobDouble>(copyKnobToHolder(holderOf(target), amount, KnobIPtr(), -1, &error));
    ASSERT_TRUE(bool(amountCopy)) << error;
    KnobDoublePtr otherCopy = std::dynamic_pointer_cast<KnobDouble>(copyKnobToHolder(holderOf(target), other, KnobIPtr(), -1, &error));
    ASSERT_TRUE(bool(otherCopy)) << error;

    EXPECT_EQ(std::string(), amountCopy->getExpression(0));
    EXPECT_EQ("thisNode.amount.getValue(0) * 2", amountCopy->getExpression(1));
    EXPECT_EQ(std::string(), amountCopy->getExpression(2));
    EXPECT_EQ(std::string(), otherCopy->getExpression(0));
    EXPECT_DOUBLE_EQ(2., amountCopy->getValue(0));
    EXPECT_DOUBLE_EQ(4., amountCopy->getValue(1));
    EXPECT_DOUBLE_EQ(3., amountCopy->getValue(2));
    EXPECT_DOUBLE_EQ(6., otherCopy->getValue(0));

    amount->setValue(10., ViewSpec::all(), 0);
    size->setValue(9., ViewSpec::all(), 0);
    KnobDoublePtr targetSize = std::dynamic_pointer_cast<KnobDouble>(target->getKnobByName(kBlurParamSize));
    ASSERT_TRUE(bool(targetSize));
    targetSize->setValue(8., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(2., amountCopy->getValue(0));
    EXPECT_DOUBLE_EQ(4., amountCopy->getValue(1));
    EXPECT_DOUBLE_EQ(3., amountCopy->getValue(2));
    EXPECT_DOUBLE_EQ(6., otherCopy->getValue(0));
}

TEST_F(UserKnobEdit, AChoiceWithoutEntriesCanBeEdited)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobChoicePtr menu = addTyped<KnobChoice>(blur, "menu");
    ASSERT_TRUE(bool(menu));
    ASSERT_TRUE(menu->getEntries_mt_safe().empty());

    UserKnobSpec spec = specFromKnob(menu);
    EXPECT_TRUE(spec.defaults.empty());
    EXPECT_EQ(std::string(), validateSpec(holderOf(blur), menu, spec));
    spec.label = "Menu";
    std::string error;
    ASSERT_TRUE(applySpec(holderOf(blur), menu, spec, &error)) << error;
    EXPECT_EQ("Menu", menu->getLabel());

    Serialization::KnobChoiceExtraSerialization entries;
    entries.entries.push_back("first");
    entries.entries.push_back("second");
    entries.helpStrings.resize(2);
    spec.extra = entries;
    ASSERT_TRUE(applySpec(holderOf(blur), menu, spec, &error)) << error;
    EXPECT_EQ(2u, menu->getEntries_mt_safe().size());
    const UserKnobSpec withEntries = specFromKnob(menu);
    ASSERT_EQ(1u, withEntries.defaults.size());
    const std::string* id = std::get_if<std::string>(&withEntries.defaults[0]);
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ("first", *id);
}

TEST_F(UserKnobEdit, AnEditWhosePlacementFailsChangesNothing)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    ASSERT_TRUE(bool(add(blur, makeSpec(KnobPage::typeNameStatic(), "tools"))));
    KnobGroupPtr vars = std::dynamic_pointer_cast<KnobGroup>(add(blur, makeSpec(KnobGroup::typeNameStatic(), "vars", 1, "tools")));
    KnobIPtr press = add(blur, makeSpec(KnobButton::typeNameStatic(), "press", 1, "vars"));
    ASSERT_TRUE(bool(vars && press));
    ASSERT_EQ(KnobIPtr(vars), press->getParentKnob());

    UserKnobSpec spec = specFromKnob(press);
    spec.name = "push";
    spec.label = "Push";
    spec.parentName = "tools";
    spec.index = -1;
    std::string error;
    recorder->commands.clear();
    ASSERT_TRUE(applySpec(holderOf(blur), press, spec, &error)) << error;
    ASSERT_EQ(1, recorder->userKnobCommands());
    ASSERT_EQ(blur->getKnobByName("tools"), press->getParentKnob());

    // The group now refuses the button, so undoing the edit cannot put it back there.
    vars->setAsRFVGroup(true);
    const std::string state = userKnobState(blur);
    undoAll();
    EXPECT_EQ(press, blur->getKnobByName("push"));
    EXPECT_FALSE(bool(blur->getKnobByName("press")));
    EXPECT_EQ("Push", press->getLabel());
    EXPECT_EQ(blur->getKnobByName("tools"), press->getParentKnob());
    EXPECT_TRUE(vars->getChildren().empty());
    EXPECT_EQ(state, userKnobState(blur));
}

TEST_F(UserKnobEdit, ARenameRewritesTheKnobsOwnExpressionsAndButtonScripts)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr pos = addTyped<KnobDouble>(blur, "pos", 2);
    ASSERT_TRUE(bool(pos));
    ASSERT_NO_THROW(pos->setExpression(1, "thisNode.pos.getValue(0) * 2", false, true));
    UserKnobSpec buttonSpec = makeSpec(KnobButton::typeNameStatic(), "press");
    Serialization::KnobButtonExtraSerialization script;
    script.script = "thisNode.pos.setValue(4.5, 0)\npressed = thisNode.press\nlabel = 'thisNode.pos'\n";
    buttonSpec.extra = script;
    KnobButtonPtr press = std::dynamic_pointer_cast<KnobButton>(add(blur, buttonSpec));
    ASSERT_TRUE(bool(press));
    recorder->commands.clear();

    UserKnobSpec spec = specFromKnob(pos);
    spec.name = "position";
    std::string error;
    ASSERT_TRUE(applySpec(holderOf(blur), pos, spec, &error)) << error;
    EXPECT_EQ("thisNode.position.getValue(0) * 2", pos->getExpression(1));
    EXPECT_EQ("thisNode.position.setValue(4.5, 0)\npressed = thisNode.press\nlabel = 'thisNode.pos'\n", press->getScript());

    UserKnobSpec pressSpec = specFromKnob(press);
    pressSpec.name = "push";
    ASSERT_TRUE(applySpec(holderOf(blur), press, pressSpec, &error)) << error;
    EXPECT_EQ("thisNode.position.setValue(4.5, 0)\npressed = thisNode.push\nlabel = 'thisNode.pos'\n", press->getScript());

    EXPECT_TRUE(press->runScript());
    EXPECT_DOUBLE_EQ(4.5, pos->getValue(0));
    EXPECT_DOUBLE_EQ(9., pos->getValue(1));

    undoAll();
    EXPECT_EQ(KnobIPtr(pos), blur->getKnobByName("pos"));
    EXPECT_EQ(KnobIPtr(press), blur->getKnobByName("press"));
    EXPECT_EQ("thisNode.pos.getValue(0) * 2", pos->getExpression(1));
    EXPECT_EQ(script.script, press->getScript());
}

namespace {

// The properties of knob a user knob keeps beyond its spec, as text.
std::string
userProperties(const KnobIPtr& knob)
{
    std::string ret = std::string("newLine=") + (knob->isNewLineActivated() ? "1" : "0");
    for (int i = 0; i < knob->getDimension(); ++i) {
        ret += " name" + std::to_string(i) + "=" + knob->getDimensionName(i);
    }
    if (KnobInt* isInt = dynamic_cast<KnobInt*>(knob.get())) {
        ret += std::string(" noSlider=") + (isInt->isSliderDisabled() ? "1" : "0");
        ret += std::string(" rectangle=") + (isInt->isRectangle() ? "1" : "0");
    } else if (KnobDouble* isDouble = dynamic_cast<KnobDouble*>(knob.get())) {
        ret += std::string(" noSlider=") + (isDouble->isSliderDisabled() ? "1" : "0");
        ret += std::string(" rectangle=") + (isDouble->isRectangle() ? "1" : "0");
        ret += std::string(" spatial=") + (isDouble->getIsSpatial() ? "1" : "0");
        for (int i = 0; i < knob->getDimension(); ++i) {
            ret += " normalized" + std::to_string(i) + "=" + std::to_string((int)isDouble->getValueIsNormalized(i));
        }
    } else if (KnobString* isString = dynamic_cast<KnobString*>(knob.get())) {
        ret += " rows=" + std::to_string(isString->getPreferredRows());
    } else if (KnobLayerSelect* isLayerSelect = dynamic_cast<KnobLayerSelect*>(knob.get())) {
        ret += std::string(" allowNone=") + (isLayerSelect->getAllowNone() ? "1" : "0");
        ret += std::string(" allowAll=") + (isLayerSelect->getAllowAll() ? "1" : "0");
        ret += " allLabel=" + isLayerSelect->getAllLabel();
    }

    return ret;
}
} // anon namespace

TEST_F(UserKnobEdit, KnobPropertiesSurviveCopyPasteAndSaveAndLoad)
{
    NodePtr source = createBlur();
    NodePtr copies = createBlur();
    NodePtr pastes = createBlur();
    ASSERT_TRUE(bool(source && copies && pastes));
    KnobHolder* sourceHolder = holderOf(source);

    KnobIntPtr area = AppManager::createKnob<KnobInt>(sourceHolder, std::string("Area"), 4, false);
    area->setName("propArea");
    area->setAsRectangle();
    area->setAddNewLine(false);
    KnobDoublePtr box = AppManager::createKnob<KnobDouble>(sourceHolder, std::string("Box"), 4, false);
    box->setName("propBox");
    box->setAsRectangle();
    box->setSpatial(true);
    box->setValueIsNormalized(0, eValueIsNormalizedX);
    box->setValueIsNormalized(1, eValueIsNormalizedY);
    box->setValueIsNormalized(2, eValueIsNormalizedX);
    box->setValueIsNormalized(3, eValueIsNormalizedY);
    ASSERT_FALSE(area->isUserKnob());
    ASSERT_FALSE(box->isUserKnob());

    KnobIntPtr count = addTyped<KnobInt>(source, "propCount");
    KnobDoublePtr extent = addTyped<KnobDouble>(source, "propExtent", 2);
    KnobStringPtr notes = addTyped<KnobString>(source, "propNotes");
    KnobLayerSelectPtr pick = addTyped<KnobLayerSelect>(source, "propPick");
    ASSERT_TRUE(bool(count && extent && notes && pick));
    count->disableSlider();
    extent->disableSlider();
    extent->setSpatial(true);
    extent->setValueIsNormalized(1, eValueIsNormalizedY);
    extent->setDimensionName(0, "w");
    extent->setDimensionName(1, "h");
    extent->setAddNewLine(false);
    notes->setAsMultiLine();
    notes->setPreferredRows(7);
    pick->setAllowNone(true);
    pick->setAllowAll(true);
    pick->setAllLabel("Matching");

    const std::vector<KnobIPtr> sources = { area, box, count, extent, notes, pick };
    std::map<std::string, std::string> before;
    for (const KnobIPtr& knob : sources) {
        before[knob->getName()] = userProperties(knob);
    }
    EXPECT_EQ("newLine=0 name0=x name1=y name2=z name3=w noSlider=1 rectangle=1", before["propArea"]);
    EXPECT_EQ("newLine=1 name0=x name1=y name2=z name3=w noSlider=0 rectangle=1 spatial=1 normalized0=1 normalized1=2 normalized2=1 normalized3=2", before["propBox"]);
    EXPECT_EQ("newLine=1 name0=x noSlider=1 rectangle=0", before["propCount"]);
    EXPECT_EQ("newLine=0 name0=w name1=h noSlider=1 rectangle=0 spatial=1 normalized0=0 normalized1=2", before["propExtent"]);
    EXPECT_EQ("newLine=1 name0=x rows=7", before["propNotes"]);
    EXPECT_EQ("newLine=1 name0=x allowNone=1 allowAll=1 allLabel=Matching", before["propPick"]);

    for (const KnobIPtr& knob : sources) {
        std::string error;
        KnobIPtr copy = copyKnobToHolder(holderOf(copies), knob, KnobIPtr(), -1, &error);
        ASSERT_TRUE(copy != nullptr) << knob->getName() << ": " << error;
        EXPECT_EQ(knob->getName(), copy->getName());
        EXPECT_TRUE(copy->isUserKnob());
        EXPECT_EQ(before[knob->getName()], userProperties(copy)) << "copied " << knob->getName();
    }

    std::list<KnobPage*> copyPages;
    holderOf(copies)->getUserPages(copyPages);
    ASSERT_EQ(1u, copyPages.size());
    const std::string text = copyUserKnobs(holderOf(copies), { holderOf(copies)->getKnobByName(copyPages.front()->getName()) });
    std::string error;
    const std::list<KnobIPtr> created = pasteUserKnobs(holderOf(pastes), text, KnobIPtr(), -1, &error);
    ASSERT_EQ(1u, created.size()) << error;
    for (const std::pair<const std::string, std::string>& entry : before) {
        KnobIPtr pasted = pastes->getKnobByName(entry.first);
        ASSERT_TRUE(bool(pasted)) << entry.first;
        EXPECT_EQ(entry.second, userProperties(pasted)) << "pasted " << entry.first;
    }

    const std::string copiesName = copies->getScriptName();
    const std::string pastesName = pastes->getScriptName();
    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("user-knob-properties.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));
    for (const std::string& nodeName : { copiesName, pastesName }) {
        NodePtr loaded = project->getNodeByName(nodeName);
        ASSERT_TRUE(bool(loaded)) << nodeName;
        for (const std::pair<const std::string, std::string>& entry : before) {
            KnobIPtr knob = loaded->getKnobByName(entry.first);
            ASSERT_TRUE(bool(knob)) << nodeName << "." << entry.first;
            EXPECT_TRUE(knob->isUserKnob()) << nodeName << "." << entry.first;
            EXPECT_EQ(entry.second, userProperties(knob)) << "loaded " << nodeName << "." << entry.first;
        }
    }
}

TEST_F(UserKnobEdit, ATypeChangeKeepsOnlyThePropertiesTheNewTypeAndDimensionHave)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr extent = addTyped<KnobDouble>(blur, "propExtent", 2);
    ASSERT_TRUE(bool(extent));
    extent->disableSlider();
    extent->setSpatial(true);
    extent->setValueIsNormalized(0, eValueIsNormalizedX);
    extent->setDimensionName(0, "w");

    KnobIPtr widened;
    std::string error;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), extent, KnobDouble::typeNameStatic(), 3, false, &widened, &error)) << error;
    ASSERT_TRUE(bool(widened));
    EXPECT_EQ("newLine=1 name0=x name1=y name2=z noSlider=1 rectangle=0 spatial=1 normalized0=0 normalized1=0 normalized2=0", userProperties(widened));

    KnobIPtr retyped;
    ASSERT_EQ(eUserKnobTypeChangeConverted, changeUserKnobType(holderOf(blur), widened, KnobInt::typeNameStatic(), 3, false, &retyped, &error)) << error;
    ASSERT_TRUE(bool(retyped));
    EXPECT_EQ("newLine=1 name0=x name1=y name2=z noSlider=0 rectangle=0", userProperties(retyped));
}
