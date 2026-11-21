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

#include "SessionShared.h"

#include <cctype>
#include <type_traits>
#include <variant>

#include <QtCore/QCryptographicHash>
#include <QtCore/QString>

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Bundle/ZipStore.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/ManifestYaml.h"

NATRON_NAMESPACE_ENTER
namespace LiveSession {

namespace {

    class VectorZipOutput
        : public Serialization::ZipOutput {
    public:
        explicit VectorZipOutput(std::vector<std::byte>* bytes)
            : _bytes(bytes)
        {
        }

        void write(const void* data,
                   std::size_t size) override
        {
            const std::byte* begin = static_cast<const std::byte*>(data);
            _bytes->insert(_bytes->end(), begin, begin + size);
        }

    private:
        std::vector<std::byte>* _bytes;
    };

    const char*
    knobOwner(const std::optional<std::string>& nodeId)
    {
        return nodeId ? nodeId->c_str() : "project";
    }

    void
    writerFor(const ProjectPtr& project,
              Serialization::ProjectBundleWriter* writer)
    {
        Serialization::ProjectSerialization serialization;
        project->toSerialization(&serialization);
        serialization.timelineCurrentTime = 0;
        writer->setProject(serialization);
    }

    // Connecting an input on the peer turns its mask switch on there, as an edit would, while
    // here it may have been turned off since: the switch is set to its state here afterwards.
    void
    appendMaskSwitch(const NodePtr& node,
                     int inputNb,
                     std::vector<Serialization::GraphOp>* switches)
    {
        KnobBoolPtr maskSwitch = node->getMaskEnabledKnob(inputNb);
        if (!maskSwitch) {
            return;
        }
        Serialization::SetKnobOp op;
        op.nodeId = node->getNodeId();
        op.knobName = maskSwitch->getName();
        Serialization::KnobSerialization value;
        if (maskSwitch->toSerialization(&value)) {
            op.value = value;
        }
        switches->push_back(Serialization::GraphOp(op));
    }

    void
    appendInputs(const NodePtr& node,
                 std::vector<Serialization::GraphOp>* ops,
                 std::vector<Serialization::GraphOp>* switches)
    {
        if (node->getParentMultiInstance()) {
            return;
        }
        const int nInputs = node->getNInputs();
        for (int i = 0; i < nInputs; ++i) {
            NodePtr source = node->getRealGuiInput(i);
            if (source) {
                ops->push_back(Serialization::GraphOp(Serialization::SetInputOp { node->getNodeId(), node->getInputLabel(i), source->getNodeId() }));
                appendMaskSwitch(node, i, switches);
            }
        }
    }

} // namespace

std::string
nodeExistenceKey(const std::string& nodeId)
{
    return "node:" + nodeId;
}

std::vector<std::string>
graphOpKeys(const Serialization::GraphOp& op)
{
    using namespace Serialization;

    return std::visit([](const auto& edit) -> std::vector<std::string> {
        using T = std::decay_t<decltype(edit)>;
        if constexpr (std::is_same_v<T, CreateNodeOp>) {
            return { nodeExistenceKey(edit.node ? edit.node->id : std::string()) };
        } else if constexpr (std::is_same_v<T, DeleteNodeOp>) {
            return { nodeExistenceKey(edit.nodeId) };
        } else if constexpr (std::is_same_v<T, SetKnobOp>) {
            return { std::string("knob:") + knobOwner(edit.nodeId) + "/" + edit.knobName };
        } else if constexpr (std::is_same_v<T, SetInputOp>) {
            return { "input:" + edit.nodeId + "/" + edit.inputLabel };
        } else if constexpr (std::is_same_v<T, RenameOp>) {
            return { "name:" + edit.nodeId };
        } else if constexpr (std::is_same_v<T, SetNodeUiOp>) {
            return { "ui:" + edit.nodeId };
        } else if constexpr (std::is_same_v<T, SetUserKnobsOp>) {
            return { "userKnobs:" + edit.nodeId };
        } else if constexpr (std::is_same_v<T, SetRotoDataOp>) {
            return { "roto:" + edit.nodeId };
        } else if constexpr (std::is_same_v<T, SetTrackerDataOp>) {
            return { "tracker:" + edit.nodeId };
        } else {
            static_assert(std::is_same_v<T, SetProjectDataOp>);
            std::vector<std::string> keys;
            if (edit.formats) {
                keys.push_back("project:formats");
            }
            if (edit.layers) {
                keys.push_back("project:layers");
            }
            if (edit.variables) {
                keys.push_back("project:variables");
            }

            return keys;
        }
    },
                      op);
}

std::optional<std::string>
graphOpTargetNode(const Serialization::GraphOp& op)
{
    using namespace Serialization;

    return std::visit([](const auto& edit) -> std::optional<std::string> {
        using T = std::decay_t<decltype(edit)>;
        if constexpr (std::is_same_v<T, CreateNodeOp>) {
            return edit.node ? std::optional<std::string>(edit.node->id) : std::nullopt;
        } else if constexpr (std::is_same_v<T, SetKnobOp>) {
            return edit.nodeId;
        } else if constexpr (std::is_same_v<T, SetProjectDataOp>) {
            return std::nullopt;
        } else {
            return edit.nodeId;
        }
    },
                      op);
}

std::vector<std::string>
graphOpNodeIds(const Serialization::GraphOp& op)
{
    std::vector<std::string> ids;
    if (std::optional<std::string> target = graphOpTargetNode(op)) {
        ids.push_back(*target);
    }
    if (const Serialization::SetInputOp* input = std::get_if<Serialization::SetInputOp>(&op)) {
        if (!input->sourceId.empty()) {
            ids.push_back(input->sourceId);
        }
    } else if (const Serialization::CreateNodeOp* create = std::get_if<Serialization::CreateNodeOp>(&op)) {
        if (!create->parentId.empty()) {
            ids.push_back(create->parentId);
        }
    }

    return ids;
}

std::string
canonicalProjectYaml(const ProjectPtr& project)
{
    Serialization::ProjectBundleWriter writer;
    writerFor(project, &writer);

    return writer.projectYaml();
}

std::string
projectChecksum(const ProjectPtr& project)
{
    Serialization::ProjectBundleWriter writer;
    writerFor(project, &writer);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const std::string& yaml = writer.projectYaml();
    hash.addData(QByteArrayView(yaml.data(), static_cast<qsizetype>(yaml.size())));
    for (const auto& member : writer.blobs().members()) {
        hash.addData(QByteArrayView(member.first.data(), static_cast<qsizetype>(member.first.size() + 1)));
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(member.second.data()), static_cast<qsizetype>(member.second.size())));
    }

    return hash.result().toHex().toStdString();
}

