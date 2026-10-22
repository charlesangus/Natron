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

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "ProjectBundleTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kNotInstalledPluginID = "com.example.NotInstalled";
const double kWhitePointRed = 0.75;
const double kUserKnobValue = 3.25;

} // anonymous namespace

class MissingPluginRoundTripTest
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

    // generator -> Grade -> Dot, with a second, unconnected generator. The Grade has a non-default
    // white point and a user page holding one user knob.
    void buildGraph()
    {
        _generator = createNode(_generatorPluginID);
        _otherGenerator = createNode(_generatorPluginID);
        _grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        _downstream = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
        ASSERT_TRUE(_generator && _otherGenerator && _grade && _downstream);
        connectNodes(_generator, _grade, 0, true);
        connectNodes(_grade, _downstream, 0, true);

        KnobColor* whitePoint = dynamic_cast<KnobColor*>(_grade->getKnobByName(kGradeParamWhitePoint).get());
        ASSERT_TRUE(whitePoint != NULL);
        whitePoint->setValue(kWhitePointRed, ViewSpec::all(), 0);

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
        _otherGeneratorName = _otherGenerator->getScriptName_mt_safe();
        _gradeName = _grade->getScriptName_mt_safe();
        _downstreamName = _downstream->getScriptName_mt_safe();
    }

    // Serializes the current project into *saved with the Grade's plug-in ID replaced by one
    // that is not installed, and returns that Grade's serialization in *missing.
    bool serializeWithMissingGrade(Serialization::ProjectSerialization* saved,
                                   Serialization::NodeSerializationPtr* missing)
    {
        getApp()->getProject()->toSerialization(saved);
        *missing = findSerializedNode(saved->nodes.nodes, PLUGINID_NATRON_GRADE);
        if (!*missing) {
            ADD_FAILURE() << "The Grade was not saved";

            return false;
        }
        (*missing)->pluginID = kNotInstalledPluginID;

        return true;
    }

    NodePtr _generator;
    NodePtr _otherGenerator;
    NodePtr _grade;
    NodePtr _downstream;
    std::string _generatorName;
    std::string _otherGeneratorName;
    std::string _gradeName;
    std::string _downstreamName;
};

TEST_F(MissingPluginRoundTripTest, SaveWritesTheMissingNodeBackUnchanged)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_NO_FATAL_FAILURE(buildGraph());

    Serialization::ProjectSerialization saved;
    Serialization::NodeSerializationPtr original;
    ASSERT_TRUE(serializeWithMissingGrade(&saved, &original));
    ASSERT_FALSE(original->knobs.empty());
    ASSERT_EQ(1u, original->userPages.size());

    ProjectPtr project = getApp()->getProject();
    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("missing-plugin.ntpz"), saved));

    NodePtr placeholder = project->getNodeByName(_gradeName);
    ASSERT_TRUE(bool(placeholder));
    ASSERT_EQ(std::string(PLUGINID_NATRON_MISSING_PLUGIN), placeholder->getPluginID());

    const std::vector<KnobIPtr>& placeholderKnobs = placeholder->getEffectInstance()->getKnobs();
    ASSERT_FALSE(placeholderKnobs.empty());
    bool foundUserKnob = false;
    for (const KnobIPtr& knob : placeholderKnobs) {
        foundUserKnob = foundUserKnob || knob->getName() == "myUserValue";
        for (int dim = 0; dim < knob->getDimension(); ++dim) {
            EXPECT_FALSE(knob->isEnabled(dim)) << knob->getName() << " dimension " << dim;
        }
    }
    EXPECT_TRUE(foundUserKnob);

    Serialization::ProjectSerialization resaved;
    ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("missing-plugin-resaved.ntpz"), nullptr, &resaved));
    Serialization::NodeSerializationPtr resavedMissing = findSerializedNode(resaved.nodes.nodes, kNotInstalledPluginID);
    ASSERT_TRUE(bool(resavedMissing));
    EXPECT_TRUE(findSerializedNode(resaved.nodes.nodes, PLUGINID_NATRON_MISSING_PLUGIN) == Serialization::NodeSerializationPtr());
    EXPECT_EQ(nodeYaml(*original), nodeYaml(*resavedMissing));

    // With the plug-in back, the file loads as if it had never been missing.
    resavedMissing->pluginID = PLUGINID_NATRON_GRADE;
    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("plugin-back.ntpz"), resaved));

    NodePtr grade = project->getNodeByName(_gradeName);
    NodePtr generator = project->getNodeByName(_generatorName);
    NodePtr downstream = project->getNodeByName(_downstreamName);
    ASSERT_TRUE(grade && generator && downstream);
    EXPECT_EQ(std::string(PLUGINID_NATRON_GRADE), grade->getPluginID());
    EXPECT_EQ(generator, grade->getInput(0));
    EXPECT_EQ(grade, downstream->getInput(0));

    KnobColor* whitePoint = dynamic_cast<KnobColor*>(grade->getKnobByName(kGradeParamWhitePoint).get());
    ASSERT_TRUE(whitePoint != NULL);
    EXPECT_DOUBLE_EQ(kWhitePointRed, whitePoint->getValue(0));

    KnobDouble* userKnob = dynamic_cast<KnobDouble*>(grade->getKnobByName("myUserValue").get());
    ASSERT_TRUE(userKnob != NULL);
    EXPECT_TRUE(userKnob->isUserKnob());
    EXPECT_DOUBLE_EQ(kUserKnobValue, userKnob->getValue(0));
}

