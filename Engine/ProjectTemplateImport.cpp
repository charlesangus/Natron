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

#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <QDateTime>
#include <QFileInfo>
#include <QThread>
#include <QUuid>

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/GraphOpApply.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/ProjectPrivate.h"
#include "Engine/ProjectTemplate.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Merge/NodeReferences.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/GraphOp.h"
#include "Serialization/Ops/OpsPrivate.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Sha256.h"
#include "Serialization/TemplateNodeSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {

QString
fileStem(const QString& filePath)
{
    QString name = QFileInfo(filePath).fileName();
    if (name.endsWith(QString::fromUtf8(".ntpz"), Qt::CaseInsensitive)) {
        name.chop(5);
    }

    return name;
}

void
stampImportedNode(Serialization::NodeSerialization* node,
                  const std::string& key)
{
    node->origin.templateKey = key;
    node->origin.nodeId = node->id;
    if (node->id.empty()) {
        node->origin = Serialization::NodeOriginSerialization();
    }
    node->id = ProjectTemplateDetail::newId();
    node->cacheID.clear();
    for (const Serialization::NodeSerializationPtr& child : node->children) {
        stampImportedNode(child.get(), key);
    }
}

void
collectInputOps(const std::list<Serialization::NodeSerializationPtr>& topLevel,
                std::vector<Serialization::GraphOp>* ops)
{
    Serialization::OpsDetail::InputRefs refs;
    Serialization::OpsDetail::resolveInputs(topLevel, &refs);
    for (const auto& entry : refs) {
        for (const auto& input : entry.second) {
            if (!input.second.sourceId.empty()) {
                ops->push_back(Serialization::SetInputOp { entry.first->id, input.first, input.second.sourceId });
            }
        }
    }
}

} // namespace

namespace ProjectTemplateDetail {
std::string
keyStem(const QString& name)
{
    std::string stem;
    for (QChar c : name) {
        const char ascii = c.toLatin1();
        const bool ok = ((ascii >= 'a') && (ascii <= 'z')) || ((ascii >= 'A') && (ascii <= 'Z')) || ((ascii >= '0') && (ascii <= '9')) || (ascii == '_');
        stem.push_back(ok ? ascii : '_');
    }

    return stem.empty() ? std::string("template") : stem;
}

std::string
uniqueTemplateKey(const std::string& stem,
                  const std::list<Serialization::TemplateImportSerialization>& existing)
{
    std::set<std::string> taken;
    for (const Serialization::TemplateImportSerialization& entry : existing) {
        taken.insert(entry.key);
    }
    if (taken.find(stem) == taken.end()) {
        return stem;
    }
    for (int n = 2;; ++n) {
        const std::string candidate = stem + "_" + std::to_string(n);
        if (taken.find(candidate) == taken.end()) {
            return candidate;
        }
    }
}

std::string
uniqueScriptName(const std::string& name,
                 const NodeCollectionPtr& group,
                 const std::set<std::string>& picked)
{
    auto free = [&](const std::string& candidate) {
        return (picked.find(candidate) == picked.end()) && !group->checkIfNodeNameExists(candidate, nullptr);
    };
    if (free(name)) {
        return name;
    }
    std::size_t digits = name.size();
    while ((digits > 0) && (name[digits - 1] >= '0') && (name[digits - 1] <= '9')) {
        --digits;
    }
    const std::string stem = name.substr(0, digits);
    int n = (digits < name.size()) ? std::stoi(name.substr(digits)) : 1;
    for (;;) {
        ++n;
        const std::string candidate = stem + std::to_string(n);
        if (free(candidate)) {
            return candidate;
        }
    }
}

std::string
newId()
{
    return QUuid::createUuid().toString(QUuid::Id128).toStdString();
}

Serialization::NodeSerializationPtr
makeTemplateNode(const Serialization::TemplateNodeState& state,
                 const std::string& scriptName,
                 bool hasPosition,
                 double x,
                 double y)
{
    Serialization::NodeSerializationPtr node = std::make_shared<Serialization::NodeSerialization>();
    node->id = newId();
    node->pluginID = PLUGINID_NATRON_TEMPLATE;
    node->scriptName = scriptName;
    node->label = scriptName;
    node->layout.hasPosition = hasPosition;
    node->layout.x = x;
    node->layout.y = y;
    Serialization::writeTemplateNode(state, node.get());

    return node;
}

bool
positionAbove(const std::list<Serialization::NodeSerializationPtr>& nodes,
              double* x,
              double* y)
{
    // Room for the template node's own height between it and the top member.
    const double gap = 100.;
    bool found = false;
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        if (!node || !node->layout.hasPosition) {
            continue;
        }
        if (!found || (node->layout.x < *x)) {
            *x = node->layout.x;
        }
        if (!found || (node->layout.y - gap < *y)) {
            *y = node->layout.y - gap;
        }
        found = true;
    }

