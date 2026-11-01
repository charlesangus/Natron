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

#include "AddPathVariableDialog.h"

#include <map>
#include <memory>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/Project.h"
#include "Engine/ProjectPathVariableCommands.h"

#include "Gui/DialogButtonBox.h"
#include "Gui/LineEdit.h"
#include "Gui/PathVariableUndo.h"
#include "Gui/SequenceFileDialog.h"

NATRON_NAMESPACE_ENTER

AddPathVariableDialog::AddPathVariableDialog(Gui* gui,
                                             const ProjectPtr& project,
                                             const QString& name,
                                             const QString& value,
                                             QWidget* parent)
    : QDialog(parent)
    , _gui(gui)
    , _project(project)
    , _nameEdit(0)
    , _valueEdit(0)
    , _browseButton(0)
    , _insertCombo(0)
    , _previewLabel(0)
    , _statusLabel(0)
    , _useExistingButton(0)
    , _allowDuplicate(0)
    , _buttons(0)
    , _created(false)
    , _fileKnob()
    , _fileName()
{
    setObjectName(QString::fromUtf8("addPathVariableDialog"));
    setWindowTitle(tr("Add Path Variable"));

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    QFormLayout* form = new QFormLayout;
    mainLayout->addLayout(form);

    _nameEdit = new LineEdit(this);
    _nameEdit->setObjectName(QString::fromUtf8("pathVariableName"));
    _nameEdit->setToolTip(tr("Used in file paths as [name]. It must be a valid Python identifier, since the "
                             "variable is also app.<name> and rfv(\"name\")."));
    _nameEdit->setText(name);
    form->addRow(tr("Name"), _nameEdit);

    QWidget* valueRow = new QWidget(this);
    QHBoxLayout* valueLayout = new QHBoxLayout(valueRow);
    valueLayout->setContentsMargins(0, 0, 0, 0);
    _valueEdit = new LineEdit(valueRow);
    _valueEdit->setObjectName(QString::fromUtf8("pathVariableValue"));
    _valueEdit->setToolTip(tr("The folder. It may use other project paths, such as [%1]/shots.")
                               .arg(QString::fromUtf8(NATRON_PROJECT_ENV_VAR_NAME)));
    _valueEdit->setText(value);
    _valueEdit->setMinimumWidth(360);
    valueLayout->addWidget(_valueEdit);

    _insertCombo = new QComboBox(valueRow);
    _insertCombo->setObjectName(QString::fromUtf8("pathVariableInsert"));
    _insertCombo->setToolTip(tr("Insert a project path at the cursor."));
    _insertCombo->addItem(tr("Insert..."));
    std::map<std::string, std::string> env;
    project->getEnvironmentVariables(env);
    for (std::map<std::string, std::string>::const_iterator it = env.begin(); it != env.end(); ++it) {
        _insertCombo->addItem(QString::fromUtf8(('[' + it->first + ']').c_str()));
    }
    valueLayout->addWidget(_insertCombo);

    _browseButton = new QPushButton(tr("Browse..."), valueRow);
    _browseButton->setObjectName(QString::fromUtf8("pathVariableBrowse"));
    _browseButton->setToolTip(tr("Pick the folder. It is spelled through the project path that already holds most of it."));
    valueLayout->addWidget(_browseButton);
    form->addRow(tr("Value"), valueRow);

    _previewLabel = new QLabel(this);
    _previewLabel->setObjectName(QString::fromUtf8("pathVariablePreview"));
    _previewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Expands To"), _previewLabel);

    _statusLabel = new QLabel(this);
    _statusLabel->setObjectName(QString::fromUtf8("pathVariableStatus"));
    _statusLabel->setWordWrap(true);
    mainLayout->addWidget(_statusLabel);

    QWidget* duplicateRow = new QWidget(this);
    QHBoxLayout* duplicateLayout = new QHBoxLayout(duplicateRow);
    duplicateLayout->setContentsMargins(0, 0, 0, 0);
    _useExistingButton = new QPushButton(duplicateRow);
    _useExistingButton->setObjectName(QString::fromUtf8("pathVariableUseExisting"));
    duplicateLayout->addWidget(_useExistingButton);
    _allowDuplicate = new QCheckBox(tr("Create Anyway"), duplicateRow);
    _allowDuplicate->setObjectName(QString::fromUtf8("pathVariableAllowDuplicate"));
    _allowDuplicate->setToolTip(tr("Create a second project path for the same folder."));
    duplicateLayout->addWidget(_allowDuplicate);
    duplicateLayout->addStretch();
    mainLayout->addWidget(duplicateRow);

    _buttons = new DialogButtonBox(QDialogButtonBox::StandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel), Qt::Horizontal, this);
    mainLayout->addWidget(_buttons);

    QObject::connect(_nameEdit, SIGNAL(textChanged(QString)), this, SLOT(refresh()));
    QObject::connect(_valueEdit, SIGNAL(textChanged(QString)), this, SLOT(refresh()));
    QObject::connect(_allowDuplicate, SIGNAL(toggled(bool)), this, SLOT(refresh()));
    QObject::connect(_insertCombo, SIGNAL(activated(int)), this, SLOT(onInsertActivated(int)));
    QObject::connect(_browseButton, SIGNAL(clicked()), this, SLOT(onBrowseClicked()));
    QObject::connect(_useExistingButton, SIGNAL(clicked()), this, SLOT(onUseExistingClicked()));
    QObject::connect(_buttons, SIGNAL(accepted()), this, SLOT(accept()));
    QObject::connect(_buttons, SIGNAL(rejected()), this, SLOT(reject()));

    refresh();
}

