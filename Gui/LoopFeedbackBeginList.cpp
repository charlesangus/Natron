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

#include "LoopFeedbackBeginList.h"

#include <algorithm>
#include <string>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QtCore/QMetaObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Loop/LoopEnd.h"

#include "Gui/GuiMacros.h"
#include "Gui/KnobUndoCommand.h"

NATRON_NAMESPACE_ENTER

namespace {
LoopEnd*
loopEndOf(const KnobIPtr& knob)
{
    return knob ? dynamic_cast<LoopEnd*>(knob->getHolder()) : 0;
}
} // anon namespace

bool
LoopFeedbackBeginList::isListKnob(const KnobIPtr& knob)
{
    return loopEndOf(knob) && (knob->getName() == kLoopEndParamPassThroughBegins);
}

LoopFeedbackBeginList::LoopFeedbackBeginList(KnobIPtr knob,
                                             KnobGuiContainerI* container)
    : KnobGui(knob, container)
    , _knob(knob)
    , _list(0)
    , _watched()
    , _rebuildPending(false)
{
}

LoopFeedbackBeginList::~LoopFeedbackBeginList()
{
}

KnobIPtr
LoopFeedbackBeginList::getKnob() const
{
    return _knob.lock();
}

void
LoopFeedbackBeginList::createWidget(QHBoxLayout* layout)
{
    _list = new QListWidget(layout->parentWidget());
    _list->setObjectName(QString::fromUtf8("LoopFeedbackBeginList"));
    _list->setToolTip(tr("Ticked LoopBegins output the iteration before under Feedback; unticked ones pass "
                         "their input through at every iteration."));
    _list->setSelectionMode(QAbstractItemView::NoSelection);
    _list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    _list->setMinimumHeight(TO_DPIY(60));
    QObject::connect(_list, SIGNAL(itemChanged(QListWidgetItem*)), this, SLOT(onItemChanged(QListWidgetItem*)));
    layout->addWidget(_list);
    rebuild();
}

void
LoopFeedbackBeginList::removeSpecificGui()
{
    disconnectWatched();
    if (_list) {
        _list->deleteLater();
        _list = 0;
    }
}

void
LoopFeedbackBeginList::_hide()
{
    if (_list) {
        _list->hide();
    }
}

void
LoopFeedbackBeginList::_show()
{
    if (_list) {
        _list->show();
    }
    scheduleRebuild();
}

void
LoopFeedbackBeginList::updateGUI(int /*dimension*/)
{
    scheduleRebuild();
}

void
LoopFeedbackBeginList::scheduleRebuild()
{
    if (_rebuildPending) {
        return;
    }
    _rebuildPending = true;
    QMetaObject::invokeMethod(this, "rebuild", Qt::QueuedConnection);
}

void
LoopFeedbackBeginList::disconnectWatched()
{
    for (const NodeWPtr& weak : _watched) {
        NodePtr node = weak.lock();
        if (node) {
            QObject::disconnect(node.get(), 0, this, 0);
        }
    }
    _watched.clear();
}

void
LoopFeedbackBeginList::rebuild()
{
    _rebuildPending = false;
    if (!_list) {
        return;
    }
    disconnectWatched();
    LoopEnd* end = loopEndOf(_knob.lock());
    const std::vector<NodePtr> begins = end ? end->getLoopBegins() : std::vector<NodePtr>();
    const std::vector<std::string> passThrough = end ? end->getPassThroughBeginNames() : std::vector<std::string>();

    if (end) {
        // Which LoopBegins open the loop moves with any wiring of its body.
        std::vector<NodePtr> watched = end->bodyNodes();
        watched.push_back(end->getNode());
        for (const NodePtr& node : watched) {
            if (!node) {
                continue;
            }
            QObject::connect(node.get(), SIGNAL(inputChanged(int)), this, SLOT(scheduleRebuild()));
            QObject::connect(node.get(), SIGNAL(scriptNameChanged(QString)), this, SLOT(scheduleRebuild()));
            QObject::connect(node.get(), SIGNAL(labelChanged(QString)), this, SLOT(scheduleRebuild()));
            _watched.push_back(node);
        }
    }

    _list->blockSignals(true);
    _list->clear();
    for (const NodePtr& begin : begins) {
        const std::string name = begin->getScriptName_mt_safe();
        QListWidgetItem* item = new QListWidgetItem(QString::fromUtf8(begin->getLabel().c_str()), _list);
        item->setData(Qt::UserRole, QString::fromUtf8(name.c_str()));
        item->setToolTip(QString::fromUtf8(name.c_str()));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        const bool feedsBack = std::find(passThrough.begin(), passThrough.end(), name) == passThrough.end();
        item->setCheckState(feedsBack ? Qt::Checked : Qt::Unchecked);
    }
    _list->blockSignals(false);
}

void
LoopFeedbackBeginList::onItemChanged(QListWidgetItem* item)
{
    KnobStringPtr knob = std::dynamic_pointer_cast<KnobString>(_knob.lock());
    LoopEnd* end = loopEndOf(knob);

    if (!item || !knob || !end) {
        return;
    }
    const std::string name = item->data(Qt::UserRole).toString().toStdString();
    std::vector<std::string> names = end->getPassThroughBeginNames();
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
    if (item->checkState() != Qt::Checked) {
        names.push_back(name);
    }
    const std::string value = LoopEnd::encodePassThroughBeginNames(names);
    if (value != knob->getValue()) {
        pushUndoCommand(new KnobUndoCommand<std::string>(shared_from_this(), knob->getValue(0), value));
    }
}

NATRON_NAMESPACE_EXIT
