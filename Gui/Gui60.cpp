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

#include "Engine/Project.h"
#include "Engine/ProjectMerge.h"

#include "Gui/GuiAppInstance.h"
#include "Gui/GuiPrivate.h"
#include "Gui/Menu.h"
#include "Gui/MergeConflictResolve.h"

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

QString
Gui::templateBaseDescription(const ProjectPtr& project,
                             const std::string& templateKey)
{
    const std::optional<Serialization::TemplateImportSerialization> entry = findImport(project, templateKey);
    const QString name = (entry && !entry->path.empty()) ? QFileInfo(QString::fromStdString(entry->path)).fileName() : QString::fromStdString(templateKey);

    return tr("the base of template %1").arg(name);
}

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

int
Gui::updateFromTemplate(const std::string& templateKey)
{
    const ProjectPtr project = getApp()->getProject();
    const std::optional<Serialization::TemplateImportSerialization> entry = findImport(project, templateKey);
    if (!entry) {
        errorDialog(tr("Update from Template").toStdString(),
                    tr("The project has no template import %1.").arg(QString::fromStdString(templateKey)).toStdString(), false);

        return -1;
    }
    QString path;
    const QString stored = QString::fromStdString(entry->path);
    if (stored.isEmpty() || !QFileInfo::exists(stored)) {
        const std::string dir = stored.isEmpty() ? _imp->_lastLoadProjectOpenedDir.toStdString() : QFileInfo(stored).absolutePath().toStdString();
        const std::string chosen = popOpenFileDialog(false, projectFileFilters(), dir, false);
        if (chosen.empty()) {
            return -1;
        }
        path = QString::fromStdString(chosen);
    }
    const ProjectMerge::UpdateResult result = MergeConflictResolve::updateFromTemplate(this, templateKey, path);
    if (!result.ok) {
        errorDialog(tr("Update from Template").toStdString(), result.error.toStdString(), false);

        return -1;
    }

    return (int)result.conflicts.size();
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
    setGraphComparison(summary, QFileInfo(QString::fromStdString(path)).fileName());
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
    setGraphComparison(summary, templateBaseDescription(getApp()->getProject(), templateKey));
}

const ProjectMerge::ChangeSummary*
Gui::getGraphComparison() const
{
    return _imp->graphComparison.get();
}

void
Gui::setGraphComparison(const ProjectMerge::ChangeSummary& summary,
                        const QString& description)
{
    _imp->graphComparison = std::make_shared<ProjectMerge::ChangeSummary>(summary);
    _imp->graphComparisonDescription = description;
    Q_EMIT graphComparisonChanged();
}

void
Gui::clearGraphComparison()
{
    if (!_imp->graphComparison) {
        return;
    }
    _imp->graphComparison.reset();
    _imp->graphComparisonDescription.clear();
    Q_EMIT graphComparisonChanged();
}

QString
Gui::getGraphComparisonDescription() const
{
    return _imp->graphComparisonDescription;
}

void
Gui::recompareGraphComparison(const std::shared_ptr<const Serialization::ProjectSerialization>& other)
{
    if (!_imp->graphComparison || !other || (_imp->graphComparison->other != other)) {
        return;
    }
    setGraphComparison(ProjectMerge::recompare(getApp()->getProject(), *_imp->graphComparison), _imp->graphComparisonDescription);
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
            QAction* action = menu->addAction(path.isEmpty() ? key : tr("%1 (%2)").arg(key, QFileInfo(path).fileName()));
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
        updateFromTemplate(action->data().toString().toStdString());
    }
}

NATRON_NAMESPACE_EXIT
