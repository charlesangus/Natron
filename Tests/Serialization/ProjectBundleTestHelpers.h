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

#ifndef Tests_Serialization_ProjectBundleTestHelpers_h
#define Tests_Serialization_ProjectBundleTestHelpers_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <exception>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "Engine/EngineFwd.h"
#include "Engine/Project.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ProjectYaml.h"

NATRON_NAMESPACE_ENTER

inline Serialization::NodeSerializationPtr
findSerializedNode(const std::list<Serialization::NodeSerializationPtr>& nodes,
                   const std::string& pluginID)
{
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        if (node->pluginID == pluginID) {
            return node;
        }
    }

    return Serialization::NodeSerializationPtr();
}

// The project.yaml text of a project holding node alone, read back and written again: a node
// read from a file has untyped knobs, so text is what compares it with one toSerialization()
// filled.
inline std::string
nodeYaml(const Serialization::NodeSerialization& node)
{
    Serialization::ProjectSerialization project;
    project.nodes.nodes.push_back(std::make_shared<Serialization::NodeSerialization>(node));
    try {
        Serialization::MemoryBlobStore blobs;
        const std::string text = Serialization::encodeProjectYaml(project, blobs);
        const Serialization::ProjectSerialization decoded = Serialization::decodeProjectYaml(text, blobs);
        Serialization::MemoryBlobStore again;

        return Serialization::encodeProjectYaml(decoded, again);
    } catch (const std::exception& e) {
        ADD_FAILURE() << "Cannot encode node " << node.scriptName << ": " << e.what();

        return std::string();
    }
}

inline bool
writeProjectBundle(const Serialization::ProjectSerialization& project,
                   const QString& filePath,
                   std::string* projectYaml = nullptr)
{
    try {
        Serialization::ProjectBundleWriter writer;
        writer.setProject(project);
        writer.write(filePath.toStdString());
        if (projectYaml) {
            *projectYaml = writer.projectYaml();
        }
    } catch (const std::exception& e) {
        ADD_FAILURE() << "Cannot write " << filePath.toStdString() << ": " << e.what();

        return false;
    }

    return true;
}

// The text of the member of the bundle at path, such as "project.yaml". A bundle that cannot be
// read or lacks the member fails the test and gives an empty string.
inline std::string
readBundleMember(const QString& path,
                 std::string_view member)
{
    try {
        const Serialization::ProjectBundleReader reader(path.toStdString());
        const std::optional<std::span<const std::byte>> bytes = reader.findMember(member);
        if (!bytes) {
            ADD_FAILURE() << path.toStdString() << " has no member " << member;

            return std::string();
        }

        return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    } catch (const std::exception& e) {
        ADD_FAILURE() << "Cannot read " << path.toStdString() << ": " << e.what();

        return std::string();
    }
}

// Writes serialization as the bundle fileName in tmp and loads that file into project.
inline bool
loadProjectBundle(const ProjectPtr& project,
                  const QTemporaryDir& tmp,
                  const QString& fileName,
                  const Serialization::ProjectSerialization& serialization,
                  std::string* projectYaml = nullptr)
{
    const QString directoryPath = tmp.path() + QLatin1Char('/');
    if (!writeProjectBundle(serialization, directoryPath + fileName, projectYaml)) {
        return false;
    }
    if (!project->loadProject(directoryPath, fileName)) {
        ADD_FAILURE() << "Cannot load " << fileName.toStdString();

        return false;
    }

    return true;
}

// Saves project as fileName in tmp the way the application does and reads back the project.yaml
// that save wrote, as text and decoded.
inline bool
saveProjectBundle(const ProjectPtr& project,
                  const QTemporaryDir& tmp,
                  const QString& fileName,
                  std::string* projectYaml,
                  Serialization::ProjectSerialization* serialization = nullptr)
{
    const QString directoryPath = tmp.path() + QLatin1Char('/');
    QString savedFilePath;
    if (!project->saveProject(directoryPath, fileName, &savedFilePath)) {
        ADD_FAILURE() << "Cannot save " << fileName.toStdString();

        return false;
    }
    try {
        const Serialization::ProjectBundleReader reader(savedFilePath.toStdString());
        if (projectYaml) {
            *projectYaml = std::string(reader.projectYaml());
        }
        if (serialization) {
            *serialization = reader.readProject();
        }
    } catch (const std::exception& e) {
        ADD_FAILURE() << "Cannot read back " << savedFilePath.toStdString() << ": " << e.what();

        return false;
    }

    return true;
}

NATRON_NAMESPACE_EXIT

#endif // Tests_Serialization_ProjectBundleTestHelpers_h
