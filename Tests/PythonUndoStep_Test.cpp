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

#include <functional>
#include <memory>
#include <string>

#include <QString>
#include <QThreadPool>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Engine/UndoRecorder.h"
#include "Engine/ViewIdx.h"

#include "Gui/PythonUndoRecorder.h"
#include "Gui/UndoHistory.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

// The window's history and recorder, wired the way GuiAppInstance wires them, on the tests' app.
class PythonUndoStep
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        _app = appPTR->getTopLevelInstance();
        ASSERT_TRUE(bool(_app));
        _app->getProject()->reset(false, true);
        _history.reset(new UndoHistory);
        _app->setUndoRecorder(std::make_shared<PythonUndoRecorder>(_history.get(), _app));
        AppInstanceWPtr app = _app;
        _history->setReplayHook([app](bool replaying) {
            AppInstancePtr locked = app.lock();
            if (locked) {
                locked->setReplayingUndo(replaying);
            }
        });
        _node = createBlur();
        ASSERT_TRUE(bool(_node));
        _size = std::dynamic_pointer_cast<KnobDouble>(_node->getKnobByName(kBlurParamSize));
        _crop = std::dynamic_pointer_cast<KnobBool>(_node->getKnobByName(kBlurParamCropToFormat));
        ASSERT_TRUE(_size && _crop);
        _size->setValue(1.0, ViewSpec::all(), 0);
        _size->setValue(1.0, ViewSpec::all(), 1);
        _crop->setValue(true);
        ASSERT_EQ(0, _history->count());
    }

    virtual void TearDown() OVERRIDE
    {
        if (_app) {
            _app->setUndoRecorder(UndoRecorderPtr());
        }
        // Entries hold the nodes they touch, which must go before the project resets.
        if (_history) {
            _history->setReplayHook(std::function<void(bool)>());
            _history->clear();
        }
        _size.reset();
        _crop.reset();
        _node.reset();
        if (_app) {
            _app->getProject()->reset(false, true);
        }
        _history.reset();
        QThreadPool::globalInstance()->waitForDone();
    }

    NodePtr createBlur()
    {
        CreateNodeArgs args(PLUGINID_NATRON_BLUR, _app->getProject());

        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, PLUGIN_MAJOR_NATRON_BLUR, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);

        return _app->createNode(args);
    }

    std::string appPy() const
    {
        return _app->getAppIDString();
    }

    std::string nodePy() const
    {
        return appPy() + "." + _node->getFullyQualifiedName();
    }

    std::string setSize(double value,
                        int dimension,
                        const std::string& indent) const
    {
        return indent + nodePy() + ".getParam(\"" kBlurParamSize "\").setValue(" + std::to_string(value) + ", " + std::to_string(dimension) + ")\n";
    }

    std::string setCrop(bool value,
                        const std::string& indent) const
    {
        return indent + nodePy() + ".getParam(\"" kBlurParamCropToFormat "\").setValue(" + (value ? "True" : "False") + ")\n";
    }

    bool run(const std::string& script,
             std::string* error = 0)
    {
        std::string err;
        std::string output;
        bool ok = interpretPythonScript(script, &err, &output);
        if (error) {
            *error = err;
        } else if (!ok) {
            ADD_FAILURE() << script << "\n"
                          << err;
        }

        return ok;
    }

    AppInstancePtr _app;
    std::unique_ptr<UndoHistory> _history;
    NodePtr _node;
    KnobDoublePtr _size;
    KnobBoolPtr _crop;
};
} // anon namespace

TEST_F(PythonUndoStep, ABlockIsOneEntryThatUndoesAndRedoesInOneStep)
{
    const std::string script = "with " + appPy() + ".newUndoStep(\"Three edits\"):\n"
        + setSize(2.0, 0, "    ")
        + setSize(3.0, 1, "    ")
        + setCrop(false, "    ");
    ASSERT_TRUE(run(script));

    EXPECT_FALSE(_history->isStepOpen());
    ASSERT_EQ(1, _history->count());
    EXPECT_EQ(QString::fromUtf8("Three edits"), _history->undoText());
    EXPECT_EQ(2.0, _size->getValue(0));
    EXPECT_EQ(3.0, _size->getValue(1));
    EXPECT_FALSE(_crop->getValue());

    _history->undo();
    EXPECT_EQ(1.0, _size->getValue(0));
    EXPECT_EQ(1.0, _size->getValue(1));
    EXPECT_TRUE(_crop->getValue());
    EXPECT_FALSE(_history->canUndo());

    _history->redo();
    EXPECT_EQ(2.0, _size->getValue(0));
    EXPECT_EQ(3.0, _size->getValue(1));
    EXPECT_FALSE(_crop->getValue());
    EXPECT_EQ(1, _history->count());
}

