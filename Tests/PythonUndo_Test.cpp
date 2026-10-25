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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoRecorder.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

// Keeps what is recorded, the way the window's history would within one step.
class CollectingRecorder
    : public UndoRecorderI {
public:
    CollectingRecorder()
        : UndoRecorderI()
        , depth(0)
        , serial(0)
        , commands()
    {
    }

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
                        const UndoTouched& touched) OVERRIDE FINAL
    {
        commands.push_back(command);
        touchedNodes.push_back(touched.nodes.empty() ? NodePtr() : touched.nodes.front());
    }

    int depth;
    std::uint64_t serial;
    std::vector<UndoCommandPtr> commands;
    std::vector<NodePtr> touchedNodes;
};

class PythonUndo
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        recorder = std::make_shared<CollectingRecorder>();
        getApp()->setUndoRecorder(recorder);
    }

    virtual void TearDown() OVERRIDE
    {
        // Records of removed or undone nodes destroy them, which must happen before the reset.
        recorder->commands.clear();
        recorder->touchedNodes.clear();
        getApp()->setUndoRecorder(UndoRecorderPtr());
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createBlur()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
        // Whatever node creation recorded is not under test.
        recorder->commands.clear();
        recorder->touchedNodes.clear();

        return node;
    }

    std::string py(const NodePtr& node) const
    {
        return getApp()->getAppIDString() + "." + node->getFullyQualifiedName();
    }

    // Runs script as one step, the way the Script Editor does.
    bool runStep(const std::string& script)
    {
        UndoStepGuard step(getApp(), "test");

        return run(script);
    }

    bool run(const std::string& script)
    {
        std::string error;
        std::string output;
        bool ok = interpretPythonScript(script, &error, &output);
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

    static KnobDoublePtr sizeOf(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kBlurParamSize));
    }

    static KnobBoolPtr cropOf(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kBlurParamCropToFormat));
    }

    std::shared_ptr<CollectingRecorder> recorder;
};
} // anon namespace

TEST_F(PythonUndo, DestroyKeepsTheNodeAndUndoReconnectsIt)
{
    NodePtr input = createBlur();
    NodePtr removed = createBlur();
    NodePtr output = createBlur();
    ASSERT_TRUE(input && removed && output);
    ASSERT_TRUE(removed->connectInput(input, 0));
    ASSERT_TRUE(output->connectInput(removed, 0));
    NodeWPtr weakRemoved = removed;
    const std::string removedPy = py(removed);
    removed.reset();

    ASSERT_TRUE(runStep(removedPy + ".destroy()\n"));
    ASSERT_EQ((std::size_t)1, recorder->commands.size());
    NodePtr kept = weakRemoved.lock();
    ASSERT_TRUE(kept != nullptr);
    EXPECT_FALSE(kept->isActivated());
    // Destroying reconnects the output to the removed node's input.
    EXPECT_EQ(input, output->getInput(0));

    undoAll();
    EXPECT_TRUE(kept->isActivated());
    EXPECT_EQ(kept, output->getInput(0));
    EXPECT_EQ(input, kept->getInput(0));

    redoAll();
    EXPECT_FALSE(kept->isActivated());
    EXPECT_EQ(input, output->getInput(0));

    undoAll();
    EXPECT_TRUE(kept->isActivated());
    EXPECT_EQ(kept, output->getInput(0));
}

TEST_F(PythonUndo, WithoutARecorderDestroyDestroys)
{
    NodePtr node = createBlur();
    ASSERT_TRUE(node != nullptr);
    getApp()->setUndoRecorder(UndoRecorderPtr());
    const std::string name = node->getScriptName();
    NodeWPtr weak = node;
    const std::string nodePy = py(node);
    node.reset();

    ASSERT_TRUE(runStep(nodePy + ".destroy()\n"));
    EXPECT_TRUE(recorder->commands.empty());
    EXPECT_FALSE(getApp()->getProject()->getNodeByName(name) != nullptr);
    NodePtr still = weak.lock();
    EXPECT_TRUE(!still || !still->isActivated());
}

TEST_F(PythonUndo, CreateNodeUndoesAndRedoes)
{
    const std::string script = "n = " + getApp()->getAppIDString() + ".createNode(\"" PLUGINID_NATRON_BLUR "\")\n";
    ASSERT_TRUE(runStep(script));
    ASSERT_EQ((std::size_t)1, recorder->commands.size());
    NodePtr created = recorder->touchedNodes.front();
    ASSERT_TRUE(created != nullptr);
    EXPECT_TRUE(created->isActivated());

    undoAll();
    EXPECT_FALSE(created->isActivated());
    redoAll();
    EXPECT_TRUE(created->isActivated());
}

TEST_F(PythonUndo, ConnectAndRenameAreRestored)
{
    NodePtr a = createBlur();
    NodePtr b = createBlur();
    ASSERT_TRUE(a && b);
    const std::string name = b->getScriptName();
    const std::string label = b->getLabel();
    const std::string bPy = py(b);

    ASSERT_TRUE(runStep(bPy + ".connectInput(0, " + py(a) + ")\n" + bPy + ".setLabel(\"Renamed label\")\n" + bPy + ".setScriptName(\"Renamed\")\n"));
    EXPECT_EQ(a, b->getInput(0));
    EXPECT_EQ(std::string("Renamed"), b->getScriptName());

    undoAll();
    EXPECT_FALSE(b->getInput(0) != nullptr);
    EXPECT_EQ(name, b->getScriptName());
    EXPECT_EQ(label, b->getLabel());

    redoAll();
    EXPECT_EQ(a, b->getInput(0));
    EXPECT_EQ(std::string("Renamed"), b->getScriptName());
    EXPECT_EQ(std::string("Renamed label"), b->getLabel());
}

