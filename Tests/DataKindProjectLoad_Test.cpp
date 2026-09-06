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
#include <string>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "DataKindTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/LogEntry.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

// Kinds are never serialized: a project can only end up on disk with a kind-invalid edge if it was
// wired up bypassing canConnectInput() (e.g. the way NodeWrapper::connectInput() does for Python
// scripts and PyPlug init). connectInput() itself hand-wires such an edge here, without going through
// canConnectInput(), to reproduce that. Loading the resulting project back must drop the edge through
// the normal disconnect path and log a user-visible warning, the same policy used for a plug-in
// missing at load time.
TEST_F(BaseTest, ProjectLoadDropsKindInvalidEdgeWithWarning)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(deepSource && imageSink);
    ASSERT_TRUE(imageSink->connectInput(deepSource, 0));
    ASSERT_EQ(deepSource, imageSink->getInput(0));

    const std::string sinkName = imageSink->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-invalid.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);
    appPTR->clearErrorLog_mt_safe();

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr sink2 = project->getNodeByName(sinkName);
    ASSERT_TRUE(bool(sink2));
    EXPECT_FALSE(bool(sink2->getInput(0)));

    std::list<LogEntry> log;
    appPTR->getErrorLog_mt_safe(&log);
    bool foundWarning = false;
    for (std::list<LogEntry>::const_iterator it = log.begin(); it != log.end(); ++it) {
        if (it->message.contains(QString::fromUtf8("incompatible data kind"), Qt::CaseInsensitive)) {
            foundWarning = true;
            break;
        }
    }
    EXPECT_TRUE(foundWarning);
}

// Every existing project is entirely image-kind: loading one must be a no-op for this check,
// edge and all, with nothing added to the error log.
TEST_F(BaseTest, ProjectLoadKeepsValidEdgeAndLogsNoDataKindWarning)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr generator = createNode(_generatorPluginID);
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(generator && imageSink);
    connectNodes(generator, imageSink, 0, true);

    const std::string sinkName = imageSink->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-valid.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);
    appPTR->clearErrorLog_mt_safe();

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr sink2 = project->getNodeByName(sinkName);
    ASSERT_TRUE(bool(sink2));
    EXPECT_TRUE(bool(sink2->getInput(0)));

    std::list<LogEntry> log;
    appPTR->getErrorLog_mt_safe(&log);
    for (std::list<LogEntry>::const_iterator it = log.begin(); it != log.end(); ++it) {
        EXPECT_FALSE(it->message.contains(QString::fromUtf8("incompatible data kind"), Qt::CaseInsensitive));
    }
}
