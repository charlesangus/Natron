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

#include "Gui.h"

#include <list>
#include <optional>
#include <string>
#include <vector>

#include <QAction>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>

#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ProjectMerge.h"
#include "Engine/ProjectTemplate.h"

#include "Gui/GuiAppInstance.h"
#include "Gui/GuiPrivate.h"
#include "Gui/Menu.h"
#include "Gui/MergeConflictResolve.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"

#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {
std::vector<std::string>
projectFileFilters()
{
    std::vector<std::string> filters;
    filters.push_back(NATRON_PROJECT_FILE_EXT);
    filters.push_back(Project::folderProjectFileName().toStdString());

    return filters;
}

std::optional<Serialization::TemplateImportSerialization>
findImport(const ProjectPtr& project,
           const std::string& key)
{
    for (const Serialization::TemplateImportSerialization& entry : project->getTemplateImports()) {
        if (entry.key == key) {
            return entry;
        }
    }

    return std::nullopt;
}
} // anon namespace

void
Gui::importTemplate()
{
    const std::string path = popOpenFileDialog(false, projectFileFilters(), _imp->_lastLoadProjectOpenedDir.toStdString(), false);
    if (path.empty()) {
        return;
    }
    QString error;
    const std::string key = MergeConflictResolve::importTemplate(this, QString::fromStdString(path), &error);
    if (key.empty()) {
        errorDialog(tr("Import Template").toStdString(), error.toStdString(), false);
    } else if (!error.isEmpty()) {
        warningDialog(tr("Import Template").toStdString(), tr("Only part of the template could be imported: %1").arg(error).toStdString(), false);
    }
}

void
Gui::defineTemplate()
{
    const ProjectPtr project = getApp()->getProject();
    NodeGraph* graph = getLastSelectedGraph();
    if (!graph) {
        graph = getNodeGraph();
    }
    NodeCollectionPtr group = graph->getGroup();
    if (!group) {
        group = project;
    }
    NodesList nodes;
    for (const NodeGuiPtr& gui : graph->getSelectedNodes()) {
        if (NodePtr node = gui->getNode()) {
            nodes.push_back(node);
        }
    }
    NodePtr templateNode;
    QString error;
    MergeConflictResolve::runTemplateEdit(this, graph, tr("Define Template"), [&](TemplateEdit* edit) {
        if (!project->defineTemplate(nodes, std::string(), group, edit)) {
            error = edit->error;

            return false;
        }
        templateNode = edit->templateNode;

        return true;
    });
    if (!templateNode) {
        getApp()->showStatusMessage(tr("Define Template: %1").arg(error));

        return;
    }
    NodeGuiPtr templateGui = std::dynamic_pointer_cast<NodeGui>(templateNode->getNodeGui());
    if (templateGui) {
        graph->setSelection(NodesGuiList(1, templateGui));
        templateGui->setVisibleSettingsPanel(true);
    }
}

void
Gui::publishTemplate(const std::string& templateKey)
{
    QString path;
    QString error;
    MergeConflictResolve::runTemplateEdit(this, 0, tr("Publish Template"), [&](TemplateEdit* edit) {
        if (!getApp()->getProject()->publishTemplate(templateKey, &error, edit)) {
            return false;
        }
        path = edit->path;

        return true;
    });
    if (path.isEmpty()) {
        getApp()->showStatusMessage(tr("Publish Template: %1").arg(error));
    } else {
        getApp()->showStatusMessage(tr("Published %1").arg(path));
    }
}

void
Gui::updateTemplateToLatest(const std::string& templateKey)
{
    const std::optional<Serialization::TemplateImportSerialization> entry = findImport(getApp()->getProject(), templateKey);
    if (!entry) {
        getApp()->showStatusMessage(tr("The project has no template import %1").arg(QString::fromStdString(templateKey)));

        return;
    }
    const QString dir = QString::fromStdString(entry->publishDir);
    const std::optional<ProjectMerge::TemplateVersion> latest = ProjectMerge::findLatestVersion(dir, entry->templateId);
    if (!latest) {
        getApp()->showStatusMessage(tr("No version of the template %1 is published in \"%2\"").arg(QString::fromStdString(entry->name)).arg(dir));

        return;
    }
    updateTemplateFromPath(templateKey, latest->path);
}

void
Gui::updateTemplateFromFile(const std::string& templateKey)
{
    const std::optional<Serialization::TemplateImportSerialization> entry = findImport(getApp()->getProject(), templateKey);
    if (!entry) {
        getApp()->showStatusMessage(tr("The project has no template import %1").arg(QString::fromStdString(templateKey)));

        return;
    }
    const std::string dir = entry->publishDir.empty() ? _imp->_lastLoadProjectOpenedDir.toStdString() : entry->publishDir;
    const std::string chosen = popOpenFileDialog(false, projectFileFilters(), dir, false);
    if (chosen.empty()) {
        return;
    }
    updateTemplateFromPath(templateKey, QString::fromStdString(chosen));
}