    return found;
}

bool
applyOps(const ProjectPtr& project,
         TemplateEdit* edit)
{
    if (edit->ops.empty()) {
        return true;
    }
    edit->results = edit->applier ? edit->applier(edit->ops) : ProjectMerge::applyAllOrNothing(project, edit->ops);
    QStringList problems;
    for (const Serialization::OpResult& result : edit->results) {
        if (!result.applied()) {
            problems.push_back(QString::fromStdString(result.message));
        }
    }
    if (edit->results.size() != edit->ops.size()) {
        problems.push_back(Project::tr("the operations were not all applied"));
    }
    if (!problems.isEmpty()) {
        edit->error = Project::tr("The template could not be changed: %1").arg(problems.join(QString::fromUtf8("; ")));

        return false;
    }

    return true;
}
} // namespace ProjectTemplateDetail

bool
Project::isLockedPyPlug(const NodeCollectionPtr& group)
{
    NodeGroupPtr isGroup = std::dynamic_pointer_cast<NodeGroup>(group);
    NodePtr node = isGroup ? isGroup->getNode() : NodePtr();
    if (!node || !node->isPyPlug()) {
        return false;
    }

    return !isGroup->isSubGraphEditable() || !node->hasPyPlugBeenEdited();
}

NodesList
Project::importTemplate(const QString& filePath,
                        const NodeCollectionPtr& group,
                        QString* error)
{
    assert(QThread::currentThread() == qApp->thread());
    auto setError = [error](const QString& message) {
        if (error) {
            *error = message;
        }
    };
    ProjectPtr self = shared_from_this();
    NodeCollectionPtr target = group ? group : NodeCollectionPtr(self);
    std::string parentId;
    if (NodeGroupPtr isGroup = std::dynamic_pointer_cast<NodeGroup>(target)) {
        NodePtr groupNode = isGroup->getNode();
        if (!groupNode) {
            setError(tr("The group to import into has no node"));

            return NodesList();
        }
        if (isLockedPyPlug(isGroup)) {
            setError(tr("%1 is a PyPlug that is not unlocked for editing").arg(QString::fromStdString(groupNode->getScriptName_mt_safe())));

            return NodesList();
        }
        parentId = groupNode->getNodeId();
    }

    Serialization::TemplateImportSerialization entry;
    Serialization::ProjectSerialization imported;
    try {
        Serialization::ProjectBundleReader bundle(filePath.toStdString());
        Serialization::YamlWarnings warnings;
        imported = bundle.readProject(&warnings);
        entry.contentHash = Serialization::sha256Hex(bundle.projectYaml());
        for (const std::string& warning : warnings) {
            addLoadWarning(QString::fromUtf8(warning.c_str()));
        }
    } catch (const std::exception& e) {
        setError(tr("%1 cannot be imported as a template: %2").arg(filePath).arg(QString::fromUtf8(e.what())));

        return NodesList();
    }
    // A template's own imports are its business: only its nodes are the merge base here.
    imported.templates.clear();
    entry.key = ProjectTemplateDetail::uniqueTemplateKey(ProjectTemplateDetail::keyStem(fileStem(filePath)), getTemplateImports());
    entry.path = QFileInfo(filePath).absoluteFilePath().toStdString();
    entry.publishDir = QFileInfo(filePath).absolutePath().toStdString();
    entry.importedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    std::optional<Serialization::NodeLayoutSerialization> templateNodeLayout;
    if (const Serialization::NodeSerialization* templateNode = Serialization::findTemplateNode(imported.nodes.nodes)) {
        const Serialization::TemplateNodeState state = Serialization::readTemplateNode(*templateNode);
        entry.templateId = state.templateId;
        entry.name = state.name;
        entry.version = state.version;
        templateNodeLayout = templateNode->layout;
    }
    if (entry.templateId.empty()) {
        entry.templateId = ProjectTemplateDetail::newId();
        entry.name = fileStem(filePath).toStdString();
        entry.version = 1;
    }
    Serialization::eraseTemplateNodes(&imported.nodes.nodes);
    entry.base = std::make_shared<Serialization::ProjectSerialization>(imported);

    std::list<Serialization::NodeSerializationPtr> nodes;
    for (const Serialization::NodeSerializationPtr& node : imported.nodes.nodes) {
        nodes.push_back(Serialization::OpsDetail::deepCopyNode(*node));
    }
    Serialization::ScopedNodeRenames scoped;
    Serialization::NodeRenames& renames = scoped[std::string()];
    std::set<std::string> picked;
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        const std::string name = ProjectTemplateDetail::uniqueScriptName(node->scriptName, target, picked);
        picked.insert(name);
        if (name != node->scriptName) {
            renames[node->scriptName] = name;
        }
    }
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        Serialization::renameNodeReferencesRecursive(node.get(), scoped, std::string());
        for (std::pair<const std::string, std::string>& input : node->inputs) {
            Serialization::NodeRenames::const_iterator found = renames.find(input.second);
            if (found != renames.end()) {
                input.second = found->second;
            }
        }
        Serialization::NodeRenames::const_iterator own = renames.find(node->scriptName);
        if (own != renames.end()) {
            if (node->label == node->scriptName) {
                node->label = own->second;
            }
            node->scriptName = own->second;
        }
        stampImportedNode(node.get(), entry.key);
    }

    Serialization::TemplateNodeState state;
    state.key = entry.key;
    state.templateId = entry.templateId;
    state.name = entry.name;
    state.version = entry.version;
    state.importedVersion = entry.version;
    state.publishDir = entry.publishDir;
    double x = 0.;
    double y = 0.;
    bool hasPosition = false;
    if (templateNodeLayout && templateNodeLayout->hasPosition) {
        hasPosition = true;
        x = templateNodeLayout->x;
        y = templateNodeLayout->y;
    } else {
        hasPosition = ProjectTemplateDetail::positionAbove(nodes, &x, &y);
    }
    const Serialization::NodeSerializationPtr templateNode = ProjectTemplateDetail::makeTemplateNode(state, ProjectTemplateDetail::uniqueScriptName("Template1", target, picked), hasPosition, x, y);

    std::vector<Serialization::GraphOp> ops;
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        ops.push_back(Serialization::CreateNodeOp { parentId, node });
    }
    const std::size_t createCount = ops.size();
    ops.push_back(Serialization::CreateNodeOp { parentId, templateNode });
    collectInputOps(nodes, &ops);

    {
        QMutexLocker k(&_imp->mergeStateMutex);
        _imp->templateImports.push_back(entry);
    }
    const std::vector<Serialization::OpResult> results = applyGraphOps(self, ops);
    NodesList created;
    QStringList problems;
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (!results[i].applied()) {
            problems.push_back(QString::fromUtf8(results[i].message.c_str()));
        }
    }
    std::size_t i = 0;
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        if ((i < createCount) && results[i].applied()) {
            if (NodePtr made = findNodeById(node->id)) {
                created.push_back(made);
            }
        }
        ++i;
    }
    if (created.empty() && !nodes.empty()) {
        if (NodePtr made = findNodeById(templateNode->id)) {
            made->destroyNode(true, false);
        }
        {
            QMutexLocker k(&_imp->mergeStateMutex);
            _imp->templateImports.pop_back();
        }
        setError(tr("No node of %1 could be created: %2").arg(filePath).arg(problems.join(QString::fromUtf8("; "))));

        return NodesList();
    }
    Q_EMIT templateImportsChanged();
    if (!problems.isEmpty()) {
        setError(problems.join(QString::fromUtf8("; ")));
    }

    return created;
}

NATRON_NAMESPACE_EXIT
