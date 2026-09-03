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

#include <string>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {
// The colorspaces the fixture stores, as they are spelled by the "blender" profile of
// the OpenColorIO-Configs tarball Natron defaulted to before the ACES configs.
const char* const kFixtureInputSpace = "Linear";
const char* const kFixtureOutputSpace = "sRGB";

// The nearest thing the ACES configs do define. Not equivalents -- only names that
// resolve, so that the control case differs from the failing one in nothing else.
const char* const kResolvableInputSpace = "scene_linear";
const char* const kResolvableOutputSpace = "sRGB - Display";

QString
writeProjectCopy(const QString& dirPath,
                 const QString& fileName,
                 const QString& inputSpace,
                 const QString& outputSpace)
{
    QFile fixture(QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/ocio-old-config.ntp"));

    if (!fixture.open(QIODevice::ReadOnly)) {
        ADD_FAILURE() << "cannot open " << fixture.fileName().toStdString();

        return QString();
    }
    QByteArray content = fixture.readAll();
    fixture.close();

    content.replace(QByteArray("<Value>") + kFixtureInputSpace + "</Value>",
                    QByteArray("<Value>") + inputSpace.toUtf8() + "</Value>");
    content.replace(QByteArray("<Value>") + kFixtureOutputSpace + "</Value>",
                    QByteArray("<Value>") + outputSpace.toUtf8() + "</Value>");

    const QString filePath = dirPath + fileName;
    QFile copy(filePath);
    if (!copy.open(QIODevice::WriteOnly)) {
        ADD_FAILURE() << "cannot write " << filePath.toStdString();

        return QString();
    }
    copy.write(content);
    copy.close();

    return filePath;
}

ProjectPtr
getProject()
{
    return appPTR->getTopLevelInstance()->getProject();
}

// Returns what the nodes reported while the project loaded. Node::setPersistentMessage()
// only stores a message when the process has a GUI; this binary, like NatronRenderer, is
// always AppManager::isBackground(), where the same call prints "Persistent message: ..."
// instead. That print is the whole of the error state observable from here.
bool
loadProjectCopy(const QString& dirPath,
                const QString& fileName,
                std::string* output)
{
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const bool ok = getProject()->loadProject(dirPath, fileName);
    *output = testing::internal::GetCapturedStdout() + testing::internal::GetCapturedStderr();

    return ok;
}
} // namespace

class ProjectOCIOTest
    : public testing::Test {
protected:
    virtual void TearDown()
    {
        // Both cases below leave a loaded project behind, and every test in this binary
        // shares one AppInstance.
        getProject()->reset(false, true);
    }
};

TEST_F(ProjectOCIOTest, ColorSpaceMissingFromTheActiveConfigPutsItsNodeInAnErrorState)
{
    QTemporaryDir tmp;

    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');

    ASSERT_FALSE(writeProjectCopy(dirPath,
                                  QString::fromUtf8("old.ntp"),
                                  QString::fromUtf8(kFixtureInputSpace),
                                  QString::fromUtf8(kFixtureOutputSpace))
                     .isEmpty());

    std::string output;
    const bool loaded = loadProjectCopy(dirPath, QString::fromUtf8("old.ntp"), &output);

    EXPECT_TRUE(loaded) << output;
    EXPECT_TRUE(getProject()->getNodeByName("Read1").get() != NULL);
    EXPECT_TRUE(getProject()->getNodeByName("Write1").get() != NULL);

    EXPECT_NE(output.find("ocioInputSpace = \"Linear\" is not a colorspace in the "
                          "OpenColorIO config \"studio-config-v4.0.0_aces-v2.0_ocio-v2.5\"."),
              std::string::npos)
        << output;
    EXPECT_NE(output.find("ocioOutputSpace = \"sRGB\" is not a colorspace in the "
                          "OpenColorIO config \"studio-config-v4.0.0_aces-v2.0_ocio-v2.5\"."),
              std::string::npos)
        << output;
}

TEST_F(ProjectOCIOTest, ColorSpaceMissingFromTheActiveConfigLeavesTheNodeWithAPersistentMessage)
{
    QTemporaryDir tmp;

    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');

    ASSERT_FALSE(writeProjectCopy(dirPath,
                                  QString::fromUtf8("old.ntp"),
                                  QString::fromUtf8(kFixtureInputSpace),
                                  QString::fromUtf8(kFixtureOutputSpace))
                     .isEmpty());

    std::string output;
    const bool loaded = loadProjectCopy(dirPath, QString::fromUtf8("old.ntp"), &output);

    EXPECT_TRUE(loaded) << output;
    NodePtr read1 = getProject()->getNodeByName("Read1");
    ASSERT_TRUE(read1.get() != NULL);
    EXPECT_TRUE(read1->hasPersistentMessage());
}

TEST_F(ProjectOCIOTest, NodesWhoseColorSpacesResolveAreLeftAlone)
{
    QTemporaryDir tmp;

    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');

    ASSERT_FALSE(writeProjectCopy(dirPath,
                                  QString::fromUtf8("new.ntp"),
                                  QString::fromUtf8(kResolvableInputSpace),
                                  QString::fromUtf8(kResolvableOutputSpace))
                     .isEmpty());

    std::string output;
    const bool loaded = loadProjectCopy(dirPath, QString::fromUtf8("new.ntp"), &output);

    EXPECT_TRUE(loaded) << output;
    EXPECT_EQ(output.find("is not a colorspace in the OpenColorIO config"), std::string::npos) << output;
}
