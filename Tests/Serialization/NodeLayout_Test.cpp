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

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "ProjectBundleTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kNotInstalledPluginID = "com.example.NotInstalled";

std::vector<std::string>
splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    return lines;
}

std::string
trimmed(const std::string& line)
{
    const std::size_t first = line.find_first_not_of(' ');

    return (first == std::string::npos) ? std::string() : line.substr(first);
}

// The script name of the top-level node whose block holds line index, or empty: top-level nodes
// are the keys indented by two spaces under "nodes:".
std::string
topLevelNodeOwning(const std::vector<std::string>& lines,
                   std::size_t index)
{
    for (std::size_t i = index + 1; i-- > 0;) {
        const std::string& line = lines[i];
        if ((line.size() > 3) && (line.compare(0, 2, "  ") == 0) && (line[2] != ' ') && (line.back() == ':')) {
            return line.substr(2, line.size() - 3);
        }
        if (!line.empty() && (line[0] != ' ')) {
            break;
        }
    }

    return std::string();
}

} // anonymous namespace

class NodeLayoutTest
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

    NodePtr createBlur()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    }

    NodePtr createInside(const NodePtr& group,
                         const std::string& pluginID)
    {
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
        if (!collection) {
            return NodePtr();
        }
        CreateNodeArgs args(pluginID, collection);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    std::string saveYaml(const QTemporaryDir& tmp,
                         const char* fileName,
                         Serialization::ProjectSerialization* serialization = nullptr)
    {
        std::string yaml;
        EXPECT_TRUE(saveProjectBundle(getApp()->getProject(), tmp, QString::fromUtf8(fileName), &yaml, serialization));

        return yaml;
    }

    bool reload(const QTemporaryDir& tmp,
                const char* fileName)
    {
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);

        return project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8(fileName));
    }

    static void expectPosition(const NodePtr& node,
                               double x,
                               double y)
    {
        double gotX = 0., gotY = 0.;
        node->getPosition(&gotX, &gotY);
        EXPECT_DOUBLE_EQ(x, gotX) << node->getFullyQualifiedName();
        EXPECT_DOUBLE_EQ(y, gotY) << node->getFullyQualifiedName();
    }
};

TEST_F(NodeLayoutTest, SetPositionIsWhatGetPositionReturns)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));

    blur->setPosition(137.5, -42.25);
    expectPosition(blur, 137.5, -42.25);
}

TEST_F(NodeLayoutTest, SaveAndReloadKeepPositionsAndBackdropSizeAndColour)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr blur = createBlur();
    NodePtr group = createNode(QString::fromUtf8(PLUGINID_NATRON_GROUP));
    NodePtr backdrop = createNode(QString::fromUtf8(PLUGINID_NATRON_BACKDROP));
    ASSERT_TRUE(blur && group && backdrop);
    NodePtr child = createInside(group, PLUGINID_NATRON_BLUR);
    ASSERT_TRUE(bool(child));

    blur->setPosition(210., 95.5);
    group->setPosition(-300., 40.);
    child->setPosition(17.25, 333.);
    backdrop->setPosition(-512., -256.);
    backdrop->setSize(640., 410.);
    backdrop->setColor(0.25, 0.5, 0.75);
    ASSERT_TRUE(backdrop->hasSize());
    ASSERT_TRUE(backdrop->hasUserColor());

    const std::string blurName = blur->getScriptName_mt_safe();
    const std::string groupName = group->getScriptName_mt_safe();
    const std::string childName = groupName + "." + child->getScriptName_mt_safe();
    const std::string backdropName = backdrop->getScriptName_mt_safe();

    saveYaml(tmp, "layout.ntpz");
    ASSERT_TRUE(reload(tmp, "layout.ntpz"));

    ProjectPtr project = getApp()->getProject();
    NodePtr reloadedBlur = project->getNodeByName(blurName);
    NodePtr reloadedGroup = project->getNodeByName(groupName);
    NodePtr reloadedChild = project->getNodeByFullySpecifiedName(childName);
    NodePtr reloadedBackdrop = project->getNodeByName(backdropName);
    ASSERT_TRUE(reloadedBlur && reloadedGroup && reloadedChild && reloadedBackdrop);

    expectPosition(reloadedBlur, 210., 95.5);
    expectPosition(reloadedGroup, -300., 40.);
    expectPosition(reloadedChild, 17.25, 333.);
    expectPosition(reloadedBackdrop, -512., -256.);

    ASSERT_TRUE(reloadedBackdrop->hasSize());
    double w = 0., h = 0.;
    reloadedBackdrop->getSize(&w, &h);
    EXPECT_DOUBLE_EQ(640., w);
    EXPECT_DOUBLE_EQ(410., h);

    ASSERT_TRUE(reloadedBackdrop->hasUserColor());
    double r = 0., g = 0., b = 0.;
    reloadedBackdrop->getUserColor(&r, &g, &b);
    EXPECT_DOUBLE_EQ(0.25, r);
    EXPECT_DOUBLE_EQ(0.5, g);
    EXPECT_DOUBLE_EQ(0.75, b);

    EXPECT_FALSE(reloadedBlur->hasSize());
    EXPECT_FALSE(reloadedBlur->hasUserColor());
}

