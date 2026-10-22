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

#include <iostream>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "ProjectBundleTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/MissingPluginNode.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const char* const kNotInstalledPluginID = "com.example.NotInstalled";
const char* const kBundledPyPlugID = "fr.inria.DropShadow";
const double kUserKnobValue = 3.25;

bool
isPluginLoaded(const char* pluginID)
{
    try {
        return appPTR->getPluginBinary(QString::fromUtf8(pluginID), -1, -1, false) != NULL;
    } catch (const std::exception&) {
        return false;
    }
}

std::size_t
countNodesRecursively(const ProjectPtr& project)
{
    NodesList nodes;

    project->getNodes_recursive(nodes, false);

    return nodes.size();
}

// Restores the "load PyPlugs from .py" preference when a test changes it.
class LoadFromPyFileScope {
public:
    explicit LoadFromPyFileScope(bool value)
        : _knob(std::dynamic_pointer_cast<KnobBool>(appPTR->getCurrentSettings()->getKnobByName("loadFromPyFile")))
    {
        if (_knob) {
            _previous = _knob->getValue();
            _knob->setValue(value);
        }
    }

    ~LoadFromPyFileScope()
    {
        if (_knob) {
            _knob->setValue(_previous);
        }
    }

    bool valid() const { return bool(_knob); }

private:
    KnobBoolPtr _knob;
    bool _previous = true;
};

} // anonymous namespace

class MissingPluginFallbackTest
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

    // generator -> Grade -> Dot, the Grade carrying a user knob.
    void buildGraph()
    {
        _generator = createNode(_generatorPluginID);
        _grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        _downstream = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
        ASSERT_TRUE(_generator && _grade && _downstream);
        connectNodes(_generator, _grade, 0, true);
        connectNodes(_grade, _downstream, 0, true);

        EffectInstancePtr gradeEffect = _grade->getEffectInstance();
        KnobPagePtr userPage = gradeEffect->getOrCreateUserPageKnob();
        ASSERT_TRUE(bool(userPage));
        KnobDoublePtr userKnob = AppManager::createKnob<KnobDouble>(gradeEffect.get(), std::string("myUserValue"), 1, false);
        ASSERT_TRUE(bool(userKnob));
        userKnob->setName("myUserValue");
        userKnob->setAsUserKnob(true);
        userKnob->setValue(kUserKnobValue);
        userPage->addKnob(userKnob);

        _generatorName = _generator->getScriptName_mt_safe();
        _gradeName = _grade->getScriptName_mt_safe();
        _downstreamName = _downstream->getScriptName_mt_safe();
    }

    // Serializes the project into *saved with the Grade's plug-in ID replaced by pluginID and
    // returns that Grade's serialization in *grade.
    bool serializeWithGradeID(const std::string& pluginID,
                              Serialization::ProjectSerialization* saved,
                              Serialization::NodeSerializationPtr* grade)
    {
        getApp()->getProject()->toSerialization(saved);
        *grade = findSerializedNode(saved->nodes.nodes, PLUGINID_NATRON_GRADE);
        if (!*grade) {
            ADD_FAILURE() << "The Grade was not saved";

            return false;
        }
        (*grade)->pluginID = pluginID;

        return true;
    }

    NodePtr _generator;
    NodePtr _grade;
    NodePtr _downstream;
    std::string _generatorName;
    std::string _gradeName;
    std::string _downstreamName;
};

TEST_F(MissingPluginFallbackTest, LowercasePluginIDLoadsTheInstalledPlugin)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_NO_FATAL_FAILURE(buildGraph());

    Serialization::ProjectSerialization saved;
    Serialization::NodeSerializationPtr grade;
    ASSERT_TRUE(serializeWithGradeID(QString::fromUtf8(PLUGINID_NATRON_GRADE).toLower().toStdString(), &saved, &grade));

    ProjectPtr project = getApp()->getProject();
    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("lowercase-id.ntpz"), saved));

    NodePtr loaded = project->getNodeByName(_gradeName);
    NodePtr generator = project->getNodeByName(_generatorName);
    NodePtr downstream = project->getNodeByName(_downstreamName);
    ASSERT_TRUE(loaded && generator && downstream);
    EXPECT_EQ(std::string(PLUGINID_NATRON_GRADE), loaded->getPluginID());
    EXPECT_EQ(generator, loaded->getInput(0));
    EXPECT_EQ(loaded, downstream->getInput(0));

    KnobDouble* userKnob = dynamic_cast<KnobDouble*>(loaded->getKnobByName("myUserValue").get());
    ASSERT_TRUE(userKnob != NULL);
    EXPECT_DOUBLE_EQ(kUserKnobValue, userKnob->getValue(0));
}