TEST_F(MissingPluginRoundTripTest, SaveFollowsTheLiveConnectionsAndName)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_NO_FATAL_FAILURE(buildGraph());

    Serialization::ProjectSerialization saved;
    Serialization::NodeSerializationPtr original;
    ASSERT_TRUE(serializeWithMissingGrade(&saved, &original));

    ProjectPtr project = getApp()->getProject();
    project->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("missing-plugin.ntpz"), saved));

    NodePtr placeholder = project->getNodeByName(_gradeName);
    NodePtr otherGenerator = project->getNodeByName(_otherGeneratorName);
    ASSERT_TRUE(placeholder && otherGenerator);
    ASSERT_EQ(std::string(PLUGINID_NATRON_MISSING_PLUGIN), placeholder->getPluginID());
    ASSERT_TRUE(placeholder->replaceInput(otherGenerator, 0));
    ASSERT_EQ(otherGenerator, placeholder->getInput(0));
    const std::string inputLabel = placeholder->getInputLabel(0);

    const std::string newName = "RenamedPlaceholder";
    placeholder->setScriptName(newName);
    ASSERT_EQ(newName, placeholder->getScriptName_mt_safe());

    Serialization::ProjectSerialization resaved;
    project->toSerialization(&resaved);
    Serialization::NodeSerializationPtr resavedMissing = findSerializedNode(resaved.nodes.nodes, kNotInstalledPluginID);
    ASSERT_TRUE(bool(resavedMissing));

    EXPECT_EQ(newName, resavedMissing->scriptName);
    ASSERT_EQ(1u, resavedMissing->inputs.size());
    EXPECT_EQ(inputLabel, resavedMissing->inputs.begin()->first);
    EXPECT_EQ(_otherGeneratorName, resavedMissing->inputs.begin()->second);

    // Everything the placeholder cannot change is written back as it was read.
    Serialization::NodeSerialization expected = *original;
    expected.scriptName = resavedMissing->scriptName;
    expected.label = resavedMissing->label;
    expected.inputs = resavedMissing->inputs;
    EXPECT_EQ(nodeYaml(expected), nodeYaml(*resavedMissing));

    bool downstreamFollowsRename = false;
    for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = resaved.nodes.nodes.begin(); it != resaved.nodes.nodes.end(); ++it) {
        if ((*it)->scriptName == _downstreamName) {
            downstreamFollowsRename = ((*it)->inputs.size() == 1) && ((*it)->inputs.begin()->second == newName);
        }
    }
    EXPECT_TRUE(downstreamFollowsRename);
}