TEST_F(NodeLayoutTest, RenameKeepsThePosition)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    blur->setPosition(64., 128.);

    blur->setScriptName("RenamedBlur");
    ASSERT_EQ(std::string("RenamedBlur"), blur->getScriptName_mt_safe());
    expectPosition(blur, 64., 128.);

    saveYaml(tmp, "renamed.ntpz");
    ASSERT_TRUE(reload(tmp, "renamed.ntpz"));
    NodePtr reloaded = getApp()->getProject()->getNodeByName("RenamedBlur");
    ASSERT_TRUE(bool(reloaded));
    expectPosition(reloaded, 64., 128.);
}

TEST_F(NodeLayoutTest, AMovedPlaceholderSavesItsNewPositionAndKeepsItsChildren)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr group = createNode(QString::fromUtf8(PLUGINID_NATRON_GROUP));
    ASSERT_TRUE(bool(group));
    NodePtr child = createInside(group, PLUGINID_NATRON_BLUR);
    ASSERT_TRUE(bool(child));
    group->setPosition(10., 20.);
    child->setPosition(-71.5, 902.);
    const std::string groupName = group->getScriptName_mt_safe();
    const std::string childName = child->getScriptName_mt_safe();

    Serialization::ProjectSerialization saved;
    getApp()->getProject()->toSerialization(&saved);
    Serialization::NodeSerializationPtr missing = findSerializedNode(saved.nodes.nodes, PLUGINID_NATRON_GROUP);
    ASSERT_TRUE(bool(missing));
    missing->pluginID = kNotInstalledPluginID;

    ProjectPtr project = getApp()->getProject();
    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("missing-group.ntpz"), saved));

    NodePtr placeholder = project->getNodeByName(groupName);
    ASSERT_TRUE(bool(placeholder));
    ASSERT_EQ(std::string(PLUGINID_NATRON_MISSING_PLUGIN), placeholder->getPluginID());
    expectPosition(placeholder, 10., 20.);

    placeholder->setPosition(480., -160.);

    Serialization::ProjectSerialization resaved;
    saveYaml(tmp, "missing-group-moved.ntpz", &resaved);
    Serialization::NodeSerializationPtr resavedGroup = findSerializedNode(resaved.nodes.nodes, kNotInstalledPluginID);
    ASSERT_TRUE(bool(resavedGroup));
    ASSERT_TRUE(resavedGroup->layout.hasPosition);
    EXPECT_DOUBLE_EQ(480., resavedGroup->layout.x);
    EXPECT_DOUBLE_EQ(-160., resavedGroup->layout.y);

    Serialization::NodeSerializationPtr resavedChild;
    for (const Serialization::NodeSerializationPtr& node : resavedGroup->children) {
        if (node->scriptName == childName) {
            resavedChild = node;
        }
    }
    ASSERT_TRUE(bool(resavedChild));
    ASSERT_TRUE(resavedChild->layout.hasPosition);
    EXPECT_DOUBLE_EQ(-71.5, resavedChild->layout.x);
    EXPECT_DOUBLE_EQ(902., resavedChild->layout.y);
}

