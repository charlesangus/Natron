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

#include "LoopDriverTable.h"

#include <algorithm>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QtCore/QMetaObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobDriver.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Loop/LoopEnd.h"

#include "Gui/ComboBox.h"
#include "Gui/GuiMacros.h"
#include "Gui/KnobDriverRow.h"
#include "Gui/SpinBox.h"

NATRON_NAMESPACE_ENTER

namespace {
enum ColumnEnum {
    eColumnNode = 0,
    eColumnParameter,
    eColumnVariable,
    eColumnStart,
    eColumnEnd,
    eColumnEasing,
    eColumnValue,
    eColumnCount
};

const char* const kRowProperty = "loopDriverRow";

const KnobDriver::EasingEnum kEasings[] = { KnobDriver::eEasingLinear, KnobDriver::eEasingSmooth, KnobDriver::eEasingCurve };

LoopEnd*
loopEndOf(const KnobIPtr& knob)
{
    return knob ? dynamic_cast<LoopEnd*>(knob->getHolder()) : 0;
}

QTableWidgetItem*
readOnlyItem(const QString& text)
{
    QTableWidgetItem* item = new QTableWidgetItem(text);

    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    return item;
}
} // anon namespace

bool
LoopDriverTable::isTableKnob(const KnobIPtr& knob)
{
    return loopEndOf(knob) && (knob->getName() == kLoopEndParamDrivenKnobs);
}

