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

#include "Project.h"

#include <exception>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>

#include "Global/GitVersion.h"

#include "Engine/Node.h"
#include "Engine/ProjectMerge.h"
#include "Engine/ProjectTemplate.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Merge/ThreeWayMerge.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/GraphOp.h"
#include "Serialization/Ops/OpsPrivate.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Sha256.h"
#include "Serialization/TemplateNodeSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {

// The in-scope nodes whose group is not in scope, in graph order: the nodes a publish writes at
// the top level of the file.
void
collectScopeRoots(const std::list<Serialization::NodeSerializationPtr>& nodes,
                  const std::set<std::string>& scope,
                  std::list<Serialization::NodeSerializationPtr>* roots)
{
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        if (scope.count(node->id)) {
            roots->push_back(Serialization::OpsDetail::deepCopyNode(*node));
        } else {
            collectScopeRoots(node->children, scope, roots);
        }
    }
}

// Members added by hand stand for no base node, so the merge scope does not hold them.
void
addTaggedMembers(const std::list<Serialization::NodeSerializationPtr>& nodes,
                 const std::string& key,
                 std::set<std::string>* scope)
{
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        if (node->origin.templateKey == key) {
            scope->insert(node->id);
        }
        addTaggedMembers(node->children, key, scope);
    }
}

// Gives node and the nodes inside it the IDs of the template nodes they stand for, and lists the
// live nodes that stand for none: their own ID goes in the file, and becomes their origin.
void
toTemplateIds(Serialization::NodeSerialization* node,
              const std::string& key,
              std::set<std::string>* used,
              std::vector<std::string>* newMembers)
{
    const std::string liveId = node->id;
    const bool standsForOne = (node->origin.templateKey == key) && !node->origin.nodeId.empty();
    if (standsForOne && used->insert(node->origin.nodeId).second) {
        node->id = node->origin.nodeId;
    } else {
        used->insert(liveId);
        if (node->origin.templateKey.empty() || (node->origin.templateKey == key)) {
            newMembers->push_back(liveId);
        }
    }
    node->origin = Serialization::NodeOriginSerialization();
    node->cacheID.clear();
    for (const Serialization::NodeSerializationPtr& child : node->children) {
        toTemplateIds(child.get(), key, used, newMembers);
    }
}

} // namespace