TEST_F(NodeLayoutTest, MovingOneNodeChangesOnlyItsPosLine)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr blur = createBlur();
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(blur && grade);
    blur->setPosition(100., 200.);
    grade->setPosition(300., 400.);
    const std::string blurName = blur->getScriptName_mt_safe();

    const std::vector<std::string> before = splitLines(saveYaml(tmp, "before-move.ntpz"));
    ASSERT_FALSE(before.empty());

    blur->setPosition(150.5, -25.);
    const std::vector<std::string> after = splitLines(saveYaml(tmp, "after-move.ntpz"));
    ASSERT_EQ(before.size(), after.size());

    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i] != after[i]) {
            changed.push_back(i);
        }
    }
    ASSERT_EQ(1u, changed.size());
    const std::size_t line = changed.front();
    EXPECT_EQ(0u, trimmed(before[line]).rfind("pos:", 0)) << before[line];
    EXPECT_EQ(0u, trimmed(after[line]).rfind("pos:", 0)) << after[line];
    EXPECT_EQ(blurName, topLevelNodeOwning(after, line)) << after[line];
}

TEST_F(NodeLayoutTest, TogglingPreviewOffItsDefaultSavesOneMoreKnobLine)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    // A knob already off its default, so the node's knobs map exists before preview is changed.
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
    ASSERT_TRUE(bool(size));
    size->setValue(7., ViewSpec::all(), 0);
    const bool previewByDefault = blur->isPreviewEnabled();

    const std::vector<std::string> before = splitLines(saveYaml(tmp, "preview-default.ntpz"));
    blur->togglePreview();
    ASSERT_NE(previewByDefault, blur->isPreviewEnabled());
    std::vector<std::string> after = splitLines(saveYaml(tmp, "preview-toggled.ntpz"));
    ASSERT_EQ(before.size() + 1, after.size());

    std::size_t previewLines = 0;
    std::vector<std::string> withoutPreview;
    for (const std::string& line : after) {
        if (trimmed(line).rfind(kEnablePreviewKnobName ":", 0) == 0) {
            ++previewLines;
        } else {
            withoutPreview.push_back(line);
        }
    }
    EXPECT_EQ(1u, previewLines);
    EXPECT_EQ(before, withoutPreview);
}

TEST_F(NodeLayoutTest, SavingTwiceWritesTheSameProjectYaml)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr blur = createBlur();
    NodePtr backdrop = createNode(QString::fromUtf8(PLUGINID_NATRON_BACKDROP));
    NodePtr group = createNode(QString::fromUtf8(PLUGINID_NATRON_GROUP));
    ASSERT_TRUE(blur && backdrop && group);
    NodePtr child = createInside(group, PLUGINID_NATRON_BLUR);
    ASSERT_TRUE(bool(child));
    blur->setPosition(12.5, 7.);
    blur->setColor(0.1, 0.2, 0.3);
    blur->setOverlayColor(0.9, 0.8, 0.7);
    backdrop->setPosition(-40., -80.);
    backdrop->setSize(300., 150.);
    backdrop->setColor(0.4, 0.3, 0.2);
    group->setPosition(500., 0.);
    child->setPosition(1., 2.);

    const std::string first = saveYaml(tmp, "twice.ntpz");
    const std::string second = saveYaml(tmp, "twice.ntpz");
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}