TEST_F(MissingPluginFallbackTest, PythonCannotModifyAPlaceholderButCanReadIt)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_NO_FATAL_FAILURE(buildGraph());

    Serialization::ProjectSerialization saved;
    Serialization::NodeSerializationPtr original;
    ASSERT_TRUE(serializeWithGradeID(kNotInstalledPluginID, &saved, &original));

    ProjectPtr project = getApp()->getProject();
    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("missing-plugin.ntpz"), saved));

    NodePtr placeholder = project->getNodeByName(_gradeName);
    ASSERT_TRUE(bool(placeholder));
    ASSERT_EQ(std::string(PLUGINID_NATRON_MISSING_PLUGIN), placeholder->getPluginID());

    const std::string node = getApp()->getAppIDString() + ".getNode(\"" + _gradeName + "\")";
    const std::string param = node + ".getParam(\"myUserValue\")";
    const char* const writes[] = {
        ".set(1.5)\n",
        ".setValue(1.5, 0)\n",
        ".setValueAtTime(1.5, 10, 0)\n",
        ".setExpression(\"2.0\", False, 0)\n",
        ".setEnabled(True, 0)\n",
    };
    for (std::size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); ++i) {
        std::string error, output;
        EXPECT_FALSE(interpretPythonScript(param + writes[i], &error, &output)) << writes[i];
        EXPECT_NE(std::string::npos, error.find("ValueError")) << writes[i] << error;
    }
    {
        std::string error, output;
        EXPECT_FALSE(interpretPythonScript(node + ".createDoubleParam(\"added\", \"Added\")\n", &error, &output));
        EXPECT_NE(std::string::npos, error.find("ValueError")) << error;
        EXPECT_FALSE(bool(placeholder->getKnobByName("added")));
    }
    {
        std::string error, output;
        EXPECT_TRUE(interpretPythonScript("if " + param + ".get() != 3.25:\n    raise ValueError(str(" + param + ".get()))\n", &error, &output)) << error;
    }

    KnobDouble* userKnob = dynamic_cast<KnobDouble*>(placeholder->getKnobByName("myUserValue").get());
    ASSERT_TRUE(userKnob != NULL);
    EXPECT_DOUBLE_EQ(kUserKnobValue, userKnob->getValue(0));
    EXPECT_FALSE(userKnob->isAnimated(0));
    EXPECT_TRUE(userKnob->getExpression(0).empty());
    EXPECT_FALSE(userKnob->isEnabled(0));

    Serialization::ProjectSerialization resaved;
    project->toSerialization(&resaved);
    Serialization::NodeSerializationPtr resavedMissing = findSerializedNode(resaved.nodes.nodes, kNotInstalledPluginID);
    ASSERT_TRUE(bool(resavedMissing));
    EXPECT_EQ(nodeYaml(*original), nodeYaml(*resavedMissing));
}

TEST_F(MissingPluginFallbackTest, PythonStillModifiesAnInstalledNode)
{
    ASSERT_NO_FATAL_FAILURE(buildGraph());

    const std::string param = getApp()->getAppIDString() + ".getNode(\"" + _gradeName + "\").getParam(\"myUserValue\")";
    std::string error, output;
    EXPECT_TRUE(interpretPythonScript(param + ".set(1.5)\n", &error, &output)) << error;

    KnobDouble* userKnob = dynamic_cast<KnobDouble*>(_grade->getKnobByName("myUserValue").get());
    ASSERT_TRUE(userKnob != NULL);
    EXPECT_DOUBLE_EQ(1.5, userKnob->getValue(0));
}

