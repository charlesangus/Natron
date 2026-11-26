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

#ifndef NATRON_GUI_KNOBEDITPROPERTIES_H
#define NATRON_GUI_KNOBEDITPROPERTIES_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPointer>
#include <QString>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"
#include "Engine/UserKnobEdit.h"
#include "Gui/GuiFwd.h"

class QComboBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QToolButton;

NATRON_NAMESPACE_ENTER

/**
 * @brief The collapsible section below a panel's pages in edit mode, holding the properties of
 * the selected user knob. A field is applied when its editing finishes, as one undo step; a
 * value the knob cannot take is shown as an error under the fields and nothing changes.
 **/
class KnobEditProperties
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON
    Q_PROPERTY(QString error READ getError)
    Q_PROPERTY(bool expanded READ isExpanded WRITE setExpanded)

public:
    explicit KnobEditProperties(KnobEditMode* mode, QWidget* parent);

    virtual ~KnobEditProperties() OVERRIDE;

    /**
     * @brief Shows the properties of knob, or nothing for a null knob. Fields being edited for
     * the same knob are kept.
     **/
    void setKnob(const KnobIPtr& knob);

    KnobIPtr getKnob() const;

    /**
     * @brief Applies every field whose value differs from the knob's.
     **/
    void commitPending();

    void focusNameField();

    QString getError() const;

    bool isExpanded() const;

    void setExpanded(bool expanded);

private Q_SLOTS:

    void onFieldEdited();

    void onTypeChanged(int index);

    void onDimensionsChanged(int index);

    void onToggleExpanded();

private:
    virtual bool eventFilter(QObject* watched, QEvent* event) OVERRIDE FINAL;

    struct Field {
        QPointer<QWidget> widget;
        // Writes the field's value into a spec of the knob.
        std::function<void(UserKnobSpec*)> write;
    };

    void scheduleRebuild();

    void rebuild();

    void clearForm();

    void addField(const QString& label, QWidget* widget, const std::function<void(UserKnobSpec*)>& write);

    void registerField(QWidget* widget, const std::function<void(UserKnobSpec*)>& write);

    void addCommonFields(const UserKnobSpec& spec);

    void addTypeFields(const UserKnobSpec& spec);

    bool applyField(const Field& field);

    void changeType(const std::string& typeName, int dimension, bool isLabel);

    void setError(const QString& error);

    QString title() const;

    KnobEditMode* _mode;
    KnobIWPtr _knob;
    QToolButton* _header;
    QPushButton* _deleteButton;
    QWidget* _body;
    QPointer<QWidget> _form;
    QFormLayout* _formLayout;
    QLabel* _errorLabel;
    QPointer<QWidget> _nameField;
    QPointer<QComboBox> _typeCombo;
    QPointer<QComboBox> _dimensionCombo;
    std::vector<Field> _fields;
    QString _error;
    bool _applying;
    bool _rebuildPending;
    bool _focusNamePending;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBEDITPROPERTIES_H
