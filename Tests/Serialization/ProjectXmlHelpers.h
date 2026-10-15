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

#ifndef Tests_Serialization_ProjectXmlHelpers_h
#define Tests_Serialization_ProjectXmlHelpers_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <exception>
#include <list>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>

#include "Engine/EngineFwd.h"
#include "Engine/Project.h"

#include "Serialization/BoostXml/NodeXml.h"
#include "Serialization/BoostXml/ProjectXml.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER

// The project files written here carry no GUI section, so they are always marked as saved by a
// background app: loading one into a GUI app would otherwise read a GUI section next.
inline std::string
saveProjectXml(const Serialization::ProjectSerialization& serialization)
{
    std::stringstream buffer;
    {
        boost::archive::xml_oarchive archive(buffer);
        bool backgroundProject = true;
        archive << boost::serialization::make_nvp("Background_project", backgroundProject);
        archive << boost::serialization::make_nvp("Project", serialization);
    }

    return buffer.str();
}

// KnobsAge and CacheID are session counters, not saved state: a node's age moves on as soon as
// the loaded graph is connected and its expressions are restored.
inline std::string
withoutSessionCounters(const std::string& xml)
{
    QString normalized = QString::fromUtf8(xml.c_str());
    const char* const sessionCounterTags[] = { "KnobsAge", "CacheID" };

    for (std::size_t i = 0; i < sizeof(sessionCounterTags) / sizeof(sessionCounterTags[0]); ++i) {
        const QString tag = QString::fromUtf8(sessionCounterTags[i]);
        QRegularExpression pattern(QString::fromUtf8("<%1>[^<]*</%1>").arg(tag));
        normalized.replace(pattern, QString::fromUtf8("<%1>SESSION_COUNTER</%1>").arg(tag));
    }

    return normalized.toStdString();
}

inline bool
loadProjectXml(const std::string& xml,
               Serialization::ProjectSerialization* serialization)
{
    try {
        std::stringstream buffer(xml);
        boost::archive::xml_iarchive archive(buffer);
        bool backgroundProject = false;
        archive >> boost::serialization::make_nvp("Background_project", backgroundProject);
        archive >> boost::serialization::make_nvp("Project", *serialization);
    } catch (const std::exception& e) {
        ADD_FAILURE() << "Loading through Serialization/BoostXml failed: " << e.what();

        return false;
    }

    return true;
}

inline Serialization::NodeSerializationPtr
findSerializedNode(const std::list<Serialization::NodeSerializationPtr>& nodes,
                   const std::string& pluginID)
{
    for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ((*it)->pluginID == pluginID) {
            return *it;
        }
    }

    return Serialization::NodeSerializationPtr();
}

// Writes xml to fileName in tmp, loads it as a project file into project and returns the XML
// saving that project writes.
inline std::string
reloadProjectXml(const ProjectPtr& project,
                 const QTemporaryDir& tmp,
                 const QString& fileName,
                 const std::string& xml)
{
    const QString directoryPath = tmp.path() + QLatin1Char('/');
    {
        QFile projectFile(directoryPath + fileName);
        if (!projectFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            ADD_FAILURE() << "Cannot write " << fileName.toStdString();

            return std::string();
        }
        projectFile.write(xml.c_str(), (qint64)xml.size());
    }
    if (!project->loadProject(directoryPath, fileName)) {
        ADD_FAILURE() << "Cannot load " << fileName.toStdString();

        return std::string();
    }

    Serialization::ProjectSerialization reloaded;
    project->toSerialization(&reloaded);
    project->toSerialization(&reloaded);

    return saveProjectXml(reloaded);
}

NATRON_NAMESPACE_EXIT

#endif // Tests_Serialization_ProjectXmlHelpers_h
