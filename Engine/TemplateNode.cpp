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

#include "TemplateNode.h"

#include <algorithm>
#include <list>
#include <optional>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include "Engine/AppInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ProjectMerge.h"

#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/TemplateNodeSerialization.h"

NATRON_NAMESPACE_ENTER

struct TemplateNodePrivate {
    KnobStringWPtr key;
    KnobStringWPtr name;
    KnobStringWPtr templateId;
    KnobIntWPtr version;
    KnobPathWPtr publishDir;
    KnobStringWPtr newerVersion;
    KnobButtonWPtr publish;
    KnobButtonWPtr updateToLatest;
    KnobButtonWPtr updateFromFile;
    KnobButtonWPtr compareWithBase;
    KnobButtonWPtr detach;
    // Set while the knobs are being set from the entry, whose own values must not go back to it.
    bool refreshing = false;

    // The last newer-version check asked for; a scan or result of an earlier one is dropped.
    // Main thread only.
    unsigned checkSequence = 0;

    // What the last deactivation detached, until the node is active again.
    struct Detached {
        Serialization::TemplateImportSerialization entry;
        std::vector<Serialization::MergeConflict> conflicts;
        std::list<std::pair<NodeWPtr, std::string>> origins;
    };
    std::optional<Detached> detached;
};

TemplateNode::TemplateNode(NodePtr node)
    : NoOpBase(node)
    , _imp(new TemplateNodePrivate())
{
}

TemplateNode::~TemplateNode()
{
}

std::string
TemplateNode::getPluginDescription() const
{
    return tr("Stands for a template in the node graph: its name, identity and versions, and the "
              "actions that publish it or update it to another version.")
        .toStdString();
}

namespace {

KnobStringPtr
readOnlyString(TemplateNode* holder,
               const KnobPagePtr& page,
               const QString& label,
               const char* name,
               const QString& hint)
{
    KnobStringPtr knob = AppManager::createKnob<KnobString>(holder, label);
    knob->setName(name);
    knob->setAnimationEnabled(false);
    knob->setEvaluateOnChange(false);
    knob->setAllDimensionsEnabled(false);
    knob->setHintToolTip(hint);
    page->addKnob(knob);

    return knob;
}

KnobIntPtr
readOnlyInt(TemplateNode* holder,
            const KnobPagePtr& page,
            const QString& label,
            const char* name,
            const QString& hint)
{
    KnobIntPtr knob = AppManager::createKnob<KnobInt>(holder, label);
    knob->setName(name);
    knob->setAnimationEnabled(false);
    knob->setEvaluateOnChange(false);
    knob->setAllDimensionsEnabled(false);
    knob->setHintToolTip(hint);
    page->addKnob(knob);

    return knob;
}

KnobButtonPtr
button(TemplateNode* holder,
       const KnobPagePtr& page,
       const QString& label,
       const char* name,
       const QString& hint)
{
    KnobButtonPtr knob = AppManager::createKnob<KnobButton>(holder, label);
    knob->setName(name);
    knob->setEvaluateOnChange(false);
    knob->setHintToolTip(hint);
    page->addKnob(knob);

    return knob;
}

} // namespace

void
TemplateNode::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobStringPtr key = AppManager::createKnob<KnobString>(this, tr("Template Key"));
    key->setName(Serialization::kTemplateParamKey);
    key->setAnimationEnabled(false);
    key->setEvaluateOnChange(false);
    key->setSecretByDefault(true);
    page->addKnob(key);
    _imp->key = key;

    KnobStringPtr name = AppManager::createKnob<KnobString>(this, tr("Template Name"));
    name->setName(Serialization::kTemplateParamName);
    name->setAnimationEnabled(false);
    name->setEvaluateOnChange(false);
    name->setHintToolTip(tr("The name of the template, which its published files are named after."));
    page->addKnob(name);
    _imp->name = name;

    _imp->templateId = readOnlyString(this, page, tr("Template ID"), Serialization::kTemplateParamId,
                                      tr("The identity every version of the template shares. Update to Latest only takes files with this ID."));
    _imp->version = readOnlyInt(this, page, tr("Version"), Serialization::kTemplateParamVersion,
                                tr("The version of the template the graph was last merged with or published as, 0 for a template that was never published. Its snapshot is the base of the next update."));

    KnobPathPtr publishDir = AppManager::createKnob<KnobPath>(this, tr("Publish Dir"));
    publishDir->setName(Serialization::kTemplateParamPublishDir);
    publishDir->setAnimationEnabled(false);
    publishDir->setEvaluateOnChange(false);
    publishDir->setHintToolTip(tr("The folder the template's versions are published to and looked for in."));
    page->addKnob(publishDir);
    _imp->publishDir = publishDir;

    KnobStringPtr newer = readOnlyString(this, page, tr("Newer Version"), Serialization::kTemplateParamNewerVersion,
                                         tr("A newer published version of the template, empty when the graph has the latest."));
    newer->setIsPersistent(false);
    _imp->newerVersion = newer;

    _imp->publish = button(this, page, tr("Publish"), "publish",
                           tr("Writes the template's members as the next version in the publish folder."));
    _imp->updateToLatest = button(this, page, tr("Update To Latest"), "updateToLatest",
                                  tr("Merges the newest version in the publish folder into the graph, keeping the edits made here."));
    _imp->updateFromFile = button(this, page, tr("Update From File"), "updateFromFile",
                                  tr("Merges a chosen version of the template into the graph, keeping the edits made here."));
    _imp->compareWithBase = button(this, page, tr("Compare With Base"), "compareWithBase",
                                   tr("Shows how the template's members differ from the imported version."));
    _imp->detach = button(this, page, tr("Detach"), "detach",
                          tr("Makes the members ordinary nodes and removes the template; the members stay."));

    if (AppInstancePtr app = getApp()) {
        if (ProjectPtr project = app->getProject()) {
            QObject::connect(project.get(), &Project::templateImportsChanged, this, &TemplateNode::refreshFromEntry);
        }
    }
}

