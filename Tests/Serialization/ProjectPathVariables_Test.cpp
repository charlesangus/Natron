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
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"

NATRON_NAMESPACE_USING

class ProjectPathVariablesTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        project()->reset(false, true);
        ASSERT_TRUE(_dir.isValid());
        setProjectDirectory(_dir.path().toStdString());
    }

    virtual void TearDown() OVERRIDE
    {
        project()->reset(false, true);
        BaseTest::TearDown();
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    void setProjectDirectory(const std::string& dir)
    {
        KnobPathPtr builtins = project()->getEnvVarKnob();

        ASSERT_TRUE(bool(builtins));
        std::list<std::vector<std::string>> table;
        table.push_back(std::vector<std::string> { NATRON_PROJECT_ENV_VAR_NAME, dir });
        builtins->setTable(table);
    }

    std::string canonical(const std::string& path) const
    {
        std::string str = path;
        project()->canonicalizePath(str);

        return str;
    }

    std::string variable(const std::string& name) const
    {
        std::map<std::string, std::string> env;
        project()->getEnvironmentVariables(env);
        std::map<std::string, std::string>::const_iterator found = env.find(name);

        return (found == env.end()) ? std::string("<missing>") : found->second;
    }

    std::string under(const std::string& relative) const
    {
        return (_dir.path() + QLatin1Char('/') + QString::fromStdString(relative)).toStdString();
    }

    QTemporaryDir _dir;
};

TEST_F(ProjectPathVariablesTest, AStringVariableIsAProjectPath)
{
    std::string error;
    KnobIPtr shots = project()->addPathVariable("shots", under("shots"), &error);

    ASSERT_TRUE(bool(shots)) << error;
    EXPECT_TRUE(dynamic_cast<KnobString*>(shots.get()) != NULL);
    EXPECT_EQ(project()->getRFVGroup(), shots->getParentKnob());
    EXPECT_EQ(under("shots"), variable("shots"));
    EXPECT_EQ(under("shots/a.exr"), canonical("[shots]/a.exr"));
}

TEST_F(ProjectPathVariablesTest, AVariableMayBeSpelledThroughAnother)
{
    ASSERT_TRUE(QDir(_dir.path()).mkpath(QString::fromUtf8("shots/s01")));
    std::string error;
    ASSERT_TRUE(bool(project()->addPathVariable("shots", "[Project]/shots", &error))) << error;
    ASSERT_TRUE(bool(project()->addPathVariable("shot", "[shots]/s01", &error))) << error;
    ASSERT_TRUE(bool(project()->addPathVariable("same", "[shot]", &error))) << error;

    EXPECT_EQ(std::string("[shots]/s01"), variable("shot"));
    EXPECT_EQ(under("shots/s01/a.exr"), canonical("[shot]/a.exr"));
    EXPECT_EQ(under("shots/s01/a.exr"), canonical("[same]/a.exr"));
    EXPECT_EQ(under("shots/s01"), project()->expandPathPreview("[shot]"));
}

TEST_F(ProjectPathVariablesTest, AVariableOfAnotherTypeIsNotAProjectPath)
{
    KnobDoublePtr gain = project()->createDoubleKnob("gain", "Gain", 1);
    std::string error;

    ASSERT_TRUE(RFV::addAsVariable(project().get(), gain, &error)) << error;
    EXPECT_EQ(std::string("<missing>"), variable("gain"));
    EXPECT_EQ(under("[gain]/a.exr"), canonical("[gain]/a.exr"));
}