bool
Project::publishTemplate(const std::string& key,
                         QString* error,
                         TemplateEdit* edit)
{
    assert(QThread::currentThread() == qApp->thread());
    TemplateEdit local;
    TemplateEdit& out = edit ? *edit : local;
    out.key = key;
    const auto fail = [&](const QString& message) {
        out.error = message;
        if (error) {
            *error = message;
        }

        return false;
    };

    std::optional<Serialization::TemplateImportSerialization> found;
    for (const Serialization::TemplateImportSerialization& entry : getTemplateImports()) {
        if (entry.key == key) {
            found = entry;
        }
    }
    if (!found) {
        return fail(tr("The project has no template import named %1").arg(QString::fromStdString(key)));
    }
    Serialization::TemplateImportSerialization entry = *found;
    if (!entry.base) {
        return fail(tr("The template %1 has no stored base to publish against").arg(QString::fromStdString(entry.name)));
    }
    const QString dirPath = QString::fromStdString(entry.publishDir);
    const QFileInfo dirInfo(dirPath);
    if (dirPath.isEmpty() || !dirInfo.isDir()) {
        return fail(tr("The publish folder \"%1\" of the template %2 does not exist").arg(dirPath).arg(QString::fromStdString(entry.name)));
    }
    if (!dirInfo.isWritable()) {
        return fail(tr("The publish folder \"%1\" is not writable").arg(dirPath));
    }

    const QString stem = QString::fromStdString(ProjectTemplateDetail::keyStem(QString::fromStdString(entry.name)));
    const int next = entry.version + 1;
    const QString versionName = QString::fromUtf8("%1_v%2").arg(stem).arg(next, 3, 10, QLatin1Char('0'));
    const QString fileName = versionName + QString::fromUtf8(".ntpz");
    const QString filePath = QDir(dirPath).absoluteFilePath(fileName);

    // Versions are numbered by template ID, whatever their files are named, so a renamed template
    // keeps counting from its last version. A file name is never shared with another template.
    const QRegularExpression pattern(QString::fromUtf8("^%1_v\\d+\\.ntpz$").arg(QRegularExpression::escape(stem)), QRegularExpression::CaseInsensitiveOption);
    std::optional<ProjectMerge::PublishedFile> newest;
    std::optional<ProjectMerge::PublishedFile> clash;
    for (const ProjectMerge::PublishedFile& file : ProjectMerge::listPublishedFiles(dirPath)) {
        if (file.templateId == entry.templateId) {
            if (!newest || (file.version > newest->version)) {
                newest = file;
            }
        } else if (!clash && pattern.match(QFileInfo(file.path).fileName()).hasMatch()) {
            clash = file;
        }
    }
    if (newest && (newest->version >= next)) {
        return fail(tr("%1 is already published in \"%2\": update to it before publishing").arg(QFileInfo(newest->path).fileName()).arg(dirPath));
    }
    if (clash) {
        return fail(tr("%1 in \"%2\" belongs to another template that is also named %3: rename this template or publish it to another folder").arg(QFileInfo(clash->path).fileName()).arg(dirPath).arg(QString::fromStdString(entry.name)));
    }
    if (QFileInfo::exists(filePath)) {
        return fail(tr("%1 in \"%2\" is in the way and is not a version of the template %3: rename this template or publish it to another folder").arg(fileName).arg(dirPath).arg(QString::fromStdString(entry.name)));
    }

    Serialization::ProjectSerialization mine;
    toSerialization(&mine);
    std::set<std::string> scope = Serialization::planMerge(*entry.base, mine, *entry.base, key).scope;
    addTaggedMembers(mine.nodes.nodes, key, &scope);
    std::list<Serialization::NodeSerializationPtr> members;
    collectScopeRoots(mine.nodes.nodes, scope, &members);
    std::set<std::string> used;
    std::vector<std::string> newMembers;
    std::set<std::string> memberNames;
    for (const Serialization::NodeSerializationPtr& member : members) {
        toTemplateIds(member.get(), key, &used, &newMembers);
        memberNames.insert(member->scriptName);
    }
    // A member fed by a node of the shot is unconnected in the template.
    for (const Serialization::NodeSerializationPtr& member : members) {
        for (std::map<std::string, std::string>::iterator it = member->inputs.begin(); it != member->inputs.end();) {
            if (memberNames.count(it->second)) {
                ++it;
            } else {
                it = member->inputs.erase(it);
            }
        }
    }

    Serialization::TemplateNodeState state;
    state.templateId = entry.templateId;
    state.name = entry.name;
    state.version = next;
    state.publishDir = entry.publishDir;
    Serialization::NodeSerializationPtr templateNode;
    if (NodePtr live = findTemplateNode(key)) {
        templateNode = std::make_shared<Serialization::NodeSerialization>();
        live->toSerialization(templateNode.get());
        templateNode->inputs.clear();
        templateNode->origin = Serialization::NodeOriginSerialization();
        templateNode->cacheID.clear();
        Serialization::writeTemplateNode(state, templateNode.get());
    } else {
        double x = 0.;
        double y = 0.;
        const bool hasPosition = ProjectTemplateDetail::positionAbove(members, &x, &y);
        templateNode = ProjectTemplateDetail::makeTemplateNode(state, "Template1", hasPosition, x, y);
    }

    Serialization::ProjectSerialization published = mine;
    published.nodes.nodes = members;
    published.nodes.nodes.push_back(templateNode);
    published.templates.clear();
    published.mergeConflicts.clear();

    Serialization::ProjectSerialization written;
    std::string writtenYaml;
    try {
        Serialization::ProjectBundleWriter writer;
        writer.setProject(published);
        writer.setRootFolder(versionName.toStdString());
        Serialization::BundleManifest manifest;
        manifest.appVersion = NATRON_VERSION_STRING;
        manifest.appCommit = GIT_COMMIT;
        manifest.saveDate = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
        manifest.creationDate = getProjectCreationTime();
        writer.setManifest(manifest);
        writeProjectFileAtomically(filePath, [&writer](Serialization::ZipOutput& output) { writer.write(output); }, [&]() {
            // Another publish of the same version may have finished meanwhile.
            if (QFileInfo::exists(filePath)) {
                throw std::runtime_error(tr("%1 was published meanwhile: update to it before publishing").arg(filePath).toStdString());
            } });
        Serialization::ProjectBundleReader bundle(filePath.toStdString());
        written = bundle.readProject();
        writtenYaml = std::string(bundle.projectYaml());
    } catch (const std::exception& e) {
        return fail(tr("The template %1 could not be published: %2").arg(QString::fromStdString(entry.name)).arg(QString::fromUtf8(e.what())));
    }
    out.path = filePath;

    out.ops.clear();
    for (const std::string& id : newMembers) {
        out.ops.push_back(Serialization::SetOriginOp { id, key, id });
    }
    if (!ProjectTemplateDetail::applyOps(shared_from_this(), &out)) {
        return fail(tr("%1 was published, but its new members could not be tagged: %2").arg(filePath).arg(out.error));
    }

    written.templates.clear();
    written.mergeConflicts.clear();
    Serialization::eraseTemplateNodes(&written.nodes.nodes);
    entry.base = std::make_shared<Serialization::ProjectSerialization>(written);
    entry.version = next;
    entry.path = filePath.toStdString();
    entry.contentHash = Serialization::sha256Hex(writtenYaml);
    entry.importedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    setTemplateImport(entry);
    out.templateNode = findTemplateNode(key);

    return true;
}

NATRON_NAMESPACE_EXIT