void
TemplateNode::onKnobsLoaded()
{
    // A saved knob restores its enabled state, and these must stay read only.
    for (const KnobIPtr& knob : { KnobIPtr(_imp->templateId.lock()), KnobIPtr(_imp->version.lock()), KnobIPtr(_imp->newerVersion.lock()) }) {
        if (knob) {
            knob->setAllDimensionsEnabled(false);
        }
    }
    refreshFromEntry();
}

std::string
TemplateNode::getTemplateKey() const
{
    KnobStringPtr key = _imp->key.lock();

    return key ? key->getValue() : std::string();
}

void
TemplateNode::setNewerVersion(const std::string& text)
{
    KnobStringPtr newer = _imp->newerVersion.lock();
    if (newer && (newer->getValue() != text)) {
        newer->setValue(text);
    }
}

void
TemplateNode::refreshFromEntry()
{
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    const std::string key = getTemplateKey();
    if (!project || key.empty() || _imp->refreshing) {
        return;
    }
    for (const Serialization::TemplateImportSerialization& entry : project->getTemplateImports()) {
        if (entry.key != key) {
            continue;
        }
        _imp->refreshing = true;
        _imp->templateId.lock()->setValue(entry.templateId);
        _imp->name.lock()->setValue(entry.name);
        _imp->version.lock()->setValue(entry.version);
        _imp->publishDir.lock()->setValue(entry.publishDir);
        _imp->refreshing = false;
        break;
    }
    checkForNewerVersion();
}

void
TemplateNode::onSettingsPanelOpened()
{
    checkForNewerVersion();
}

std::string
TemplateNode::getNewerVersion() const
{
    KnobStringPtr newer = _imp->newerVersion.lock();

    return newer ? newer->getValue() : std::string();
}

namespace {

TemplateNode*
templateNodeOf(const NodeWPtr& weakNode)
{
    const NodePtr node = weakNode.lock();

    return node ? dynamic_cast<TemplateNode*>(node->getEffectInstance().get()) : 0;
}

// Several template edits in a row make one scan.
constexpr int kVersionCheckDelayMs = 300;

} // namespace

void
TemplateNode::checkForNewerVersion()
{
    AppInstancePtr app = getApp();
    if (!app || app->isBackground() || (QThread::currentThread() != qApp->thread())) {
        return;
    }
    const unsigned sequence = ++_imp->checkSequence;
    const NodeWPtr weakNode = getNode();
    QTimer::singleShot(kVersionCheckDelayMs, qApp, [weakNode, sequence]() {
        TemplateNode* self = templateNodeOf(weakNode);
        if (!self || (self->_imp->checkSequence != sequence)) {
            return;
        }
        KnobPathPtr publishDir = self->_imp->publishDir.lock();
        KnobStringPtr templateId = self->_imp->templateId.lock();
        if (!publishDir || !templateId) {
            return;
        }
        const QString dir = QString::fromStdString(publishDir->getValue());
        const std::string id = templateId->getValue();
        if (dir.isEmpty() || id.empty()) {
            self->setNewerVersion(std::string());

            return;
        }
        ProjectMerge::folderScanPool()->start([dir, id, weakNode, sequence]() {
            const std::optional<ProjectMerge::TemplateVersion> latest = ProjectMerge::findLatestVersion(dir, id);
            QMetaObject::invokeMethod(qApp, [weakNode, sequence, latest]() {
                TemplateNode* self = templateNodeOf(weakNode);
                if (!self || (self->_imp->checkSequence != sequence)) {
                    return;
                }
                KnobIntPtr version = self->_imp->version.lock();
                const bool newer = version && latest && (latest->version > version->getValue());
                self->setNewerVersion(newer ? latest->path.toStdString() : std::string()); }, Qt::QueuedConnection);
        });
    });
}

namespace {

// The project, when removing a template node may detach its import: not while the project loads or
// closes, when nodes come and go with their entries, and not off the main thread.
ProjectPtr
projectForDetach(const AppInstancePtr& app)
{
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (!project || project->isProjectClosing() || project->isLoadingProject() || (QThread::currentThread() != qApp->thread())) {
        return ProjectPtr();
    }

    return project;
}

} // namespace