TEST_F(PythonUndoStep, ANestedStepFoldsIntoTheOuterOne)
{
    const std::string script = "with " + appPy() + ".newUndoStep(\"Outer\"):\n"
        + setSize(2.0, 0, "    ")
        + "    with " + appPy() + ".newUndoStep(\"Inner\"):\n"
        + setCrop(false, "        ");
    ASSERT_TRUE(run(script));

    EXPECT_FALSE(_history->isStepOpen());
    ASSERT_EQ(1, _history->count());
    EXPECT_EQ(QString::fromUtf8("Outer"), _history->undoText());

    _history->undo();
    EXPECT_EQ(1.0, _size->getValue(0));
    EXPECT_TRUE(_crop->getValue());
}

TEST_F(PythonUndoStep, ABlockEditingOneKnobKeepsItsLabel)
{
    const std::string script = "with " + appPy() + ".newUndoStep(\"One knob\"):\n"
        + setSize(2.0, 0, "    ")
        + setSize(3.0, 0, "    ");
    ASSERT_TRUE(run(script));

    ASSERT_EQ(1, _history->count());
    EXPECT_EQ(QString::fromUtf8("One knob"), _history->undoText());

    _history->undo();
    EXPECT_EQ(1.0, _size->getValue(0));
    EXPECT_EQ(QString::fromUtf8("One knob"), _history->redoText());
}

TEST_F(PythonUndoStep, ANestedBlockEditingOneKnobKeepsTheOuterLabel)
{
    const std::string script = "with " + appPy() + ".newUndoStep(\"Outer\"):\n"
        + "    with " + appPy() + ".newUndoStep(\"Inner\"):\n"
        + setCrop(false, "        ");
    ASSERT_TRUE(run(script));

    ASSERT_EQ(1, _history->count());
    EXPECT_EQ(QString::fromUtf8("Outer"), _history->undoText());
}

TEST_F(PythonUndoStep, AnUnlabelledStepWithOneEditKeepsTheEditsText)
{
    {
        UndoStepGuard step(_app, "Script");
        ASSERT_TRUE(run(setCrop(false, "")));
    }

    ASSERT_EQ(1, _history->count());
    EXPECT_EQ(QString::fromUtf8(("Edit " + _crop->getLabel()).c_str()), _history->undoText());
}

// The suppressed edit is not the step's: undo leaves it in place and redo does not replay it.
TEST_F(PythonUndoStep, NoUndoStepEditOfAnotherDimensionOfAStepsKnobSurvivesUndoAndRedo)
{
    const std::string script = "with " + appPy() + ".newUndoStep(\"Partly\"):\n"
        + setSize(2.0, 0, "    ")
        + "    with " + appPy() + ".noUndoStep():\n"
        + setSize(3.0, 1, "        ");
    ASSERT_TRUE(run(script));

    ASSERT_EQ(1, _history->count());
    _history->undo();
    EXPECT_EQ(1.0, _size->getValue(0));
    EXPECT_EQ(3.0, _size->getValue(1));

    _history->redo();
    EXPECT_EQ(2.0, _size->getValue(0));
    EXPECT_EQ(3.0, _size->getValue(1));
}

// The suppressed edit overwrites the step's edit of that dimension, so undoing the step leaves it,
// while the step's later edit of the same dimension still undoes back to it.
TEST_F(PythonUndoStep, NoUndoStepEditOfTheSameDimensionIsWhatUndoReturnsTo)
{
    const std::string script = "with " + appPy() + ".newUndoStep(\"Same\"):\n"
        + setSize(2.0, 0, "    ")
        + "    with " + appPy() + ".noUndoStep():\n"
        + setSize(5.0, 0, "        ")
        + setSize(7.0, 0, "    ");
    ASSERT_TRUE(run(script));

    ASSERT_EQ(1, _history->count());
    EXPECT_EQ(7.0, _size->getValue(0));
    _history->undo();
    EXPECT_EQ(5.0, _size->getValue(0));
    EXPECT_EQ(1.0, _size->getValue(1));
    _history->redo();
    EXPECT_EQ(7.0, _size->getValue(0));
}