std::vector<std::byte>
projectBundleBytes(const ProjectPtr& project)
{
    Serialization::ProjectBundleWriter writer;
    Serialization::ProjectSerialization serialization;
    project->toSerialization(&serialization);
    writer.setProject(serialization);
    QString stem = project->getProjectFilename();
    if (stem.endsWith(QString::fromUtf8(".ntpz"), Qt::CaseInsensitive)) {
        stem.chop(5);
    }
    writer.setRootFolder(stem.toStdString());
    Serialization::BundleManifest manifest;
    manifest.appVersion = NATRON_VERSION_STRING;
    manifest.creationDate = project->getProjectCreationTime();
    writer.setManifest(manifest);

    std::vector<std::byte> bytes;
    VectorZipOutput output(&bytes);
    writer.write(output);

    return bytes;
}

std::string
parentIdOf(const NodePtr& node)
{
    if (NodePtr mainInstance = node->getParentMultiInstance()) {
        return mainInstance->getNodeId();
    }
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(node->getGroup());
    if (group) {
        NodePtr groupNode = group->getNode();
        if (groupNode) {
            return groupNode->getNodeId();
        }
    }

    return std::string();
}

NodeCollectionPtr
collectionForParent(const ProjectPtr& project,
                    const std::string& parentId)
{
    if (parentId.empty()) {
        return project;
    }
    NodePtr parent = project->findNodeById(parentId);
    if (!parent) {
        return NodeCollectionPtr();
    }
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(parent->getEffectInstance());
    if (group) {
        return group;
    }

    return parent->getGroup();
}

std::string
uniqueScriptName(const NodeCollectionPtr& collection,
                 const std::string& wanted,
                 const Node* except,
                 const std::set<std::string>& taken)
{
    const auto isFree = [&](const std::string& name) {
        return !name.empty() && !taken.count(name) && (!collection || !collection->checkIfNodeNameExists(name, except));
    };
    if (isFree(wanted)) {
        return wanted;
    }
    std::size_t end = wanted.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(wanted[end - 1]))) {
        --end;
    }
    std::string base = wanted.substr(0, end);
    if (base.empty()) {
        base = "Node";
    }
    for (unsigned long number = 1;; ++number) {
        const std::string candidate = base + std::to_string(number);
        if (isFree(candidate)) {
            return candidate;
        }
    }
}

std::vector<Serialization::GraphOp>
nodeStateOps(const NodePtr& node)
{
    std::vector<Serialization::GraphOp> ops;
    Serialization::CreateNodeOp create;
    create.parentId = parentIdOf(node);
    create.node = std::make_shared<Serialization::NodeSerialization>();
    node->toSerialization(create.node.get());
    ops.push_back(Serialization::GraphOp(create));

    std::vector<Serialization::GraphOp> switches;
    appendInputs(node, &ops, &switches);
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(node->getEffectInstance());
    if (group) {
        NodesList inside;
        group->getNodes_recursive(inside, true);
        for (const NodePtr& child : inside) {
            appendInputs(child, &ops, &switches);
        }
    }

    const NodesWList outputs = node->getOutputs();
    std::set<const Node*> done;
    for (const NodeWPtr& weak : outputs) {
        NodePtr output = weak.lock();
        if (!output || !output->isActivated() || output->getParentMultiInstance() || !done.insert(output.get()).second) {
            continue;
        }
        const int nInputs = output->getNInputs();
        for (int i = 0; i < nInputs; ++i) {
            if (output->getRealGuiInput(i) == node) {
                ops.push_back(Serialization::GraphOp(Serialization::SetInputOp { output->getNodeId(), output->getInputLabel(i), node->getNodeId() }));
                appendMaskSwitch(output, i, &switches);
            }
        }
    }
    ops.insert(ops.end(), switches.begin(), switches.end());

    return ops;
}

} // namespace LiveSession
NATRON_NAMESPACE_EXIT
