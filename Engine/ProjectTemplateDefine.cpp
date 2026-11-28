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

#include <list>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QDateTime>
#include <QThread>

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/ProjectTemplate.h"
#include "Engine/TemplateNode.h"

#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/GraphOp.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/TemplateNodeSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {

void
clearOrigins(Serialization::NodeSerialization* node)
{
    node->origin = Serialization::NodeOriginSerialization();
    for (const Serialization::NodeSerializationPtr& child : node->children) {
        clearOrigins(child.get());
    }
}

// node and the nodes inside it, as the IDs a graph operation names them by.
void
collectSubtree(const NodePtr& node,
               NodesList* nodes)
{
    nodes->push_back(node);
    if (NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(node->getEffectInstance())) {
        for (const NodePtr& child : group->getNodes()) {
            collectSubtree(child, nodes);
        }
    }
}

bool
isTemplateNode(const NodePtr& node)
{
    return node && (node->getPluginID() == PLUGINID_NATRON_TEMPLATE);
}

std::optional<Serialization::TemplateImportSerialization>
entryNamed(const ProjectPtr& project,
           const std::string& key)
{
    for (const Serialization::TemplateImportSerialization& entry : project->getTemplateImports()) {
        if (entry.key == key) {
            return entry;
        }
    }

    return std::nullopt;
}

} // namespace

NodePtr
Project::findTemplateNode(const std::string& key) const
{
    if (key.empty()) {
        return NodePtr();
    }
    NodesList nodes;
    getNodes_recursive(nodes, true);
    for (const NodePtr& node : nodes) {
        if (!isTemplateNode(node)) {
            continue;
        }
        TemplateNode* effect = dynamic_cast<TemplateNode*>(node->getEffectInstance().get());
        if (effect && (effect->getTemplateKey() == key)) {
            return node;
        }
    }

    return NodePtr();
}

bool
Project::defineTemplate(const NodesList& nodes,
                        const std::string& name,
                        const NodeCollectionPtr& group,
                        TemplateEdit* edit)
{
    assert(QThread::currentThread() == qApp->thread());
    TemplateEdit local;
    TemplateEdit& out = edit ? *edit : local;
    ProjectPtr self = shared_from_this();
    NodeCollectionPtr target = group ? group : NodeCollectionPtr(self);
    std::string parentId;
    if (NodeGroupPtr isGroup = std::dynamic_pointer_cast<NodeGroup>(target)) {
        NodePtr groupNode = isGroup->getNode();
        if (!groupNode) {
            out.error = tr("The group to define the template in has no node");

            return false;
        }
        if (isLockedPyPlug(isGroup)) {
            out.error = tr("%1 is a PyPlug that is not unlocked for editing").arg(QString::fromStdString(groupNode->getScriptName_mt_safe()));

            return false;
        }
        parentId = groupNode->getNodeId();
    }

    NodesList members;
    if (nodes.empty()) {
        for (const NodePtr& node : target->getNodes()) {
            if (node && node->isActivated() && !isTemplateNode(node) && node->getOriginTemplate().empty()) {
                members.push_back(node);
            }
        }
    } else {
        for (const NodePtr& node : nodes) {
            if (!node) {
                continue;
            }
            if (node->getGroup() != target) {
                out.error = tr("%1 is not in the group the template is defined in").arg(QString::fromStdString(node->getScriptName_mt_safe()));

                return false;
            }
            if (isTemplateNode(node)) {
                out.error = tr("%1 is a template node, which cannot be a member of a template").arg(QString::fromStdString(node->getScriptName_mt_safe()));

                return false;
            }
            if (!node->getOriginTemplate().empty()) {
                out.error = tr("%1 already belongs to the template %2").arg(QString::fromStdString(node->getScriptName_mt_safe())).arg(QString::fromStdString(node->getOriginTemplate()));

                return false;
            }
            members.push_back(node);
        }
    }
    if (members.empty()) {
        out.error = tr("There are no nodes to make a template of");

        return false;
    }

    const std::list<Serialization::TemplateImportSerialization> before = getTemplateImports();
    Serialization::TemplateImportSerialization entry;
    entry.name = name.empty() ? std::string("template") : name;
    entry.key = ProjectTemplateDetail::uniqueTemplateKey(ProjectTemplateDetail::keyStem(QString::fromStdString(entry.name)), before);
    entry.templateId = ProjectTemplateDetail::newId();
    entry.version = 0;
    entry.publishDir = getProjectPath().toStdString();
    entry.importedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    std::shared_ptr<Serialization::ProjectSerialization> base = std::make_shared<Serialization::ProjectSerialization>();
    for (const NodePtr& member : members) {
        Serialization::NodeSerializationPtr snapshot = std::make_shared<Serialization::NodeSerialization>();
        member->toSerialization(snapshot.get());
        clearOrigins(snapshot.get());
        base->nodes.nodes.push_back(snapshot);
    }
    entry.base = base;

    Serialization::TemplateNodeState state;
    state.key = entry.key;
    state.templateId = entry.templateId;
    state.name = entry.name;
    state.publishDir = entry.publishDir;
    double x = 0.;
    double y = 0.;
    const bool hasPosition = ProjectTemplateDetail::positionAbove(base->nodes.nodes, &x, &y);
    const Serialization::NodeSerializationPtr templateNode = ProjectTemplateDetail::makeTemplateNode(state, ProjectTemplateDetail::uniqueScriptName("Template1", target, std::set<std::string>()), hasPosition, x, y);

    out.key = entry.key;
    out.ops.clear();
    out.ops.push_back(Serialization::CreateNodeOp { parentId, templateNode });
    for (const NodePtr& member : members) {
        NodesList subtree;
        collectSubtree(member, &subtree);
        for (const NodePtr& node : subtree) {
            out.ops.push_back(Serialization::SetOriginOp { node->getNodeId(), entry.key, node->getNodeId() });
        }
    }
    setTemplateImport(entry);
    if (!ProjectTemplateDetail::applyOps(self, &out)) {
        setTemplateImports(before);

        return false;
    }
    out.templateNode = findNodeById(templateNode->id);

    return true;
}

