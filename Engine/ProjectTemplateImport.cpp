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
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <QDateTime>
#include <QFileInfo>
#include <QThread>
#include <QUuid>

#include "Engine/AppInstance.h"
#include "Engine/GraphOpApply.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/ProjectPrivate.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Merge/NodeReferences.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/GraphOp.h"
#include "Serialization/Ops/OpsPrivate.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Sha256.h"

NATRON_NAMESPACE_ENTER

namespace {

std::string
templateKeyStem(const QString& filePath)
{
    QString name = QFileInfo(filePath).fileName();
    if (name.endsWith(QString::fromUtf8(".ntpz"), Qt::CaseInsensitive)) {
        name.chop(5);
    }
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

// Grade1 is taken, so Grade2, then Grade3; TplIn becomes TplIn2.
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
newNodeId()
{
    return QUuid::createUuid().toString(QUuid::Id128).toStdString();
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
    node->id = newNodeId();
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
    entry.key = uniqueTemplateKey(templateKeyStem(filePath), _imp->templateImports);
    entry.path = QFileInfo(filePath).absoluteFilePath().toStdString();
    entry.importedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    entry.base = std::make_shared<Serialization::ProjectSerialization>(imported);

    std::list<Serialization::NodeSerializationPtr> nodes;
    for (const Serialization::NodeSerializationPtr& node : imported.nodes.nodes) {
        nodes.push_back(Serialization::OpsDetail::deepCopyNode(*node));
    }
    Serialization::NodeRenames renames;
    std::set<std::string> picked;
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        const std::string name = uniqueScriptName(node->scriptName, target, picked);
        picked.insert(name);
        if (name != node->scriptName) {
            renames[node->scriptName] = name;
        }
    }
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        Serialization::renameNodeReferencesRecursive(node.get(), renames);
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

    std::vector<Serialization::GraphOp> ops;
    for (const Serialization::NodeSerializationPtr& node : nodes) {
        ops.push_back(Serialization::CreateNodeOp { parentId, node });
    }
    const std::size_t createCount = ops.size();
    collectInputOps(nodes, &ops);

    _imp->templateImports.push_back(entry);
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
        _imp->templateImports.pop_back();
        setError(tr("No node of %1 could be created: %2").arg(filePath).arg(problems.join(QString::fromUtf8("; "))));

        return NodesList();
    }
    if (!problems.isEmpty()) {
        setError(problems.join(QString::fromUtf8("; ")));
    }

    return created;
}

NATRON_NAMESPACE_EXIT