AddPathVariableDialog::~AddPathVariableDialog()
{
}

void
AddPathVariableDialog::refresh()
{
    ProjectPtr project = _project.lock();
    if (!project) {
        return;
    }
    const std::string name = _nameEdit->text().trimmed().toStdString();
    const std::string value = _valueEdit->text().trimmed().toStdString();

    const std::string preview = project->expandPathPreview(value);
    const bool unresolved = preview.find('[') != std::string::npos;
    QString previewText = QString::fromUtf8(preview.c_str());
    if (!preview.empty() && !unresolved && !QFileInfo(previewText).isDir()) {
        previewText += tr(" (no such folder yet)");
    }
    _previewLabel->setText(previewText);

    std::string nameError;
    const bool nameOk = project->canAddPathVariable(name, &nameError);
    _duplicateOf = unresolved ? QString() : QString::fromUtf8(project->findPathVariableWithTarget(value).c_str());

    QString status;
    if (value.empty()) {
        status = tr("Enter the folder, or pick it with Browse.");
    } else if (unresolved) {
        status = tr("The value uses a project path that does not exist.");
    } else if (!nameOk) {
        status = QString::fromUtf8(nameError.c_str());
    }
    if (!_duplicateOf.isEmpty()) {
        const QString duplicate = tr("[%1] already names this folder.").arg(_duplicateOf);
        status = status.isEmpty() ? duplicate : (duplicate + QLatin1Char(' ') + status);
    }
    _statusLabel->setText(status);

    const bool hasDuplicate = !_duplicateOf.isEmpty();
    _useExistingButton->setText(tr("Use [%1]").arg(_duplicateOf));
    _useExistingButton->setVisible(hasDuplicate);
    _allowDuplicate->setVisible(hasDuplicate);

    const bool ok = nameOk && !value.empty() && !unresolved && (!hasDuplicate || _allowDuplicate->isChecked());
    QPushButton* okButton = _buttons->button(QDialogButtonBox::Ok);
    if (okButton) {
        okButton->setEnabled(ok);
    }
}

void
AddPathVariableDialog::onInsertActivated(int index)
{
    if (index <= 0) {
        return;
    }
    _valueEdit->insert(_insertCombo->itemText(index));
    _insertCombo->setCurrentIndex(0);
    _valueEdit->setFocus();
}

void
AddPathVariableDialog::onBrowseClicked()
{
    ProjectPtr project = _project.lock();
    if (!project) {
        return;
    }
    const std::string start = project->expandPathPreview(_valueEdit->text().trimmed().toStdString());
    std::vector<std::string> filters;
    SequenceFileDialog dialog(this, filters, false, SequenceFileDialog::eFileDialogModeDir, start, _gui, false);
    if (!dialog.exec()) {
        return;
    }
    std::string dir = dialog.selectedDirectory();
    while ((dir.size() > 1) && (dir[dir.size() - 1] == '/')) {
        dir.erase(dir.size() - 1, 1);
    }
    _valueEdit->setText(QString::fromUtf8(spellThroughProjectPaths(*project, dir).c_str()));
    if (_nameEdit->text().trimmed().isEmpty()) {
        _nameEdit->setText(QString::fromUtf8(availableName(*project, QFileInfo(QString::fromUtf8(dir.c_str())).fileName().toStdString()).c_str()));
    }
}

void
AddPathVariableDialog::onUseExistingClicked()
{
    if (_duplicateOf.isEmpty()) {
        return;
    }
    _resultName = _duplicateOf;
    _created = false;
    QDialog::accept();
}