bool
Project::addToTemplate(const std::string& key,
                       const NodePtr& node,
                       TemplateEdit* edit)
{
    assert(QThread::currentThread() == qApp->thread());
    TemplateEdit local;
    TemplateEdit& out = edit ? *edit : local;
    out.key = key;
    std::optional<Serialization::TemplateImportSerialization> entry = entryNamed(shared_from_this(), key);
    if (!entry) {
        out.error = tr("The project has no template import named %1").arg(QString::fromStdString(key));

        return false;
    }
    if (!node || isTemplateNode(node)) {
        out.error = tr("Only a node other than a template node can be added to a template");

        return false;
    }
    const std::string current = node->getOriginTemplate();
    if (!current.empty() && (current != key)) {
        out.error = tr("%1 already belongs to the template %2").arg(QString::fromStdString(node->getScriptName_mt_safe())).arg(QString::fromStdString(current));

        return false;
    }
    NodesList subtree;
    collectSubtree(node, &subtree);
    out.ops.clear();
    bool excludedChanged = false;
    for (const NodePtr& member : subtree) {
        if (member->getOriginTemplate().empty()) {
            out.ops.push_back(Serialization::SetOriginOp { member->getNodeId(), key, member->getNodeId() });
        }
        excludedChanged = (entry->excluded.erase(member->getNodeId()) > 0) || excludedChanged;
    }
    if (!ProjectTemplateDetail::applyOps(shared_from_this(), &out)) {
        return false;
    }
    if (excludedChanged) {
        setTemplateImport(*entry);
    }
    out.templateNode = findTemplateNode(key);

    return true;
}