TEST_F(PythonUndo, ValuesKeyframesExpressionsAndLinksAreRestoredExactly)
{
    NodePtr target = createBlur();
    NodePtr source = createBlur();
    ASSERT_TRUE(target && source);
    KnobDoublePtr size = sizeOf(target);
    KnobBoolPtr crop = cropOf(target);
    ASSERT_TRUE(size && crop);
    const double before0 = size->getValue(0);
    const double before1 = size->getValue(1);
    const bool cropBefore = crop->getValue(0);
    const std::string t = py(target);

    ASSERT_TRUE(runStep(t + ".size.setValue(3.0, 0)\n" + t + ".size.setValueAtTime(1.0, 1, 0)\n" + t + ".size.setValueAtTime(2.0, 10, 0)\n" + t + ".size.setExpression(\"4.5\", False, 1)\n" + t + ".getParam(\"" kBlurParamCropToFormat "\").slaveTo(" + py(source) + ".getParam(\"" kBlurParamCropToFormat "\"), 0, 0)\n"));
    ASSERT_EQ(2, size->getKeyFramesCount(ViewSpec::current(), 0));
    ASSERT_EQ(std::string("4.5"), size->getExpression(1));
    ASSERT_TRUE(crop->isSlave(0));

    undoAll();
    EXPECT_EQ(0, size->getKeyFramesCount(ViewSpec::current(), 0));
    EXPECT_EQ(before0, size->getValue(0));
    EXPECT_EQ(before1, size->getValue(1));
    EXPECT_TRUE(size->getExpression(1).empty());
    EXPECT_FALSE(crop->isSlave(0));
    EXPECT_EQ(cropBefore, crop->getValue(0));

    redoAll();
    EXPECT_EQ(2, size->getKeyFramesCount(ViewSpec::current(), 0));
    EXPECT_EQ(std::string("4.5"), size->getExpression(1));
    EXPECT_TRUE(crop->isSlave(0));

    undoAll();
    EXPECT_EQ(0, size->getKeyFramesCount(ViewSpec::current(), 0));
    EXPECT_FALSE(crop->isSlave(0));
}

TEST_F(PythonUndo, AKeyframeLoopIsOneRecord)
{
    NodePtr node = createBlur();
    ASSERT_TRUE(node != nullptr);
    ASSERT_TRUE(runStep("for i in range(100):\n    " + py(node) + ".size.setValueAtTime(float(i), i, 0)\n"));
    EXPECT_EQ((std::size_t)1, recorder->commands.size());
    EXPECT_EQ(100, sizeOf(node)->getKeyFramesCount(ViewSpec::current(), 0));

    undoAll();
    EXPECT_EQ(0, sizeOf(node)->getKeyFramesCount(ViewSpec::current(), 0));
}

TEST_F(PythonUndo, EachStepRecordsTheKnobAgain)
{
    NodePtr node = createBlur();
    ASSERT_TRUE(node != nullptr);
    ASSERT_TRUE(runStep(py(node) + ".size.setValue(2.0, 0)\n"));
    ASSERT_TRUE(runStep(py(node) + ".size.setValue(5.0, 0)\n"));
    ASSERT_EQ((std::size_t)2, recorder->commands.size());

    UndoReplayGuard replaying(getApp());
    recorder->commands.back()->undo();
    EXPECT_EQ(2.0, sizeOf(node)->getValue(0));
}

TEST_F(PythonUndo, UserParamsRoundTrip)
{
    NodePtr node = createBlur();
    ASSERT_TRUE(node != nullptr);
    const std::string n = py(node);

    ASSERT_TRUE(runStep(n + ".createDoubleParam(\"pyUndoParam\", \"Py Undo Param\")\n" + n + ".refreshUserParamsGUI()\n"));
    ASSERT_TRUE(node->getKnobByName("pyUndoParam") != nullptr);
    undoAll();
    EXPECT_FALSE(node->getKnobByName("pyUndoParam") != nullptr);
    redoAll();
    ASSERT_TRUE(node->getKnobByName("pyUndoParam") != nullptr);

    recorder->commands.clear();
    ASSERT_TRUE(runStep(n + ".removeParam(" + n + ".getParam(\"pyUndoParam\"))\n"));
    EXPECT_FALSE(node->getKnobByName("pyUndoParam") != nullptr);
    undoAll();
    EXPECT_TRUE(node->getKnobByName("pyUndoParam") != nullptr);
}

TEST_F(PythonUndo, NothingIsRecordedOutsideAStepUnderSuppressionOrWhileCreatingANodeTree)
{
    NodePtr node = createBlur();
    ASSERT_TRUE(node != nullptr);
    const std::string script = py(node) + ".size.setValue(6.0, 0)\n";

    ASSERT_TRUE(run(script));
    EXPECT_TRUE(recorder->commands.empty());

    {
        UndoStepGuard step(getApp(), "test");
        UndoSuppressGuard suppressed(getApp());
        ASSERT_TRUE(run(script));
    }
    EXPECT_TRUE(recorder->commands.empty());

    {
        // What PyPlugs and toolsets run while they build their nodes.
        UndoStepGuard step(getApp(), "test");
        CreatingNodeTreeFlag_RAII creating(getApp());
        ASSERT_TRUE(run(script));
    }
    EXPECT_TRUE(recorder->commands.empty());
}