void
AddPathVariableDialog::accept()
{
    ProjectPtr project = _project.lock();
    if (!project) {
        return;
    }
    const std::string name = _nameEdit->text().trimmed().toStdString();
    const std::string value = _valueEdit->text().trimmed().toStdString();
    std::string error;
    if (!project->canAddPathVariable(name, &error)) {
        _statusLabel->setText(QString::fromUtf8(error.c_str()));

        return;
    }
    std::shared_ptr<AddPathVariableCommand> command = std::make_shared<AddPathVariableCommand>(project, name, value);
    KnobStringBasePtr fileKnob = std::dynamic_pointer_cast<KnobStringBase>(_fileKnob.lock());
    if (fileKnob) {
        command->addValueEdit(fileKnob, fileKnob->getValue(), filePathUnder(name, _fileName));
    }
    error = pushPathVariableCommand(_gui, project, command);
    if (!error.empty()) {
        _statusLabel->setText(QString::fromUtf8(error.c_str()));

        return;
    }
    _resultName = QString::fromUtf8(name.c_str());
    _created = true;
    QDialog::accept();
}

std::string
AddPathVariableDialog::spellThroughProjectPaths(const Project& project,
                                                const std::string& absolutePath)
{
    std::map<std::string, std::string> env;
    project.getEnvironmentVariables(env);
    std::string bestName;
    std::string bestTarget;
    for (std::map<std::string, std::string>::const_iterator it = env.begin(); it != env.end(); ++it) {
        if (it->second.empty()) {
            continue;
        }
        const std::string target = project.expandPathPreview(it->second);
        if (target.empty() || (target.size() <= bestTarget.size())) {
            continue;
        }
        const bool same = absolutePath == target;
        const bool inside = (absolutePath.size() > target.size()) && (absolutePath.compare(0, target.size(), target) == 0) && ((absolutePath[target.size()] == '/') || (target[target.size() - 1] == '/'));
        if (same || inside) {
            bestName = it->first;
            bestTarget = target;
        }
    }
    if (bestName.empty()) {
        return absolutePath;
    }
    std::string rest = absolutePath.substr(bestTarget.size());
    if (!rest.empty() && (rest[0] != '/')) {
        rest = '/' + rest;
    }

    return '[' + bestName + ']' + rest;
}

std::string
AddPathVariableDialog::availableName(const Project& project,
                                     const std::string& base)
{
    std::string stem = NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(base);
    if (stem.empty()) {
        stem = "path";
    }
    if (project.canAddPathVariable(stem)) {
        return stem;
    }
    for (int i = 1; i < 10000; ++i) {
        const std::string candidate = stem + std::to_string(i);
        if (project.canAddPathVariable(candidate)) {
            return candidate;
        }
    }

    return std::string();
}

std::string
AddPathVariableDialog::filePathUnder(const std::string& name,
                                     const std::string& fileName)
{
    std::string ret = '[' + name + ']';
    if (!fileName.empty()) {
        ret += '/' + fileName;
    }

    return ret;
}

bool
AddPathVariableDialog::runForFilePath(Gui* gui,
                                      const ProjectPtr& project,
                                      const KnobIPtr& fileKnob,
                                      QWidget* parent,
                                      std::string* newFilePath,
                                      bool* created)
{
    KnobStringBase* isString = dynamic_cast<KnobStringBase*>(fileKnob.get());
    if (!project || !isString) {
        return false;
    }
    const std::string filePath = isString->getValue();
    const std::string expanded = project->expandPathPreview(filePath);
    std::string folder;
    if (!expanded.empty()) {
        folder = QFileInfo(QString::fromUtf8(expanded.c_str())).path().toStdString();
    }
    const std::string value = folder.empty() ? std::string() : spellThroughProjectPaths(*project, folder);
    const std::string name = folder.empty() ? std::string() : availableName(*project, QFileInfo(QString::fromUtf8(folder.c_str())).fileName().toStdString());

    AddPathVariableDialog dialog(gui, project, QString::fromUtf8(name.c_str()), QString::fromUtf8(value.c_str()), parent);
    // The file part is kept as typed, so a frame pattern or a view token survives unexpanded.
    const std::size_t slash = filePath.find_last_of('/');
    dialog._fileKnob = fileKnob;
    dialog._fileName = (slash == std::string::npos) ? filePath : filePath.substr(slash + 1);
    if (!dialog.exec()) {
        return false;
    }
    *newFilePath = filePathUnder(dialog.getVariableName().toStdString(), dialog._fileName);
    *created = dialog.hasCreatedVariable();

    return true;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_AddPathVariableDialog.cpp"