void
Gui::updateTemplateFromPath(const std::string& templateKey,
                            const QString& path)
{
    const QString title = tr("Update from Template");
    ProjectMerge::UpdateResult result = MergeConflictResolve::updateFromTemplate(this, templateKey, path, false);
    if (result.status == ProjectMerge::UpdateStatus::eTemplateMismatch) {
        QMessageBox box(QMessageBox::Warning, title, result.error, QMessageBox::Cancel, this);
        box.setObjectName(QString::fromUtf8("templateMismatchDialog"));
        QPushButton* mergeAnyway = box.addButton(tr("Merge Anyway"), QMessageBox::AcceptRole);
        mergeAnyway->setObjectName(QString::fromUtf8("mergeAnywayButton"));
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != mergeAnyway) {
            return;
        }
        result = MergeConflictResolve::updateFromTemplate(this, templateKey, path, true);
    }
    const QString name = QFileInfo(path).fileName();
    switch (result.status) {
    case ProjectMerge::UpdateStatus::eUpdated:
        if (result.conflicts.empty()) {
            getApp()->showStatusMessage(tr("Updated to %1").arg(name));
        } else {
            getApp()->showStatusMessage(tr("Updated to %1 with %n conflict(s)", "", (int)result.conflicts.size()).arg(name));
        }
        break;
    case ProjectMerge::UpdateStatus::eUpToDate:
        getApp()->showStatusMessage(tr("The template is up to date with %1").arg(name));
        break;
    case ProjectMerge::UpdateStatus::eTemplateMismatch:
    case ProjectMerge::UpdateStatus::eLowMatch:
        getApp()->showStatusMessage(result.error);
        break;
    case ProjectMerge::UpdateStatus::eError:
        errorDialog(title.toStdString(), result.error.toStdString(), false);
        break;
    }
}

void
Gui::addToTemplate(const std::string& templateKey,
                   const NodesList& nodes)
{
    QString error;
    MergeConflictResolve::runTemplateEdit(this, 0, tr("Add to Template"), [&](TemplateEdit* edit) {
        bool ok = true;
        for (const NodePtr& node : nodes) {
            if (!getApp()->getProject()->addToTemplate(templateKey, node, edit) && ok) {
                ok = false;
                error = edit->error;
            }
        }

        return ok;
    });
    if (!error.isEmpty()) {
        getApp()->showStatusMessage(tr("Add to Template: %1").arg(error));
    }
}

void
Gui::removeFromTemplate(const std::string& templateKey,
                        const NodesList& nodes)
{
    QString error;
    MergeConflictResolve::runTemplateEdit(this, 0, tr("Remove from Template"), [&](TemplateEdit* edit) {
        bool ok = true;
        for (const NodePtr& node : nodes) {
            if (!getApp()->getProject()->removeFromTemplate(templateKey, node, edit) && ok) {
                ok = false;
                error = edit->error;
            }
        }

        return ok;
    });
    if (!error.isEmpty()) {
        getApp()->showStatusMessage(tr("Remove from Template: %1").arg(error));
    }
}

void
Gui::detachTemplate(const std::string& templateKey)
{
    QString error;
    const bool ok = MergeConflictResolve::runTemplateEdit(this, 0, tr("Detach Template"), [&](TemplateEdit* edit) {
        const bool detached = getApp()->getProject()->detachTemplate(templateKey, edit);
        error = edit->error;

        return detached;
    });
    if (!ok) {
        getApp()->showStatusMessage(tr("Detach Template: %1").arg(error));
    }
}

void
Gui::compareWithFile()
{
    const std::string path = popOpenFileDialog(false, projectFileFilters(), _imp->_lastLoadProjectOpenedDir.toStdString(), false);
    if (path.empty()) {
        return;
    }
    ProjectMerge::ChangeSummary summary;
    QString error;
    if (!ProjectMerge::compareWithFile(getApp()->getProject(), QString::fromStdString(path), &summary, &error)) {
        errorDialog(tr("Compare with File").toStdString(), error.toStdString(), false);

        return;
    }
    setGraphComparison(summary);
}

void
Gui::compareWithTemplateBase(const std::string& templateKey)
{
    ProjectMerge::ChangeSummary summary;
    QString error;
    if (!ProjectMerge::compareWithTemplateBase(getApp()->getProject(), templateKey, &summary, &error)) {
        errorDialog(tr("Compare with Template Base").toStdString(), error.toStdString(), false);

        return;
    }
    setGraphComparison(summary);
}

const ProjectMerge::ChangeSummary*
Gui::getGraphComparison() const
{
    return _imp->graphComparison.get();
}

void
Gui::setGraphComparison(const ProjectMerge::ChangeSummary& summary)
{
    _imp->graphComparison = std::make_shared<ProjectMerge::ChangeSummary>(summary);
    Q_EMIT graphComparisonChanged();
}

void
Gui::clearGraphComparison()
{
    if (!_imp->graphComparison) {
        return;
    }
    _imp->graphComparison.reset();
    Q_EMIT graphComparisonChanged();
}

void
Gui::onUpdateFromTemplateMenuAboutToShow()
{
    Menu* menu = _imp->menuUpdateFromTemplate;
    if (!menu) {
        return;
    }
    menu->clear();
    const GuiAppInstancePtr app = getApp();
    const ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (project) {
        for (const Serialization::TemplateImportSerialization& entry : project->getTemplateImports()) {
            const QString key = QString::fromStdString(entry.key);
            const QString path = QString::fromStdString(entry.path);
            QAction* action = menu->addAction(tr("%1 (v%2)").arg(key).arg(entry.version, 3, 10, QLatin1Char('0')));
            action->setData(key);
            action->setToolTip(path);
            QObject::connect(action, SIGNAL(triggered()), this, SLOT(onUpdateFromTemplateActionTriggered()));
        }
    }
    if (menu->isEmpty()) {
        QAction* none = menu->addAction(tr("No templates imported"));
        none->setEnabled(false);
    }
}

void
Gui::onUpdateFromTemplateActionTriggered()
{
    QAction* action = qobject_cast<QAction*>(sender());
    if (action) {
        updateTemplateToLatest(action->data().toString().toStdString());
    }
}

NATRON_NAMESPACE_EXIT