void
TemplateNode::onNodeDeactivated()
{
    const ProjectPtr project = projectForDetach(getApp());
    const std::string key = getTemplateKey();
    if (!project || key.empty() || project->findTemplateNode(key)) {
        return;
    }
    std::list<Serialization::TemplateImportSerialization> imports = project->getTemplateImports();
    std::list<Serialization::TemplateImportSerialization>::iterator found = std::find_if(imports.begin(), imports.end(), [&key](const Serialization::TemplateImportSerialization& entry) {
        return entry.key == key;
    });
    if (found == imports.end()) {
        return;
    }
    TemplateNodePrivate::Detached detached;
    detached.entry = *found;
    imports.erase(found);

    std::vector<Serialization::MergeConflict> kept;
    for (const Serialization::MergeConflict& conflict : project->getMergeConflicts()) {
        if (conflict.templateKey == key) {
            detached.conflicts.push_back(conflict);
        } else {
            kept.push_back(conflict);
        }
    }

    // Inactive nodes too: a member deleted before is one again if its removal is undone.
    NodesList nodes;
    project->getNodes_recursive(nodes, false);
    for (const NodePtr& node : nodes) {
        if (node->getOriginTemplate() == key) {
            detached.origins.push_back(std::make_pair(NodeWPtr(node), node->getOriginNode()));
            node->setOrigin(std::string(), std::string());
        }
    }
    project->setTemplateImports(imports);
    if (!detached.conflicts.empty()) {
        project->setMergeConflicts(kept);
    }
    _imp->detached = std::move(detached);
}

void
TemplateNode::onNodeActivated()
{
    if (!_imp->detached) {
        return;
    }
    const ProjectPtr project = projectForDetach(getApp());
    if (!project) {
        return;
    }
    const TemplateNodePrivate::Detached detached = std::move(*_imp->detached);
    _imp->detached.reset();
    // A paste of the node's cut copy took the entry back already.
    if (project->hasTemplateImport(detached.entry.key)) {
        return;
    }
    for (const std::pair<NodeWPtr, std::string>& origin : detached.origins) {
        const NodePtr node = origin.first.lock();
        if (node && node->getOriginTemplate().empty()) {
            node->setOrigin(detached.entry.key, origin.second);
        }
    }
    project->setTemplateImport(detached.entry);
    if (!detached.conflicts.empty()) {
        std::vector<Serialization::MergeConflict> conflicts = project->getMergeConflicts();
        conflicts.insert(conflicts.end(), detached.conflicts.begin(), detached.conflicts.end());
        project->setMergeConflicts(conflicts);
    }
}

bool
TemplateNode::restoreDetachedEntry()
{
    const ProjectPtr project = projectForDetach(getApp());
    if (!project || !_imp->detached || project->hasTemplateImport(_imp->detached->entry.key)) {
        return false;
    }
    project->setTemplateImport(_imp->detached->entry);

    return true;
}

void
TemplateNode::writeThrough()
{
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    const std::string key = getTemplateKey();
    if (!project || key.empty()) {
        return;
    }
    for (Serialization::TemplateImportSerialization entry : project->getTemplateImports()) {
        if (entry.key != key) {
            continue;
        }
        const std::string name = _imp->name.lock()->getValue();
        const std::string publishDir = _imp->publishDir.lock()->getValue();
        if ((entry.name != name) || (entry.publishDir != publishDir)) {
            entry.name = name;
            entry.publishDir = publishDir;
            _imp->refreshing = true;
            project->setTemplateImport(entry);
            _imp->refreshing = false;
        }

        return;
    }
}

bool
TemplateNode::knobChanged(KnobI* k,
                          ValueChangedReasonEnum reason,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    if (k == _imp->key.lock().get()) {
        refreshFromEntry();

        return true;
    }
    if ((k == _imp->name.lock().get()) || (k == _imp->publishDir.lock().get())) {
        if (!_imp->refreshing && (reason != eValueChangedReasonTimeChanged) && (reason != eValueChangedReasonSlaveRefresh)) {
            writeThrough();
        }

        return true;
    }
    const std::pair<KnobButtonWPtr, AppInstance::TemplateNodeActionEnum> actions[] = {
        { _imp->publish, AppInstance::eTemplateNodeActionPublish },
        { _imp->updateToLatest, AppInstance::eTemplateNodeActionUpdateToLatest },
        { _imp->updateFromFile, AppInstance::eTemplateNodeActionUpdateFromFile },
        { _imp->compareWithBase, AppInstance::eTemplateNodeActionCompareWithBase },
        { _imp->detach, AppInstance::eTemplateNodeActionDetach },
    };
    for (const std::pair<KnobButtonWPtr, AppInstance::TemplateNodeActionEnum>& action : actions) {
        if (k == action.first.lock().get()) {
            if (AppInstancePtr app = getApp()) {
                app->runTemplateNodeAction(getNode(), getTemplateKey(), action.second);
            }

            return true;
        }
    }

    return false;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_TemplateNode.cpp"
