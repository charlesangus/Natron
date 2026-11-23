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

#include "KnobEditProperties.h"

#include <algorithm>
#include <cfloat>
#include <climits>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleValidator>
#include <QEvent>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/ProjectPathVariableCommands.h"
#include "Engine/UserKnobEdit.h"

#include "Gui/DockablePanel.h"
#include "Gui/KnobEditMode.h"
#include "Gui/KnobEditPalette.h"
#include "Gui/PathVariableUndo.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
isPageOrGroup(const std::string& typeName)
{
    return (typeName == KnobPage::typeNameStatic()) || (typeName == KnobGroup::typeNameStatic());
}

bool
isNumeric(const std::string& typeName)
{
    return (typeName == KnobInt::typeNameStatic()) || (typeName == KnobDouble::typeNameStatic()) || (typeName == KnobColor::typeNameStatic());
}

bool
specIsLabel(const UserKnobSpec& spec)
{
    const Serialization::KnobTextExtraSerialization* text = std::get_if<Serialization::KnobTextExtraSerialization>(&spec.extra);

    return text && text->isLabel;
}

// The dimensions a user knob of typeName can have, empty when it has a single one.
std::vector<int>
dimensionChoices(const std::string& typeName)
{
    if ((typeName == KnobInt::typeNameStatic()) || (typeName == KnobDouble::typeNameStatic())) {
        return std::vector<int> { 1, 2, 3 };
    }
    if (typeName == KnobColor::typeNameStatic()) {
        return std::vector<int> { 3, 4 };
    }
    if (typeName == KnobParametric::typeNameStatic()) {
        return std::vector<int> { 1, 2, 3, 4, 5, 6, 7, 8 };
    }

    return std::vector<int>();
}

QString
numberText(double value)
{
    return QString::number(value, 'g', 12);
}

QStringList
linesOf(const QPlainTextEdit* edit)
{
    return edit->toPlainText().split(QLatin1Char('\n'));
}

QString
joinLines(const std::vector<std::string>& lines)
{
    QStringList ret;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        ret.push_back(QString::fromUtf8(lines[i].c_str()));
    }

    return ret.join(QLatin1Char('\n'));
}

QLineEdit*
numberEdit(double value,
           bool isInt,
           QWidget* parent)
{
    QLineEdit* edit = new QLineEdit(parent);
    if (isInt) {
        edit->setValidator(new QIntValidator(INT_MIN, INT_MAX, edit));
        edit->setText(QString::number((int)value));
    } else {
        QDoubleValidator* validator = new QDoubleValidator(edit);
        validator->setNotation(QDoubleValidator::ScientificNotation);
        edit->setValidator(validator);
        edit->setText(numberText(value));
    }

    return edit;
}

double
numberOf(const QLineEdit* edit,
         double fallback)
{
    bool ok = false;
    const double value = edit->text().toDouble(&ok);

    return ok ? value : fallback;
}
} // anon namespace

