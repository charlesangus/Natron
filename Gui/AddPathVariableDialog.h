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

#ifndef Gui_AddPathVariableDialog_h
#define Gui_AddPathVariableDialog_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include <QDialog>
#include <QString>

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

NATRON_NAMESPACE_ENTER

/**
 * @brief Creates a project path: a string variable of the project (see Project::addPathVariable())
 * whose value may use other project paths. Shows the value fully expanded as it is typed, refuses
 * a name the project cannot take, and when another project path already names the same folder
 * offers to use that one instead, creating a second one only when asked to.
 **/
class AddPathVariableDialog
    : public QDialog {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    AddPathVariableDialog(Gui* gui,
                          const ProjectPtr& project,
                          const QString& name,
                          const QString& value,
                          QWidget* parent);

    virtual ~AddPathVariableDialog() OVERRIDE;

    /**
     * @brief Once accepted, the project path to use: the one created, or the existing one chosen
     * instead.
     **/
    QString getVariableName() const
    {
        return _resultName;
    }

    /**
     * @brief Once accepted, whether getVariableName() was created by the dialog.
     **/
    bool hasCreatedVariable() const
    {
        return _created;
    }

    /**
     * @brief Runs the dialog for a file path: it starts with the folder of filePath, spelled
     * through the project path naming the longest part of it, and a name made from the folder's.
     * @returns true with newFilePath set to filePath's file name under the accepted project path,
     * or false when the dialog was cancelled.
     **/
    static bool runForFilePath(Gui* gui,
                               const ProjectPtr& project,
                               const std::string& filePath,
                               QWidget* parent,
                               std::string* newFilePath);

    /**
     * @brief absolutePath spelled through the project path whose expanded folder is its longest
     * leading folder, or absolutePath unchanged when no project path contains it.
     **/
    static std::string spellThroughProjectPaths(const Project& project, const std::string& absolutePath);

    /**
     * @brief A name the project can take for a project path (see Project::canAddPathVariable()),
     * made from base, with a number appended when needed.
     **/
    static std::string availableName(const Project& project, const std::string& base);

public Q_SLOTS:

    void refresh();

    void onInsertActivated(int index);

    void onBrowseClicked();

    void onUseExistingClicked();

    virtual void accept() OVERRIDE;

private:
    Gui* _gui;
    ProjectWPtr _project;
    LineEdit* _nameEdit;
    LineEdit* _valueEdit;
    QPushButton* _browseButton;
    QComboBox* _insertCombo;
    QLabel* _previewLabel;
    QLabel* _statusLabel;
    QPushButton* _useExistingButton;
    QCheckBox* _allowDuplicate;
    DialogButtonBox* _buttons;
    QString _duplicateOf;
    QString _resultName;
    bool _created;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_AddPathVariableDialog_h