// Copy and paste or duplicate hand the copied node's serialization to this same function.
TEST_F(MissingPluginFallbackTest, CreatingFromAMissingPluginSerializationMakesAPlaceholderWithoutItsChildren)
{
    Serialization::NodeSerializationPtr child = std::make_shared<Serialization::NodeSerialization>();
    child->scriptName = "Inner1";
    child->label = "Inner1";
    child->pluginID = PLUGINID_NATRON_DOT;

    Serialization::NodeSerializationPtr serialization = std::make_shared<Serialization::NodeSerialization>();
    serialization->scriptName = "Vanished1";
    serialization->label = "Vanished1";
    serialization->pluginID = kNotInstalledPluginID;
    serialization->pluginMajorVersion = 2;
    serialization->pluginMinorVersion = 1;
    serialization->children.push_back(child);

    ProjectPtr project = getApp()->getProject();
    const std::size_t nodesBefore = countNodesRecursively(project);

    CreateNodeArgs args(kNotInstalledPluginID, project);
    args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, serialization);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, 2, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, 1, 1);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);

    bool isPlaceholder = false;
    NodePtr node = getApp()->createNodeOrMissingPluginPlaceholder(args, &isPlaceholder);
    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(isPlaceholder);
    EXPECT_EQ(std::string(PLUGINID_NATRON_MISSING_PLUGIN), node->getPluginID());
    EXPECT_EQ(nodesBefore + 1, countNodesRecursively(project));

    std::shared_ptr<MissingPluginNode> placeholder = std::dynamic_pointer_cast<MissingPluginNode>(node->getEffectInstance());
    ASSERT_TRUE(bool(placeholder));
    EXPECT_EQ(serialization, placeholder->getStoredSerialization());

    Serialization::NodeSerialization copied;
    node->toSerialization(&copied);
    EXPECT_EQ(std::string(kNotInstalledPluginID), copied.pluginID);
    ASSERT_EQ(1u, copied.children.size());
    EXPECT_EQ(child->scriptName, copied.children.front()->scriptName);
}

TEST_F(MissingPluginFallbackTest, MissingPyPlugIsAPlaceholderAndComesBackWhenInstalled)
{
    if (!isPluginLoaded(kBundledPyPlugID)) {
        std::cerr << "Skipping: the bundled PyPlugs were not registered as plugins." << std::endl;

        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr pyPlug = createNode(QString::fromUtf8(kBundledPyPlugID));
    ASSERT_TRUE(bool(pyPlug));
    const std::string pyPlugName = pyPlug->getScriptName_mt_safe();

    ProjectPtr project = getApp()->getProject();
    Serialization::ProjectSerialization saved;
    project->toSerialization(&saved);
    Serialization::NodeSerializationPtr original = findSerializedNode(saved.nodes.nodes, kBundledPyPlugID);
    ASSERT_TRUE(bool(original));
    ASSERT_FALSE(original->pythonModule.empty());
    ASSERT_FALSE(original->children.empty());
    const std::string installedModule = original->pythonModule;

    // A PyPlug neither registered nor found on disk under its module's file name.
    const std::string notInstalledPyPlugID = "com.example.NotInstalledPyPlug";
    original->pluginID = notInstalledPyPlugID;
    original->pythonModule = "/nonexistent/NotInstalledPyPlug.py";

    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("missing-pyplug.ntpz"), saved));

    NodePtr placeholder = project->getNodeByName(pyPlugName);
    ASSERT_TRUE(bool(placeholder));
    EXPECT_EQ(std::string(PLUGINID_NATRON_MISSING_PLUGIN), placeholder->getPluginID());
    EXPECT_EQ(saved.nodes.nodes.size(), countNodesRecursively(project));

    Serialization::ProjectSerialization resaved;
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("missing-pyplug-resaved.ntpz"), nullptr, &resaved));
    Serialization::NodeSerializationPtr resavedPyPlug = findSerializedNode(resaved.nodes.nodes, notInstalledPyPlugID);
    ASSERT_TRUE(bool(resavedPyPlug));
    EXPECT_EQ(nodeYaml(*original), nodeYaml(*resavedPyPlug));

    resavedPyPlug->pluginID = kBundledPyPlugID;
    resavedPyPlug->pythonModule = installedModule;
    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("pyplug-back.ntpz"), resaved));

    NodePtr restored = project->getNodeByName(pyPlugName);
    ASSERT_TRUE(bool(restored));
    EXPECT_EQ(std::string(kBundledPyPlugID), restored->getPluginID());
    EXPECT_FALSE(restored->getPluginPythonModule().empty());
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(restored->getEffectInstance());
    ASSERT_TRUE(bool(group));
    NodesList innerNodes;
    group->getNodes_recursive(innerNodes, false);
    EXPECT_FALSE(innerNodes.empty());
}

