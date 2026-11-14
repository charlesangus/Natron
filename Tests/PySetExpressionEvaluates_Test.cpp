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

#include <string>

#include <gtest/gtest.h>

#include <QObject>
#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Project.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

/*
 * A Python setExpression evaluates the change like the GUI's expression edit: the node's
 * onParamChanged callback runs as a user edit and its content hash moves. Value-changed callbacks
 * are not held back by beginChanges(), as for setValue; only the render request is, and it
 * comes once, from the outermost endChanges().
 */
class PySetExpressionEvaluatesTest
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

    std::string py(const NodePtr& node) const
    {
        return getApp()->getAppIDString() + "." + node->getFullyQualifiedName();
    }

    void run(const std::string& script)
    {
        std::string error, output;

        EXPECT_TRUE(interpretPythonScript(script, &error, &output)) << script << "\n"
                                                                    << error;
    }

    NodePtr createConstantWithCallback()
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        if (!constant) {
            return constant;
        }
        run("m424_calls = []\n"
            "def m424_param_changed(thisParam, thisNode, thisGroup, app, userEdited):\n"
            "    m424_calls.append((thisParam.getScriptName(), userEdited))\n"
            + py(constant) + ".getParam('onParamChanged').setValue('m424_param_changed')\n"
                             "del m424_calls[:]\n");

        return constant;
    }
};

TEST_F(PySetExpressionEvaluatesTest, SetAndClearFromPythonEvaluateOnceAsUserEdits)
{
    NodePtr constant = createConstantWithCallback();
    ASSERT_TRUE(constant != nullptr);

    constant->refreshHashNow();
    const U64 original = constant->getContentHash();

    run("c = " + py(constant) + ".getParam('" kConstantParamColor "')\n"
                                "assert c.setExpression('0.25', False, 0)\n"
                                "assert m424_calls == [('" kConstantParamColor "', False)], m424_calls\n");
    constant->refreshHashNow();
    const U64 withExpression = constant->getContentHash();
    EXPECT_NE(original, withExpression);

    run("del m424_calls[:]\n"
        "assert c.setExpression('', False, 0)\n"
        "assert m424_calls == [('" kConstantParamColor "', False)], m424_calls\n");
    constant->refreshHashNow();
    EXPECT_EQ(original, constant->getContentHash());
}

TEST_F(PySetExpressionEvaluatesTest, SettingTheSameExpressionAgainLeavesTheHash)
{
    NodePtr constant = createConstantWithCallback();
    ASSERT_TRUE(constant != nullptr);

    run("c = " + py(constant) + ".getParam('" kConstantParamColor "')\n"
                                "assert c.setExpression('frame * 0.0 + 0.5', False, 1)\n");
    constant->refreshHashNow();
    const U64 first = constant->getContentHash();

    run("assert c.setExpression('frame * 0.0 + 0.5', False, 1)\n");
    constant->refreshHashNow();
    EXPECT_EQ(first, constant->getContentHash());
}

TEST_F(PySetExpressionEvaluatesTest, ABeginEndChangesBatchRequestsOneRenderAtTheEnd)
{
    NodePtr constant = createConstantWithCallback();
    NodePtr viewerNode = createNode(QString::fromUtf8(PLUGINID_NATRON_VIEWER));
    ASSERT_TRUE(constant != nullptr);
    ASSERT_TRUE(viewerNode != nullptr);
    connectNodes(constant, viewerNode, 0, true);

    ViewerInstance* viewer = dynamic_cast<ViewerInstance*>(viewerNode->getEffectInstance().get());
    ASSERT_TRUE(viewer != nullptr);
    RenderEnginePtr engine = viewer->getRenderEngine();
    ASSERT_TRUE(engine != nullptr);

    int renderRequests = 0;
    QObject receiver;
    QObject::connect(engine.get(), &RenderEngine::currentFrameRenderRequestPosted, &receiver, [&renderRequests]() { ++renderRequests; }, Qt::DirectConnection);

    run("n = " + py(constant) + "\n"
                                "c = n.getParam('" kConstantParamColor "')\n"
                                "n.beginChanges()\n"
                                "c.setValue(0.1, 0)\n"
                                "c.setValue(0.2, 1)\n"
                                "c.setValue(0.3, 2)\n"
                                "m424_set_value_calls = len(m424_calls)\n"
                                "n.endChanges()\n"
                                "del m424_calls[:]\n");
    const int setValueRenderRequests = renderRequests;
    EXPECT_EQ(1, setValueRenderRequests);

    renderRequests = 0;
    run("n.beginChanges()\n"
        "assert c.setExpression('0.1', False, 0)\n"
        "assert c.setExpression('0.2', False, 1)\n"
        "assert c.setExpression('0.3', False, 2)\n"
        "m424_expression_calls = len(m424_calls)\n");
    EXPECT_EQ(0, renderRequests);
    run("n.endChanges()\n");
    EXPECT_EQ(1, renderRequests);
    run("assert m424_expression_calls == m424_set_value_calls, (m424_expression_calls, m424_set_value_calls)\n");
}

TEST_F(PySetExpressionEvaluatesTest, AnInvalidExpressionThatReplacesOneStillEvaluates)
{
    NodePtr constant = createConstantWithCallback();
    ASSERT_TRUE(constant != nullptr);

    run("c = " + py(constant) + ".getParam('" kConstantParamColor "')\n"
                                "assert c.setExpression('0.75', False, 0)\n"
                                "del m424_calls[:]\n"
                                "assert not c.setExpression('this is not python', False, 0)\n"
                                "assert m424_calls == [('" kConstantParamColor "', False)], m424_calls\n"
                                "del m424_calls[:]\n"
                                "assert not c.setExpression('this is not python', False, 0)\n"
                                "assert m424_calls == [], m424_calls\n");
}

TEST_F(PySetExpressionEvaluatesTest, RestoringAnExpressionStaysSilent)
{
    NodePtr constant = createConstantWithCallback();
    ASSERT_TRUE(constant != nullptr);
    KnobIPtr color = constant->getKnobByName(kConstantParamColor);
    ASSERT_TRUE(color != nullptr);

    color->restoreExpression(0, "0.5", false);
    EXPECT_EQ(std::string("0.5"), color->getExpression(0));
    run("assert m424_calls == [], m424_calls\n");
}