LoopDriverTable::LoopDriverTable(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    : KnobGui(knob, container)
    , _knob(knob)
    , _table(0)
    , _entries()
    , _watched()
    , _rebuildPending(false)
{
}

LoopDriverTable::~LoopDriverTable()
{
}

KnobIPtr
LoopDriverTable::getKnob() const
{
    return _knob.lock();
}

void
LoopDriverTable::createWidget(QHBoxLayout* layout)
{
    _table = new QTableWidget(0, eColumnCount, layout->parentWidget());
    _table->setObjectName(QString::fromUtf8("LoopDriverTable"));
    _table->setHorizontalHeaderLabels(QStringList() << tr("Node") << tr("Parameter") << tr("Variable") << tr("Start")
                                                    << tr("End") << tr("Easing") << tr("Value"));
    _table->setToolTip(tr("Every parameter of the loop's body that a loop variable drives. Value is the "
                          "parameter's value at the iteration Single Pass Iteration names."));
    _table->verticalHeader()->setVisible(false);
    _table->horizontalHeader()->setStretchLastSection(true);
    _table->setSelectionMode(QAbstractItemView::NoSelection);
    _table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    _table->setMinimumHeight(TO_DPIY(120));
    layout->addWidget(_table);
    rebuild();
}

void
LoopDriverTable::removeSpecificGui()
{
    disconnectWatched();
    if (_table) {
        _table->deleteLater();
        _table = 0;
    }
}

void
LoopDriverTable::_hide()
{
    if (_table) {
        _table->hide();
    }
}

void
LoopDriverTable::_show()
{
    if (_table) {
        _table->show();
    }
    // The body may have changed while the panel was closed.
    scheduleRebuild();
}

void
LoopDriverTable::scheduleRebuild()
{
    if (_rebuildPending) {
        return;
    }
    _rebuildPending = true;
    QMetaObject::invokeMethod(this, "rebuild", Qt::QueuedConnection);
}

void
LoopDriverTable::disconnectWatched()
{
    for (const KnobIWPtr& weak : _watched) {
        KnobIPtr knob = weak.lock();
        KnobHelper* helper = dynamic_cast<KnobHelper*>(knob.get());
        if (helper && helper->getSignalSlotHandler()) {
            QObject::disconnect(helper->getSignalSlotHandler().get(), 0, this, 0);
        }
    }
    _watched.clear();
}

void
LoopDriverTable::rebuild()
{
    _rebuildPending = false;
    if (!_table) {
        return;
    }
    disconnectWatched();
    const std::vector<Entry> previous = _entries;
    _entries.clear();

    LoopEnd* end = loopEndOf(_knob.lock());
    if (!end) {
        _table->setRowCount(0);

        return;
    }
    // The loop's own knobs move the preview iteration and so the values shown.
    const KnobsVec& ownKnobs = end->getKnobs();
    for (const KnobIPtr& knob : ownKnobs) {
        KnobHelper* helper = dynamic_cast<KnobHelper*>(knob.get());
        if (helper && helper->getSignalSlotHandler()) {
            QObject::connect(helper->getSignalSlotHandler().get(), SIGNAL(valueChanged(ViewSpec, int, int)), this, SLOT(refreshValues()));
            _watched.push_back(knob);
        }
    }

    const std::vector<NodePtr> body = end->bodyNodes();
    for (const NodePtr& node : body) {
        const KnobsVec& knobs = node->getKnobs();
        for (const KnobIPtr& knob : knobs) {
            if (!LoopDriverEdit::canDrive(knob)) {
                continue;
            }
            KnobHelper* helper = dynamic_cast<KnobHelper*>(knob.get());
            if (helper->getSignalSlotHandler()) {
                KnobSignalSlotHandler* handler = helper->getSignalSlotHandler().get();
                QObject::connect(handler, SIGNAL(driverChanged(int)), this, SLOT(scheduleRebuild()));
                QObject::connect(handler, SIGNAL(valueChanged(ViewSpec, int, int)), this, SLOT(refreshValues()));
                _watched.push_back(knob);
            }
            const std::vector<KnobDriverPtr> drivers = LoopDriverEdit::driversOf(knob);
            for (int i = 0; i < (int)drivers.size(); ++i) {
                if (drivers[i]) {
                    Entry entry;
                    entry.knob = knob;
                    entry.dimension = i;
                    _entries.push_back(entry);
                }
            }
        }
    }

    // Same rows: update the cells in place, so a spin box being dragged survives its own edit.
    bool sameRows = (previous.size() == _entries.size()) && (_table->rowCount() == (int)_entries.size());
    for (std::size_t i = 0; sameRows && i < _entries.size(); ++i) {
        sameRows = (previous[i].knob.lock() == _entries[i].knob.lock()) && (previous[i].dimension == _entries[i].dimension);
    }
    if (sameRows) {
        for (int row = 0; row < (int)_entries.size(); ++row) {
            const KnobDriverPtr driver = LoopDriverEdit::driversOf(_entries[row].knob.lock())[_entries[row].dimension];
            ComboBox* variable = qobject_cast<ComboBox*>(_table->cellWidget(row, eColumnVariable));
            SpinBox* start = qobject_cast<SpinBox*>(_table->cellWidget(row, eColumnStart));
            SpinBox* endValue = qobject_cast<SpinBox*>(_table->cellWidget(row, eColumnEnd));
            ComboBox* easing = qobject_cast<ComboBox*>(_table->cellWidget(row, eColumnEasing));
            const QString current = QString::fromUtf8(driver->getVariable().c_str());
            if (variable && !variable->itemTexts().contains(current)) {
                variable->addItem(current);
            }
            if (variable) {
                variable->setCurrentText_no_emit(current);
            }
            if (start && !start->hasFocus()) {
                start->setValue(driver->getStart());
            }
            if (endValue && !endValue->hasFocus()) {
                endValue->setValue(driver->getEnd());
            }
            if (easing) {
                easing->setCurrentIndex_no_emit((int)(std::find(kEasings, kEasings + 3, driver->getEasing()) - kEasings));
            }
        }
        refreshValues();

        return;
    }

    _table->setRowCount(0);
    _table->setRowCount((int)_entries.size());
    for (int row = 0; row < (int)_entries.size(); ++row) {
        const KnobIPtr knob = _entries[row].knob.lock();
        const int dimension = _entries[row].dimension;
        const KnobDriverPtr driver = LoopDriverEdit::driversOf(knob)[dimension];
        const NodePtr node = LoopDriverEdit::nodeOf(knob);

        _table->setItem(row, eColumnNode, readOnlyItem(QString::fromUtf8(node->getLabel().c_str())));
        _table->setItem(row, eColumnParameter, readOnlyItem(LoopDriverEdit::knobLabel(knob, knob->getDimension() > 1 ? dimension : -1)));

        ComboBox* variable = new ComboBox(_table);
        const QStringList variables = LoopDriverEdit::loopVariables(node);
        const QString current = QString::fromUtf8(driver->getVariable().c_str());
        for (const QString& name : variables) {
            variable->addItem(name);
        }
        if (!variables.contains(current)) {
            variable->addItem(current);
        }
        variable->setCurrentText_no_emit(current);
        variable->setProperty(kRowProperty, row);
        QObject::connect(variable, SIGNAL(currentIndexChanged(int)), this, SLOT(onVariableChanged(int)));
        _table->setCellWidget(row, eColumnVariable, variable);

        SpinBox* start = new SpinBox(_table, SpinBox::eSpinBoxTypeDouble);
        start->setValue(driver->getStart());
        start->setProperty(kRowProperty, row);
        QObject::connect(start, SIGNAL(valueChanged(double)), this, SLOT(onStartChanged(double)));
        _table->setCellWidget(row, eColumnStart, start);

        SpinBox* endValue = new SpinBox(_table, SpinBox::eSpinBoxTypeDouble);
        endValue->setValue(driver->getEnd());
        endValue->setProperty(kRowProperty, row);
        QObject::connect(endValue, SIGNAL(valueChanged(double)), this, SLOT(onEndChanged(double)));
        _table->setCellWidget(row, eColumnEnd, endValue);

        ComboBox* easing = new ComboBox(_table);
        easing->addItem(tr("Linear"));
        easing->addItem(tr("Smooth"));
        easing->addItem(tr("Curve"));
        easing->setToolTip(tr("Edit the curve itself from the parameter's driver row."));
        easing->setCurrentIndex_no_emit((int)(std::find(kEasings, kEasings + 3, driver->getEasing()) - kEasings));
        easing->setProperty(kRowProperty, row);
        QObject::connect(easing, SIGNAL(currentIndexChanged(int)), this, SLOT(onEasingChanged(int)));
        _table->setCellWidget(row, eColumnEasing, easing);

        _table->setItem(row, eColumnValue, readOnlyItem(QString()));
    }
    _table->resizeColumnsToContents();
    refreshValues();
}

void
LoopDriverTable::refreshValues()
{
    if (!_table) {
        return;
    }
    for (int row = 0; row < (int)_entries.size() && row < _table->rowCount(); ++row) {
        const KnobIPtr knob = _entries[row].knob.lock();
        QTableWidgetItem* item = _table->item(row, eColumnValue);
        if (knob && item) {
            item->setText(QString::number(LoopDriverEdit::previewValue(knob, _entries[row].dimension), 'g', 6));
        }
    }
}

int
LoopDriverTable::senderRow() const
{
    const QObject* widget = sender();

    if (!widget) {
        return -1;
    }
    bool ok = false;
    const int row = widget->property(kRowProperty).toInt(&ok);

    return (ok && (row >= 0) && (row < (int)_entries.size())) ? row : -1;
}

void
LoopDriverTable::onVariableChanged(int index)
{
    const int row = senderRow();
    const ComboBox* combo = qobject_cast<const ComboBox*>(sender());

    if ((row < 0) || !combo || (index < 0)) {
        return;
    }
    LoopDriverEdit::DriverChange change(LoopDriverEdit::DriverChange::eFieldVariable);
    change.variable = combo->itemText(index).toStdString();
    LoopDriverEdit::editDrivers(getGui(), _entries[row].knob.lock(), _entries[row].dimension, change, 0);
}

void
LoopDriverTable::onStartChanged(double value)
{
    const int row = senderRow();
    const SpinBox* spin = qobject_cast<const SpinBox*>(sender());

    if ((row < 0) || !spin) {
        return;
    }
    LoopDriverEdit::DriverChange change(LoopDriverEdit::DriverChange::eFieldStart);
    change.value = value;
    LoopDriverEdit::editDrivers(getGui(), _entries[row].knob.lock(), _entries[row].dimension, change, spin->getGestureKey());
}

void
LoopDriverTable::onEndChanged(double value)
{
    const int row = senderRow();
    const SpinBox* spin = qobject_cast<const SpinBox*>(sender());

    if ((row < 0) || !spin) {
        return;
    }
    LoopDriverEdit::DriverChange change(LoopDriverEdit::DriverChange::eFieldEnd);
    change.value = value;
    LoopDriverEdit::editDrivers(getGui(), _entries[row].knob.lock(), _entries[row].dimension, change, spin->getGestureKey());
}

void
LoopDriverTable::onEasingChanged(int index)
{
    const int row = senderRow();

    if ((row < 0) || (index < 0) || (index >= 3)) {
        return;
    }
    LoopDriverEdit::DriverChange change(LoopDriverEdit::DriverChange::eFieldEasing);
    change.easing = kEasings[index];
    LoopDriverEdit::editDrivers(getGui(), _entries[row].knob.lock(), _entries[row].dimension, change, 0);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_LoopDriverTable.cpp"