TEST_F(MissingPluginFallbackTest, LoadFromPyFilePreferenceChoosesModuleOrEmbeddedGroup)
{
    if (!isPluginLoaded(kBundledPyPlugID)) {
        std::cerr << "Skipping: the bundled PyPlugs were not registered as plugins." << std::endl;

        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr pyPlug = createNode(QString::fromUtf8(kBundledPyPlugID));
    ASSERT_TRUE(bool(pyPlug));
    const std::string pyPlugName = pyPlug->getScriptName_mt_safe();
    NodeGroupPtr pyPlugGroup = std::dynamic_pointer_cast<NodeGroup>(pyPlug->getEffectInstance());
    ASSERT_TRUE(bool(pyPlugGroup));
    NodesList moduleNodes;
    pyPlugGroup->getNodes_recursive(moduleNodes, false);
    const std::size_t moduleNodeCount = moduleNodes.size();
    ASSERT_GT(moduleNodeCount, std::size_t(1));

    ProjectPtr project = getApp()->getProject();
    Serialization::ProjectSerialization saved;
    project->toSerialization(&saved);
    Serialization::NodeSerializationPtr serializedPyPlug = findSerializedNode(saved.nodes.nodes, kBundledPyPlugID);
    ASSERT_TRUE(bool(serializedPyPlug));

    // Both load paths yield a Group node that reports the PyPlug's identity, so the embedded
    // serialization is told apart from the module by dropping an inner node from it: the module
    // script recreates that node, the embedded serialization does not.
    std::list<Serialization::NodeSerializationPtr>& children = serializedPyPlug->children;
    std::list<Serialization::NodeSerializationPtr>::iterator dropped = children.end();
    for (std::list<Serialization::NodeSerializationPtr>::iterator it = children.begin(); it != children.end(); ++it) {
        if (((*it)->pluginID == PLUGINID_NATRON_INPUT) || ((*it)->pluginID == PLUGINID_NATRON_OUTPUT)) {
            continue;
        }
        bool referenced = false;
        for (std::list<Serialization::NodeSerializationPtr>::const_iterator other = children.begin(); other != children.end(); ++other) {
            for (std::map<std::string, std::string>::const_iterator input = (*other)->inputs.begin(); input != (*other)->inputs.end(); ++input) {
                referenced = referenced || (input->second == (*it)->scriptName);
            }
        }
        if (!referenced) {
            dropped = it;
            break;
        }
        if (dropped == children.end()) {
            dropped = it;
        }
    }
    ASSERT_TRUE(dropped != children.end());
    children.erase(dropped);

    {
        LoadFromPyFileScope enabled(true);
        ASSERT_TRUE(enabled.valid());
        project->reset(false, true);
        ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("pyfile-on.ntpz"), saved));
        NodePtr node = project->getNodeByName(pyPlugName);
        ASSERT_TRUE(bool(node));
        EXPECT_EQ(std::string(kBundledPyPlugID), node->getPluginID());
        NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(node->getEffectInstance());
        ASSERT_TRUE(bool(group));
        NodesList innerNodes;
        group->getNodes_recursive(innerNodes, false);
        EXPECT_EQ(moduleNodeCount, innerNodes.size());
    }
    {
        LoadFromPyFileScope disabled(false);
        ASSERT_TRUE(disabled.valid());
        project->reset(false, true);
        ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("pyfile-off.ntpz"), saved));
        NodePtr node = project->getNodeByName(pyPlugName);
        ASSERT_TRUE(bool(node));
        NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(node->getEffectInstance());
        ASSERT_TRUE(bool(group));
        NodesList innerNodes;
        group->getNodes_recursive(innerNodes, false);
        EXPECT_EQ(moduleNodeCount - 1, innerNodes.size());
    }
}