KnobEditProperties::KnobEditProperties(KnobEditMode* mode,
                                       QWidget* parent)
    : QWidget(parent)
    , _mode(mode)
    , _knob()
    , _header(0)
    , _body(0)
    , _form()
    , _formLayout(0)
    , _errorLabel(0)
    , _nameField()
    , _typeCombo()
    , _dimensionCombo()
    , _fields()
    , _error()
    , _applying(false)
    , _rebuildPending(false)
    , _focusNamePending(false)
{
    setObjectName(QString::fromUtf8("KnobEditProperties"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(2);
    _header = new QToolButton(this);
    _header->setObjectName(QString::fromUtf8("KnobEditPropertiesHeader"));
    _header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _header->setArrowType(Qt::DownArrow);
    _header->setAutoRaise(true);
    _header->setFocusPolicy(Qt::NoFocus);
    _header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _header->setToolTip(tr("Show or hide the properties of the selected parameter."));
    QObject::connect(_header, SIGNAL(clicked()), this, SLOT(onToggleExpanded()));
    layout->addWidget(_header);
    _body = new QWidget(this);
    QVBoxLayout* bodyLayout = new QVBoxLayout(_body);
    bodyLayout->setContentsMargins(8, 0, 8, 4);
    bodyLayout->setSpacing(2);
    _errorLabel = new QLabel(_body);
    _errorLabel->setObjectName(QString::fromUtf8("KnobEditPropertiesError"));
    _errorLabel->setWordWrap(true);
    _errorLabel->setStyleSheet(QString::fromUtf8("color: rgb(230, 70, 70);"));
    _errorLabel->hide();
    bodyLayout->addWidget(_errorLabel);
    layout->addWidget(_body);
    _header->setText(title());
}

KnobEditProperties::~KnobEditProperties()
{
}

KnobIPtr
KnobEditProperties::getKnob() const
{
    return _knob.lock();
}

void
KnobEditProperties::setKnob(const KnobIPtr& knob)
{
    if (knob != _knob.lock()) {
        setError(QString());
    }
    _knob = knob;
    scheduleRebuild();
}

void
KnobEditProperties::scheduleRebuild()
{
    if (_rebuildPending) {
        return;
    }
    _rebuildPending = true;
    QTimer::singleShot(0, this, [this]() {
        _rebuildPending = false;
        rebuild();
    });
}

QString
KnobEditProperties::getError() const
{
    return _error;
}

bool
KnobEditProperties::isExpanded() const
{
    return !_body->isHidden();
}

void
KnobEditProperties::setExpanded(bool expanded)
{
    _body->setVisible(expanded);
    _header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
}

void
KnobEditProperties::onToggleExpanded()
{
    setExpanded(!isExpanded());
}

void
KnobEditProperties::focusNameField()
{
    _focusNamePending = true;
    scheduleRebuild();
}

void
KnobEditProperties::setError(const QString& error)
{
    _error = error;
    _errorLabel->setText(error);
    _errorLabel->setVisible(!error.isEmpty());
}

QString
KnobEditProperties::title() const
{
    KnobIPtr knob = _knob.lock();
    if (!knob) {
        return tr("Properties - no parameter selected");
    }
    const UserKnobSpec spec = specFromKnob(knob);
    const KnobEditTypeEntry* entry = findKnobEditTypeEntry(spec.typeName, specIsLabel(spec));
    const QString typeLabel = entry ? entry->label : QString::fromUtf8(spec.typeName.c_str());
    const std::string label = knob->getLabel().empty() ? knob->getName() : knob->getLabel();

    return tr("Properties - %1 (%2)").arg(QString::fromUtf8(label.c_str()), typeLabel);
}

void
KnobEditProperties::clearForm()
{
    for (std::size_t i = 0; i < _fields.size(); ++i) {
        if (_fields[i].widget) {
            _fields[i].widget->blockSignals(true);
            _fields[i].widget->removeEventFilter(this);
        }
    }
    _fields.clear();
    if (_typeCombo) {
        _typeCombo->blockSignals(true);
    }
    if (_dimensionCombo) {
        _dimensionCombo->blockSignals(true);
    }
    if (_form) {
        _form->hide();
        _form->deleteLater();
    }
    _form.clear();
    _formLayout = 0;
    _nameField.clear();
    _typeCombo.clear();
    _dimensionCombo.clear();
}

void
KnobEditProperties::rebuild()
{
    int focused = -1;
    for (std::size_t i = 0; i < _fields.size(); ++i) {
        if (_fields[i].widget && _fields[i].widget->hasFocus()) {
            focused = (int)i;
        }
    }
    clearForm();
    _header->setText(title());
    KnobIPtr knob = _knob.lock();
    if (!knob) {
        _focusNamePending = false;

        return;
    }
    _form = new QWidget(_body);
    _form->setObjectName(QString::fromUtf8("KnobEditPropertiesForm"));
    _formLayout = new QFormLayout(_form);
    _formLayout->setContentsMargins(0, 0, 0, 0);
    _formLayout->setSpacing(2);
    _formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    static_cast<QVBoxLayout*>(_body->layout())->insertWidget(0, _form);

    const UserKnobSpec spec = specFromKnob(knob);
    addCommonFields(spec);
    addTypeFields(spec);

    if ((focused >= 0) && (focused < (int)_fields.size()) && _fields[focused].widget) {
        _fields[focused].widget->setFocus(Qt::OtherFocusReason);
    }
    if (_focusNamePending && _nameField) {
        setExpanded(true);
        _nameField->setFocus(Qt::OtherFocusReason);
        if (QLineEdit* edit = qobject_cast<QLineEdit*>(_nameField.data())) {
            edit->selectAll();
        }
    }
    _focusNamePending = false;
} // rebuild

void
KnobEditProperties::registerField(QWidget* widget,
                                  const std::function<void(UserKnobSpec*)>& write)
{
    Field field;
    field.widget = widget;
    field.write = write;
    _fields.push_back(field);
    if (QLineEdit* edit = qobject_cast<QLineEdit*>(widget)) {
        QObject::connect(edit, SIGNAL(editingFinished()), this, SLOT(onFieldEdited()));
    } else if (QCheckBox* check = qobject_cast<QCheckBox*>(widget)) {
        QObject::connect(check, SIGNAL(toggled(bool)), this, SLOT(onFieldEdited()));
    } else if (QComboBox* combo = qobject_cast<QComboBox*>(widget)) {
        QObject::connect(combo, SIGNAL(activated(int)), this, SLOT(onFieldEdited()));
    } else if (qobject_cast<QPlainTextEdit*>(widget)) {
        widget->installEventFilter(this);
    }
}

void
KnobEditProperties::addField(const QString& label,
                             QWidget* widget,
                             const std::function<void(UserKnobSpec*)>& write)
{
    _formLayout->addRow(label, widget);
    registerField(widget, write);
}

void
KnobEditProperties::addCommonFields(const UserKnobSpec& spec)
{
    const bool isPageOrGroupKnob = isPageOrGroup(spec.typeName);
    const bool isSeparator = spec.typeName == KnobSeparator::typeNameStatic();
    const bool isButton = spec.typeName == KnobButton::typeNameStatic();
    const bool isParametric = spec.typeName == KnobParametric::typeNameStatic();
    const bool isLabel = specIsLabel(spec);
    const Serialization::KnobTypeTraits traits = Serialization::getKnobTypeTraits(spec.typeName);

    const KnobEditTypeEntry* current = findKnobEditTypeEntry(spec.typeName, isLabel);
    if (isPageOrGroupKnob || isSeparator) {
        QLabel* type = new QLabel(current ? current->label : QString::fromUtf8(spec.typeName.c_str()), _form);
        type->setObjectName(QString::fromUtf8("KnobEditPropertiesType"));
        _formLayout->addRow(tr("Type"), type);
    } else {
        QComboBox* type = new QComboBox(_form);
        type->setObjectName(QString::fromUtf8("KnobEditPropertiesType"));
        type->setToolTip(tr("Changing the type keeps the values and animation between Int, Float and Color, "
                            "and asks before resetting them otherwise."));
        const std::vector<KnobEditTypeEntry>& entries = knobEditTypeEntries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (isPageOrGroup(entries[i].typeName) || (entries[i].typeName == KnobSeparator::typeNameStatic())) {
                continue;
            }
            type->addItem(entries[i].label, entries[i].id);
            if (current && (current->id == entries[i].id)) {
                type->setCurrentIndex(type->count() - 1);
            }
        }
        QObject::connect(type, SIGNAL(activated(int)), this, SLOT(onTypeChanged(int)));
        _formLayout->addRow(tr("Type"), type);
        _typeCombo = type;
    }

    QLineEdit* name = new QLineEdit(QString::fromUtf8(spec.name.c_str()), _form);
    name->setObjectName(QString::fromUtf8("KnobEditPropertiesName"));
    name->setToolTip(tr("The name of the parameter in scripts and expressions. Renaming it updates the expressions that use it."));
    addField(tr("Script Name"), name, [name](UserKnobSpec* s) { s->name = name->text().trimmed().toStdString(); });
    _nameField = name;

    QLineEdit* label = new QLineEdit(QString::fromUtf8(spec.label.c_str()), _form);
    label->setObjectName(QString::fromUtf8("KnobEditPropertiesLabel"));
    addField(tr("Label"), label, [label](UserKnobSpec* s) { s->label = label->text().toStdString(); });

    QLineEdit* tooltip = new QLineEdit(QString::fromUtf8(spec.tooltip.c_str()), _form);
    tooltip->setObjectName(QString::fromUtf8("KnobEditPropertiesTooltip"));
    addField(tr("Tooltip"), tooltip, [tooltip](UserKnobSpec* s) { s->tooltip = tooltip->text().toStdString(); });

    // Defaults, one per dimension.
    const Serialization::KnobChoiceExtraSerialization* choice = std::get_if<Serialization::KnobChoiceExtraSerialization>(&spec.extra);
    if (traits.isChoice && choice) {
        QComboBox* def = new QComboBox(_form);
        def->setObjectName(QString::fromUtf8("KnobEditPropertiesDefault0"));
        const std::string* currentId = spec.defaults.empty() ? 0 : std::get_if<std::string>(&spec.defaults[0]);
        for (std::size_t i = 0; i < choice->entries.size(); ++i) {
            def->addItem(QString::fromUtf8(choice->entries[i].c_str()));
            if (currentId && (*currentId == choice->entries[i])) {
                def->setCurrentIndex((int)i);
            }
        }
        addField(tr("Default"), def, [def](UserKnobSpec* s) {
            if (def->currentIndex() >= 0) {
                s->defaults.assign(1, Serialization::KnobValueSerialization(def->currentText().toStdString()));
            }
        });
    } else if (traits.hasValue && !traits.isChoice && !isButton && !isParametric && ((int)spec.defaults.size() == spec.dimension) && (traits.storageType != Serialization::eKnobStorageTypeNone)) {
        QWidget* row = new QWidget(_form);
        QHBoxLayout* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(2);
        for (int i = 0; i < spec.dimension; ++i) {
            const Serialization::KnobValueSerialization& value = spec.defaults[i];
            QWidget* w = 0;
            std::function<void(UserKnobSpec*)> write;
            if (traits.storageType == Serialization::eKnobStorageTypeBool) {
                QCheckBox* check = new QCheckBox(row);
                const bool* b = std::get_if<bool>(&value);
                check->setChecked(b && *b);
                write = [check, i](UserKnobSpec* s) {
                    if (i < (int)s->defaults.size()) {
                        s->defaults[i] = check->isChecked();
                    }
                };
                w = check;
            } else if (traits.storageType == Serialization::eKnobStorageTypeString) {
                const std::string* str = std::get_if<std::string>(&value);
                QLineEdit* edit = new QLineEdit(str ? QString::fromUtf8(str->c_str()) : QString(), row);
                write = [edit, i](UserKnobSpec* s) {
                    if (i < (int)s->defaults.size()) {
                        s->defaults[i] = edit->text().toStdString();
                    }
                };
                w = edit;
            } else {
                const bool isInt = traits.storageType == Serialization::eKnobStorageTypeInt;
                double current = 0.;
                if (const int* iv = std::get_if<int>(&value)) {
                    current = *iv;
                } else if (const double* dv = std::get_if<double>(&value)) {
                    current = *dv;
                }
                QLineEdit* edit = numberEdit(current, isInt, row);
                write = [edit, i, isInt, current](UserKnobSpec* s) {
                    if (i >= (int)s->defaults.size()) {
                        return;
                    }
                    const double v = numberOf(edit, current);
                    if (isInt) {
                        s->defaults[i] = (int)v;
                    } else {
                        s->defaults[i] = v;
                    }
                };
                w = edit;
            }
            w->setObjectName(QString::fromUtf8("KnobEditPropertiesDefault%1").arg(i));
            rowLayout->addWidget(w);
            registerField(w, write);
        }
        _formLayout->addRow(tr("Default"), row);
    }

    if (std::holds_alternative<Serialization::KnobValueRangeSerialization>(spec.extra)) {
        const Serialization::KnobValueRangeSerialization& range = std::get<Serialization::KnobValueRangeSerialization>(spec.extra);
        const bool isInt = spec.typeName == KnobInt::typeNameStatic();
        const auto addPair = [&](const QString& label, const QString& objectName, double first, double second,
                                 double Serialization::KnobValueRangeSerialization::* firstMember,
                                 double Serialization::KnobValueRangeSerialization::* secondMember) {
            QWidget* row = new QWidget(_form);
            QHBoxLayout* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(2);
            QLineEdit* firstEdit = numberEdit(first, isInt, row);
            QLineEdit* secondEdit = numberEdit(second, isInt, row);
            firstEdit->setObjectName(objectName + QString::fromUtf8("Min"));
            secondEdit->setObjectName(objectName + QString::fromUtf8("Max"));
            rowLayout->addWidget(firstEdit);
            rowLayout->addWidget(secondEdit);
            registerField(firstEdit, [firstEdit, firstMember](UserKnobSpec* s) {
                if (Serialization::KnobValueRangeSerialization* r = std::get_if<Serialization::KnobValueRangeSerialization>(&s->extra)) {
                    r->*firstMember = numberOf(firstEdit, r->*firstMember);
                }
            });
            registerField(secondEdit, [secondEdit, secondMember](UserKnobSpec* s) {
                if (Serialization::KnobValueRangeSerialization* r = std::get_if<Serialization::KnobValueRangeSerialization>(&s->extra)) {
                    r->*secondMember = numberOf(secondEdit, r->*secondMember);
                }
            });
            _formLayout->addRow(label, row);
        };
        addPair(tr("Min / Max"), QString::fromUtf8("KnobEditPropertiesRange"), range.minimum, range.maximum,
                &Serialization::KnobValueRangeSerialization::minimum, &Serialization::KnobValueRangeSerialization::maximum);
        addPair(tr("Display Min / Max"), QString::fromUtf8("KnobEditPropertiesDisplayRange"), range.displayMinimum, range.displayMaximum,
                &Serialization::KnobValueRangeSerialization::displayMinimum, &Serialization::KnobValueRangeSerialization::displayMaximum);
    }

    const std::vector<int> dimensions = dimensionChoices(spec.typeName);
    if (!dimensions.empty()) {
        QComboBox* dims = new QComboBox(_form);
        dims->setObjectName(QString::fromUtf8("KnobEditPropertiesDimensions"));
        for (std::size_t i = 0; i < dimensions.size(); ++i) {
            dims->addItem(QString::number(dimensions[i]), dimensions[i]);
            if (dimensions[i] == spec.dimension) {
                dims->setCurrentIndex((int)i);
            }
        }
        QObject::connect(dims, SIGNAL(activated(int)), this, SLOT(onDimensionsChanged(int)));
        _formLayout->addRow(tr("Dimensions"), dims);
        _dimensionCombo = dims;
    }

    const auto addFlag = [&](const QString& label, const QString& objectName, bool value, bool UserKnobSpec::* member) {
        QCheckBox* check = new QCheckBox(_form);
        check->setObjectName(objectName);
        check->setChecked(value);
        addField(label, check, [check, member](UserKnobSpec* s) { s->*member = check->isChecked(); });
    };
    if (!isPageOrGroupKnob && !isSeparator && !isButton) {
        addFlag(tr("Animates"), QString::fromUtf8("KnobEditPropertiesAnimates"), spec.animates, &UserKnobSpec::animates);
    }
    if (!isPageOrGroupKnob && !isSeparator) {
        addFlag(tr("Evaluate On Change"), QString::fromUtf8("KnobEditPropertiesEvaluateOnChange"), spec.evaluatesOnChange, &UserKnobSpec::evaluatesOnChange);
    }
    addFlag(tr("Hide"), QString::fromUtf8("KnobEditPropertiesHide"), spec.secret, &UserKnobSpec::secret);
    if (!isPageOrGroupKnob) {
        addFlag(tr("Add New Line"), QString::fromUtf8("KnobEditPropertiesAddNewLine"), spec.newLine, &UserKnobSpec::newLine);
    }
    if (spec.typeName == KnobGroup::typeNameStatic()) {
        addFlag(tr("Group As Tab"), QString::fromUtf8("KnobEditPropertiesGroupAsTab"), spec.groupAsTab, &UserKnobSpec::groupAsTab);
    }
} // addCommonFields

void
KnobEditProperties::addTypeFields(const UserKnobSpec& spec)
{
    const auto addCheck = [&](const QString& label, const QString& objectName, bool value, const std::function<void(UserKnobSpec*, bool)>& apply) {
        QCheckBox* check = new QCheckBox(_form);
        check->setObjectName(objectName);
        check->setChecked(value);
        addField(label, check, [check, apply](UserKnobSpec* s) { apply(s, check->isChecked()); });
    };
    const auto addLines = [&](const QString& label, const QString& objectName, const QString& text, bool monospace,
                              const std::function<void(UserKnobSpec*, const QPlainTextEdit*)>& apply) {
        QPlainTextEdit* edit = new QPlainTextEdit(_form);
        edit->setObjectName(objectName);
        edit->setPlainText(text);
        QFontMetrics fm(edit->font());
        if (monospace) {
            edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            edit->setLineWrapMode(QPlainTextEdit::NoWrap);
            edit->setMinimumHeight(fm.lineSpacing() * 8);
        } else {
            edit->setMinimumHeight(fm.lineSpacing() * 4);
        }
        edit->setTabChangesFocus(!monospace);
        addField(label, edit, [edit, apply](UserKnobSpec* s) { apply(s, edit); });
    };

    if (const Serialization::KnobChoiceExtraSerialization* choice = std::get_if<Serialization::KnobChoiceExtraSerialization>(&spec.extra)) {
        addLines(tr("Menu Items"), QString::fromUtf8("KnobEditPropertiesMenuItems"), joinLines(choice->entries), false,
                 [](UserKnobSpec* s, const QPlainTextEdit* edit) {
                     Serialization::KnobChoiceExtraSerialization* c = std::get_if<Serialization::KnobChoiceExtraSerialization>(&s->extra);
                     if (!c) {
                         return;
                     }
                     std::vector<std::string> entries;
                     const QStringList lines = linesOf(edit);
                     for (int i = 0; i < lines.size(); ++i) {
                         if (!lines[i].trimmed().isEmpty()) {
                             entries.push_back(lines[i].trimmed().toStdString());
                         }
                     }
                     if (entries.empty()) {
                         return;
                     }
                     c->entries = entries;
                     c->helpStrings.resize(entries.size());
                     const std::string* def = s->defaults.empty() ? 0 : std::get_if<std::string>(&s->defaults[0]);
                     if (!def || (std::find(entries.begin(), entries.end(), *def) == entries.end())) {
                         s->defaults.assign(1, Serialization::KnobValueSerialization(entries[0]));
                     }
                 });
        addLines(tr("Item Help"), QString::fromUtf8("KnobEditPropertiesItemHelp"), joinLines(choice->helpStrings), false,
                 [](UserKnobSpec* s, const QPlainTextEdit* edit) {
                     Serialization::KnobChoiceExtraSerialization* c = std::get_if<Serialization::KnobChoiceExtraSerialization>(&s->extra);
                     if (!c) {
                         return;
                     }
                     const QStringList lines = linesOf(edit);
                     c->helpStrings.assign(c->entries.size(), std::string());
                     for (std::size_t i = 0; i < c->entries.size() && (int)i < lines.size(); ++i) {
                         c->helpStrings[i] = lines[(int)i].toStdString();
                     }
                 });
    } else if (const Serialization::KnobFileExtraSerialization* file = std::get_if<Serialization::KnobFileExtraSerialization>(&spec.extra)) {
        addCheck(tr("Sequences"), QString::fromUtf8("KnobEditPropertiesSequences"), file->useSequences, [](UserKnobSpec* s, bool v) {
            if (Serialization::KnobFileExtraSerialization* f = std::get_if<Serialization::KnobFileExtraSerialization>(&s->extra)) {
                f->useSequences = v;
            }
        });
    } else if (const Serialization::KnobPathExtraSerialization* path = std::get_if<Serialization::KnobPathExtraSerialization>(&spec.extra)) {
        addCheck(tr("Multi Path"), QString::fromUtf8("KnobEditPropertiesMultiPath"), path->multiPath, [](UserKnobSpec* s, bool v) {
            if (Serialization::KnobPathExtraSerialization* p = std::get_if<Serialization::KnobPathExtraSerialization>(&s->extra)) {
                p->multiPath = v;
            }
        });
    } else if (const Serialization::KnobLayerSelectExtraSerialization* layer = std::get_if<Serialization::KnobLayerSelectExtraSerialization>(&spec.extra)) {
        addCheck(tr("Channel Buttons"), QString::fromUtf8("KnobEditPropertiesChannelButtons"), layer->withChannelButtons, [](UserKnobSpec* s, bool v) {
            if (Serialization::KnobLayerSelectExtraSerialization* l = std::get_if<Serialization::KnobLayerSelectExtraSerialization>(&s->extra)) {
                l->withChannelButtons = v;
            }
        });
    } else if (const Serialization::KnobTextExtraSerialization* text = std::get_if<Serialization::KnobTextExtraSerialization>(&spec.extra)) {
        if (!text->isLabel) {
            addCheck(tr("Multi Line"), QString::fromUtf8("KnobEditPropertiesMultiLine"), text->isMultiLine, [](UserKnobSpec* s, bool v) {
                if (Serialization::KnobTextExtraSerialization* t = std::get_if<Serialization::KnobTextExtraSerialization>(&s->extra)) {
                    t->isMultiLine = v;
                }
            });
        }
        addCheck(tr("Rich Text"), QString::fromUtf8("KnobEditPropertiesRichText"), text->usesRichText, [](UserKnobSpec* s, bool v) {
            if (Serialization::KnobTextExtraSerialization* t = std::get_if<Serialization::KnobTextExtraSerialization>(&s->extra)) {
                t->usesRichText = v;
            }
        });
    } else if (const Serialization::KnobParametricExtraSerialization* curves = std::get_if<Serialization::KnobParametricExtraSerialization>(&spec.extra)) {
        addLines(tr("Curve Labels"), QString::fromUtf8("KnobEditPropertiesCurveLabels"), joinLines(curves->curveLabels), false,
                 [](UserKnobSpec* s, const QPlainTextEdit* edit) {
                     Serialization::KnobParametricExtraSerialization* p = std::get_if<Serialization::KnobParametricExtraSerialization>(&s->extra);
                     if (!p) {
                         return;
                     }
                     const QStringList lines = linesOf(edit);
                     for (std::size_t i = 0; i < p->curveLabels.size() && (int)i < lines.size(); ++i) {
                         p->curveLabels[i] = lines[(int)i].trimmed().toStdString();
                     }
                 });
        QWidget* row = new QWidget(_form);
        QHBoxLayout* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(2);
        QLineEdit* rangeMin = numberEdit(curves->rangeMinimum, false, row);
        QLineEdit* rangeMax = numberEdit(curves->rangeMaximum, false, row);
        rangeMin->setObjectName(QString::fromUtf8("KnobEditPropertiesCurveRangeMin"));
        rangeMax->setObjectName(QString::fromUtf8("KnobEditPropertiesCurveRangeMax"));
        rowLayout->addWidget(rangeMin);
        rowLayout->addWidget(rangeMax);
        registerField(rangeMin, [rangeMin](UserKnobSpec* s) {
            if (Serialization::KnobParametricExtraSerialization* p = std::get_if<Serialization::KnobParametricExtraSerialization>(&s->extra)) {
                p->rangeMinimum = numberOf(rangeMin, p->rangeMinimum);
            }
        });
        registerField(rangeMax, [rangeMax](UserKnobSpec* s) {
            if (Serialization::KnobParametricExtraSerialization* p = std::get_if<Serialization::KnobParametricExtraSerialization>(&s->extra)) {
                p->rangeMaximum = numberOf(rangeMax, p->rangeMaximum);
            }
        });
        _formLayout->addRow(tr("Curve Range"), row);
    } else if (const Serialization::KnobButtonExtraSerialization* button = std::get_if<Serialization::KnobButtonExtraSerialization>(&spec.extra)) {
        addLines(tr("Script"), QString::fromUtf8("KnobEditPropertiesScript"), QString::fromUtf8(button->script.c_str()), true,
                 [](UserKnobSpec* s, const QPlainTextEdit* edit) {
                     if (Serialization::KnobButtonExtraSerialization* b = std::get_if<Serialization::KnobButtonExtraSerialization>(&s->extra)) {
                         b->script = edit->toPlainText().toStdString();
                     }
                 });
    } else if (spec.typeName == KnobButton::typeNameStatic()) {
        addLines(tr("Script"), QString::fromUtf8("KnobEditPropertiesScript"), QString(), true,
                 [](UserKnobSpec* s, const QPlainTextEdit* edit) {
                     Serialization::KnobButtonExtraSerialization b;
                     b.script = edit->toPlainText().toStdString();
                     if (!b.script.empty()) {
                         s->extra = b;
                     }
                 });
    }
} // addTypeFields

bool
KnobEditProperties::eventFilter(QObject* watched,
                                QEvent* event)
{
    if (event->type() == QEvent::FocusOut) {
        for (std::size_t i = 0; i < _fields.size(); ++i) {
            if (_fields[i].widget.data() == watched) {
                const Field field = _fields[i];
                applyField(field);
                break;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void
KnobEditProperties::onFieldEdited()
{
    QObject* from = sender();
    for (std::size_t i = 0; i < _fields.size(); ++i) {
        if (_fields[i].widget.data() == from) {
            const Field field = _fields[i];
            applyField(field);

            return;
        }
    }
}

bool
KnobEditProperties::applyField(const Field& field)
{
    KnobIPtr knob = _knob.lock();
    KnobHolder* holder = _mode->getHolder();
    if (_applying || !knob || !holder || !field.widget) {
        return false;
    }
    const UserKnobSpec before = specFromKnob(knob);
    UserKnobSpec spec = before;
    field.write(&spec);
    if (spec == before) {
        return true;
    }
    if ((spec.name != before.name) && (spec.label == before.label) && (before.label == knobEditReadableLabel(before.name))) {
        spec.label = knobEditReadableLabel(spec.name);
    }
    const std::string invalid = validateSpec(holder, knob, spec);
    if (!invalid.empty()) {
        setError(QString::fromUtf8(invalid.c_str()));

        return false;
    }
    std::string error;
    _applying = true;
    const bool ok = applySpec(holder, knob, spec, &error);
    _applying = false;
    if (!ok) {
        setError(QString::fromUtf8(error.c_str()));

        return false;
    }
    setError(QString());
    if (spec.name != before.name) {
        renamePathVariable(before.name, before.label);
    }
    scheduleRebuild();

    return true;
} // applyField

void
KnobEditProperties::commitPending()
{
    const std::vector<Field> fields = _fields;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        applyField(fields[i]);
    }
}

void
KnobEditProperties::renamePathVariable(const std::string& oldName,
                                       const std::string& oldLabel)
{
    KnobIPtr knob = _knob.lock();
    Project* isProject = dynamic_cast<Project*>(_mode->getHolder());
    DockablePanel* panel = _mode->getPanel();
    if (!knob || !isProject || !panel || !dynamic_cast<KnobString*>(knob.get()) || !isProject->getApp()) {
        return;
    }
    KnobGroupPtr variables = isProject->getRFVGroup();
    if (!variables || (knob->getParentKnob() != variables)) {
        return;
    }
    ProjectPtr project = isProject->getApp()->getProject();
    std::shared_ptr<RenamePathVariableCommand> command = std::make_shared<RenamePathVariableCommand>(project, oldName, knob->getName(), oldLabel, knob->getLabel());
    if (command->hasValueEdits()) {
        ignore_result(pushPathVariableCommand(panel->getGui(), project, command));
    }
}

void
KnobEditProperties::onTypeChanged(int index)
{
    KnobIPtr knob = _knob.lock();
    if (!knob || !_typeCombo) {
        return;
    }
    const KnobEditTypeEntry* entry = findKnobEditTypeEntry(_typeCombo->itemData(index).toString());
    if (!entry) {
        return;
    }
    int dimension = entry->dimension;
    if (isNumeric(knob->typeName()) && isNumeric(entry->typeName)) {
        const std::vector<int> choices = dimensionChoices(entry->typeName);
        dimension = std::max(choices.front(), std::min(choices.back(), knob->getDimension()));
    }
    changeType(entry->typeName, dimension, entry->isLabel);
}

void
KnobEditProperties::onDimensionsChanged(int index)
{
    KnobIPtr knob = _knob.lock();
    if (!knob || !_dimensionCombo) {
        return;
    }
    changeType(knob->typeName(), _dimensionCombo->itemData(index).toInt(), specIsLabel(specFromKnob(knob)));
}

void
KnobEditProperties::changeType(const std::string& typeName,
                               int dimension,
                               bool isLabel)
{
    KnobIPtr knob = _knob.lock();
    KnobHolder* holder = _mode->getHolder();
    if (!knob || !holder) {
        return;
    }
    KnobIPtr current = knob;
    if ((typeName != knob->typeName()) || (dimension != knob->getDimension())) {
        KnobIPtr newKnob;
        std::string error;
        UserKnobTypeChangeEnum result = changeUserKnobType(holder, knob, typeName, dimension, false, &newKnob, &error);
        if (result == eUserKnobTypeChangeNeedsReset) {
            const KnobEditTypeEntry* entry = findKnobEditTypeEntry(typeName, isLabel);
            StandardButtonEnum rep = Dialogs::questionDialog(tr("Change type").toStdString(),
                                                             tr("Changing %1 to %2 resets its value, animation and links. Continue?")
                                                                 .arg(QString::fromUtf8(knob->getName().c_str()),
                                                                      entry ? entry->label : QString::fromUtf8(typeName.c_str()))
                                                                 .toStdString(),
                                                             false);
            if (rep == eStandardButtonYes) {
                result = changeUserKnobType(holder, knob, typeName, dimension, true, &newKnob, &error);
            }
        }
        if ((result == eUserKnobTypeChangeFailed) || !newKnob) {
            setError(QString::fromUtf8(error.c_str()));
            scheduleRebuild();

            return;
        }
        if (result == eUserKnobTypeChangeNeedsReset) {
            scheduleRebuild();

            return;
        }
        current = newKnob;
    }
    if (typeName == KnobString::typeNameStatic()) {
        UserKnobSpec spec = specFromKnob(current);
        Serialization::KnobTextExtraSerialization text;
        if (const Serialization::KnobTextExtraSerialization* t = std::get_if<Serialization::KnobTextExtraSerialization>(&spec.extra)) {
            text = *t;
        }
        if (text.isLabel != isLabel) {
            text.isLabel = isLabel;
            if (isLabel) {
                text.isMultiLine = false;
            }
            spec.extra = text;
            std::string error;
            if (!applySpec(holder, current, spec, &error)) {
                setError(QString::fromUtf8(error.c_str()));
            }
        }
    }
    setError(QString());
    _knob = current;
    _mode->setSelectedKnob(current);
    scheduleRebuild();
} // changeType

NATRON_NAMESPACE_EXIT