TEST_F(ProjectPathVariablesTest, TheBuiltInNamesAreRefused)
{
    std::string reason;

    EXPECT_FALSE(project()->canAddPathVariable(NATRON_PROJECT_ENV_VAR_NAME, &reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_FALSE(project()->canAddPathVariable(NATRON_OCIO_ENV_VAR_NAME, &reason));
    EXPECT_FALSE(bool(project()->addPathVariable(NATRON_PROJECT_ENV_VAR_NAME, "/elsewhere", &reason)));
    EXPECT_EQ(_dir.path().toStdString(), variable(NATRON_PROJECT_ENV_VAR_NAME));

    KnobStringPtr ocio = project()->createStringKnob(NATRON_OCIO_ENV_VAR_NAME, "OCIO");
    ASSERT_TRUE(bool(ocio));
    EXPECT_FALSE(RFV::addAsVariable(project().get(), ocio, &reason));
    EXPECT_FALSE(reason.empty());
    project()->deleteKnob(ocio.get(), true);
}

TEST_F(ProjectPathVariablesTest, ADuplicateOrInvalidNameIsRefused)
{
    std::string error;

    ASSERT_TRUE(bool(project()->addPathVariable("shots", under("shots"), &error))) << error;
    EXPECT_FALSE(project()->canAddPathVariable("shots", &error));
    EXPECT_FALSE(bool(project()->addPathVariable("shots", under("other"), &error)));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(under("shots"), variable("shots"));
    EXPECT_FALSE(project()->canAddPathVariable("", &error));
    EXPECT_FALSE(project()->canAddPathVariable("2shots", &error));
    EXPECT_FALSE(project()->canAddPathVariable("with space", &error));
}

TEST_F(ProjectPathVariablesTest, AVariableNamingTheSameFolderIsFound)
{
    std::string error;

    ASSERT_TRUE(bool(project()->addPathVariable("shots", "[Project]/shots", &error))) << error;
    EXPECT_EQ(std::string("shots"), project()->findPathVariableWithTarget(under("shots")));
    EXPECT_EQ(std::string("shots"), project()->findPathVariableWithTarget(under("shots/")));
    EXPECT_EQ(std::string("shots"), project()->findPathVariableWithTarget("[Project]/x/../shots"));
    EXPECT_EQ(std::string("shots"), project()->findPathVariableWithTarget("shots"));
    EXPECT_EQ(std::string(NATRON_PROJECT_ENV_VAR_NAME), project()->findPathVariableWithTarget(_dir.path().toStdString()));
    EXPECT_EQ(std::string(), project()->findPathVariableWithTarget(under("other")));
    EXPECT_EQ(std::string(), project()->findPathVariableWithTarget(under("shots"), "shots"));
}

TEST_F(ProjectPathVariablesTest, AUserRowOfTheBuiltInTableIsNotAProjectPath)
{
    KnobPathPtr builtins = project()->getEnvVarKnob();
    std::list<std::vector<std::string>> table;

    builtins->getTable(&table);
    table.push_back(std::vector<std::string> { "legacy", under("legacy") });
    builtins->setTable(table);
    EXPECT_EQ(std::string("<missing>"), variable("legacy"));
}

TEST_F(ProjectPathVariablesTest, ANestedVariableResolvesAfterSaveAndLoad)
{
    std::string error;
    ASSERT_TRUE(bool(project()->addPathVariable("shots", "[Project]/shots", &error))) << error;
    ASSERT_TRUE(bool(project()->addPathVariable("shot", "[shots]/s01", &error))) << error;

    const QString dirPath = _dir.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("path-variables.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project()->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project()->reset(false, true);
    EXPECT_EQ(std::string("<missing>"), variable("shot"));

    ASSERT_TRUE(project()->loadProject(QFileInfo(savedFilePath).absolutePath() + QLatin1Char('/'), QFileInfo(savedFilePath).fileName()));
    EXPECT_EQ(std::string("[Project]/shots"), variable("shots"));
    EXPECT_EQ(std::string("[shots]/s01"), variable("shot"));
    KnobIPtr shot = project()->getKnobByName("shot");
    ASSERT_TRUE(bool(shot));
    EXPECT_EQ(project()->getRFVGroup(), shot->getParentKnob());

    const std::string projectDir = variable(NATRON_PROJECT_ENV_VAR_NAME);
    ASSERT_FALSE(projectDir.empty());
    EXPECT_EQ(QDir::cleanPath(QString::fromStdString(projectDir + "/shots/s01")).toStdString(), project()->expandPathPreview("[shot]"));
}