bool
Project::removeFromTemplate(const std::string& key,
                            const NodePtr& node,
                            TemplateEdit* edit)
{
    assert(QThread::currentThread() == qApp->thread());
    TemplateEdit local;
    TemplateEdit& out = edit ? *edit : local;
    out.key = key;
    std::optional<Serialization::TemplateImportSerialization> entry = entryNamed(shared_from_this(), key);
    if (!entry) {
        out.error = tr("The project has no template import named %1").arg(QString::fromStdString(key));

        return false;
    }
    if (!node || isTemplateNode(node)) {
        out.error = tr("Only a node other than a template node can be removed from a template");

        return false;
    }
    const std::string current = node->getOriginTemplate();
    if (!current.empty() && (current != key)) {
        out.error = tr("%1 belongs to the template %2, not to %3").arg(QString::fromStdString(node->getScriptName_mt_safe())).arg(QString::fromStdString(current)).arg(QString::fromStdString(key));

        return false;
    }
    NodesList subtree;
    collectSubtree(node, &subtree);
    out.ops.clear();
    bool excludedChanged = false;
    for (const NodePtr& member : subtree) {
        if (member->getOriginTemplate() == key) {
            out.ops.push_back(Serialization::SetOriginOp { member->getNodeId(), std::string(), std::string() });
        }
        excludedChanged = entry->excluded.insert(member->getNodeId()).second || excludedChanged;
    }
    if (!ProjectTemplateDetail::applyOps(shared_from_this(), &out)) {
        return false;
    }
    if (excludedChanged) {
        setTemplateImport(*entry);
    }
    out.templateNode = findTemplateNode(key);

    return true;
}

bool
Project::detachTemplate(const std::string& key,
                        TemplateEdit* edit)
{
    assert(QThread::currentThread() == qApp->thread());
    TemplateEdit local;
    TemplateEdit& out = edit ? *edit : local;
    out.key = key;
    if (!hasTemplateImport(key)) {
        out.error = tr("The project has no template import named %1").arg(QString::fromStdString(key));

        return false;
    }
    NodesList nodes;
    getNodes_recursive(nodes, true);
    out.ops.clear();
    std::vector<Serialization::GraphOp> deletes;
    for (const NodePtr& node : nodes) {
        if (node->getOriginTemplate() == key) {
            out.ops.push_back(Serialization::SetOriginOp { node->getNodeId(), std::string(), std::string() });
        }
        if (isTemplateNode(node)) {
            TemplateNode* effect = dynamic_cast<TemplateNode*>(node->getEffectInstance().get());
            if (effect && (effect->getTemplateKey() == key)) {
                deletes.push_back(Serialization::DeleteNodeOp { node->getNodeId() });
            }
        }
    }
    out.ops.insert(out.ops.end(), deletes.begin(), deletes.end());
    if (!ProjectTemplateDetail::applyOps(shared_from_this(), &out)) {
        return false;
    }

    std::list<Serialization::TemplateImportSerialization> imports = getTemplateImports();
    imports.remove_if([&key](const Serialization::TemplateImportSerialization& entry) {
        return entry.key == key;
    });
    setTemplateImports(imports);
    std::vector<Serialization::MergeConflict> conflicts = getMergeConflicts();
    std::vector<Serialization::MergeConflict> kept;
    for (const Serialization::MergeConflict& conflict : conflicts) {
        if (conflict.templateKey != key) {
            kept.push_back(conflict);
        }
    }
    if (kept.size() != conflicts.size()) {
        setMergeConflicts(kept);
    }

    return true;
}

namespace ProjectTemplateDetail {
QString
updateStatusName(ProjectMerge::UpdateStatus status)
{
    switch (status) {
    case ProjectMerge::UpdateStatus::eUpdated:
        return QString::fromUtf8("updated");
    case ProjectMerge::UpdateStatus::eUpToDate:
        return QString::fromUtf8("upToDate");
    case ProjectMerge::UpdateStatus::eTemplateMismatch:
        return QString::fromUtf8("templateMismatch");
    case ProjectMerge::UpdateStatus::eLowMatch:
        return QString::fromUtf8("lowMatch");
    case ProjectMerge::UpdateStatus::eError:
        break;
    }

    return QString::fromUtf8("error");
}
} // namespace ProjectTemplateDetail

NATRON_NAMESPACE_EXIT