TEST_F(PythonUndoStep, NoUndoStepRecordsNothing)
{
    const std::string script = "with " + appPy() + ".noUndoStep():\n"
        + setSize(2.0, 0, "    ")
        + "    with " + appPy() + ".newUndoStep(\"Inside\"):\n"
        + setCrop(false, "        ");
    ASSERT_TRUE(run(script));

    EXPECT_FALSE(_history->isStepOpen());
    EXPECT_EQ(0, _history->count());
    EXPECT_EQ(2.0, _size->getValue(0));
    EXPECT_FALSE(_crop->getValue());
}

TEST_F(PythonUndoStep, NoUndoStepInsideAStepLeavesOnlyItsBlockOut)
{
    const std::string script = "with " + appPy() + ".newUndoStep(\"Partly\"):\n"
        + setSize(2.0, 0, "    ")
        + "    with " + appPy() + ".noUndoStep():\n"
        + setCrop(false, "        ");
    ASSERT_TRUE(run(script));

    ASSERT_EQ(1, _history->count());
    _history->undo();
    EXPECT_EQ(1.0, _size->getValue(0));
    EXPECT_FALSE(_crop->getValue());
}

TEST_F(PythonUndoStep, AnExceptionClosesTheStepKeepsTheEditsAndPropagates)
{
    const std::string script = "with " + appPy() + ".newUndoStep(\"Fails\"):\n"
        + setSize(2.0, 0, "    ")
        + setCrop(false, "    ")
        + "    raise RuntimeError(\"undoStepBoom\")\n";
    std::string error;
    EXPECT_FALSE(run(script, &error));
    EXPECT_NE(std::string::npos, error.find("undoStepBoom")) << error;

    EXPECT_FALSE(_history->isStepOpen());
    ASSERT_EQ(1, _history->count());
    EXPECT_EQ(QString::fromUtf8("Fails"), _history->undoText());
    EXPECT_EQ(2.0, _size->getValue(0));
    EXPECT_FALSE(_crop->getValue());

    _history->undo();
    EXPECT_EQ(1.0, _size->getValue(0));
    EXPECT_TRUE(_crop->getValue());
}

TEST_F(PythonUndoStep, AnEmptyBlockLeavesNoEntry)
{
    ASSERT_TRUE(run("with " + appPy() + ".newUndoStep(\"Empty\"):\n    pass\n"));

    EXPECT_FALSE(_history->isStepOpen());
    EXPECT_EQ(0, _history->count());
}

TEST_F(PythonUndoStep, WithoutARecorderBothManagersAreNoOps)
{
    _app->setUndoRecorder(UndoRecorderPtr());
    const std::string script = "with " + appPy() + ".newUndoStep(\"Unrecorded\"):\n"
        + setSize(2.0, 0, "    ")
        + "    with " + appPy() + ".noUndoStep():\n"
        + setCrop(false, "        ");
    ASSERT_TRUE(run(script));

    EXPECT_FALSE(_history->isStepOpen());
    EXPECT_EQ(0, _history->count());
    EXPECT_EQ(2.0, _size->getValue(0));
    EXPECT_FALSE(_crop->getValue());
}

// A step entered on a worker thread must not open one on the main thread's history: the main
// thread's edit made while the worker holds its block open stays unrecorded.
TEST_F(PythonUndoStep, OffTheMainThreadAStepIsANoOp)
{
    const std::string script = "import threading\n"
                               "_undoStepEntered = threading.Event()\n"
                               "_undoStepRelease = threading.Event()\n"
                               "def _undoStepWorker():\n"
                               "    with "
        + appPy() + ".newUndoStep(\"Worker\"):\n"
                    "        _undoStepEntered.set()\n"
                    "        _undoStepRelease.wait(10)\n"
                    "_undoStepThread = threading.Thread(target=_undoStepWorker)\n"
                    "_undoStepThread.start()\n"
                    "assert _undoStepEntered.wait(10)\n"
        + setSize(2.0, 0, "")
        + "_undoStepRelease.set()\n"
          "_undoStepThread.join(10)\n"
          "assert not _undoStepThread.is_alive()\n"
          "del _undoStepEntered, _undoStepRelease, _undoStepWorker, _undoStepThread\n";
    ASSERT_TRUE(run(script));

    EXPECT_FALSE(_history->isStepOpen());
    EXPECT_EQ(0, _history->count());
    EXPECT_EQ(2.0, _size->getValue(0));
}
