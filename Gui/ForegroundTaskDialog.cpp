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

#include "ForegroundTaskDialog.h"

#include <string>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QCloseEvent>
#include <QCoreApplication>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"

#include "Gui/Gui.h"
#include "Gui/ProgressTaskInfo.h"

NATRON_NAMESPACE_ENTER

namespace {
// Cancel, Escape and the window's close button all ask to cancel; only the owner hides the
// dialog, once its tasks have actually ended.
class ForegroundProgressDialog
    : public QProgressDialog {
public:
    explicit ForegroundProgressDialog(QWidget* parent)
        : QProgressDialog(parent)
    {
        // QProgressDialog::cancel() hides the dialog. Qt makes this connection by signature, so a
        // member-pointer disconnect would not match it and Cancel would hide the dialog at once.
        QObject::disconnect(this, SIGNAL(canceled()), this, SLOT(cancel()));
    }

    virtual void reject() OVERRIDE FINAL
    {
        Q_EMIT canceled();
    }

protected:
    virtual void closeEvent(QCloseEvent* e) OVERRIDE FINAL
    {
        e->ignore();
        Q_EMIT canceled();
    }
};
} // anonymous namespace

ForegroundTaskDialog::ForegroundTaskDialog()
    : _gui(0)
    , _texts()
    , _dialog()
    , _bar()
    , _entries()
    , _failedLabels()
    , _held(false)
    , _userCanceled(false)
    , _closed(false)
    , _onCancel()
{
}

ForegroundTaskDialog::~ForegroundTaskDialog()
{
}

ForegroundTaskDialogPtr
ForegroundTaskDialog::show(Gui* gui,
                           const Texts& texts,
                           const std::vector<TaskDesc>& tasks,
                           bool held)
{
    if (!gui) {
        return ForegroundTaskDialogPtr();
    }

    ForegroundTaskDialogPtr self(new ForegroundTaskDialog());
    self->_gui = gui;
    self->_texts = texts;
    self->_held = held;

    bool anyRunning = false;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        Entry entry;
        entry.label = tasks[i].label;
        entry.task = tasks[i].task;
        entry.progress = 0.;
        entry.done = !entry.task || entry.task->wasCanceled();
        entry.stopping = false;
        if (!entry.done) {
            entry.task->setForeground(true);
            anyRunning = true;
        }
        self->_entries.push_back(entry);
    }
    if (!anyRunning && !held) {
        return ForegroundTaskDialogPtr();
    }

    ForegroundProgressDialog* dialog = new ForegroundProgressDialog(gui);
    dialog->setObjectName(QString::fromUtf8("ForegroundTaskDialog"));
    dialog->setWindowTitle(texts.title);
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    QProgressBar* bar = new QProgressBar(dialog);
    dialog->setBar(bar);
    dialog->setRange(0, 1000);
    dialog->setCancelButtonText(QCoreApplication::translate("ForegroundTaskDialog", "Cancel"));
    dialog->setMinimumDuration(0);
    self->_dialog = dialog;
    self->_bar = bar;

    self->connectTasks();
    self->refresh();
    if (self->_closed) {
        return ForegroundTaskDialogPtr();
    }

    // open() would force Qt::WindowModal, which leaves floating panels usable; show() with
    // application modality blocks every window without spinning a nested event loop.
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->show();

    return self;
} // ForegroundTaskDialog::show

void
ForegroundTaskDialog::connectTasks()
{
    // The dialog holds this object through these connections and drops it when deleted.
    ForegroundTaskDialogPtr self = shared_from_this();
    QProgressDialog* dialog = _dialog.data();

    for (std::size_t i = 0; i < _entries.size(); ++i) {
        if (_entries[i].done) {
            continue;
        }
        ProgressTaskInfo* taskPtr = _entries[i].task.get();
        QObject::connect(taskPtr, &ProgressTaskInfo::progressChanged, dialog, [self, taskPtr](double progress) {
            Entry* entry = self->entryFor(taskPtr);
            if (entry) {
                entry->progress = progress;
            }
            self->refresh();
        });
        QObject::connect(taskPtr, &ProgressTaskInfo::taskFinished, dialog, [self, taskPtr](int retCode) {
            Entry* entry = self->entryFor(taskPtr);
            // A cancelled task finishes at once, while its render may still be stopping.
            if (entry && !entry->stopping) {
                if (!entry->done && (retCode != 0) && !self->_userCanceled) {
                    self->_failedLabels.push_back(entry->label);
                }
                entry->done = true;
            }
            self->refresh();
        });
        NodePtr node = taskPtr->getNode();
        OutputEffectInstance* writer = node ? dynamic_cast<OutputEffectInstance*>(node->getEffectInstance().get()) : 0;
        RenderEnginePtr engine = writer ? writer->getRenderEngine() : RenderEnginePtr();
        if (engine) {
            QObject::connect(engine.get(), &RenderEngine::renderFinished, dialog, [self, taskPtr](int /*retCode*/) {
                Entry* entry = self->entryFor(taskPtr);
                if (entry && entry->stopping) {
                    entry->stopping = false;
                    entry->done = true;
                }
                self->refresh();
            });
        }
    }
    QObject::connect(dialog, &QProgressDialog::canceled, dialog, [self]() {
        self->cancel();
    });
}

void
ForegroundTaskDialog::refresh()
{
    if (_closed) {
        return;
    }

    const std::size_t n = _entries.size();
    std::size_t current = n;
    double total = 0.;
    for (std::size_t i = 0; i < n; ++i) {
        const Entry& entry = _entries[i];
        total += entry.done ? 1. : entry.progress;
        if (!entry.done && (current == n)) {
            current = i;
        }
    }

    if ((current == n) && !_held) {
        close();

        return;
    }
    if (_userCanceled) {
        if (_dialog) {
            _dialog->setLabelText(QCoreApplication::translate("ForegroundTaskDialog", "Stopping..."));
            QPushButton* cancelButton = _dialog->findChild<QPushButton*>();
            if (cancelButton) {
                cancelButton->setEnabled(false);
            }
        }

        return;
    }
    if (current == n) {
        return;
    }

    if (!_dialog || !_bar) {
        return;
    }
    const Entry& entry = _entries[current];
    // One pass, so a label holding "%2" is left as it is.
    QString text = _texts.taskFormat.contains(QLatin1String("%3"))
        ? _texts.taskFormat.arg(entry.label, QString::number((int)current + 1), QString::number((int)n))
        : _texts.taskFormat.arg(entry.label);
    int frame = entry.task ? entry.task->getLastRenderedFrame() : -1;
    if (frame != -1) {
        text += QLatin1Char('\n');
        text += QCoreApplication::translate("ForegroundTaskDialog", "Frame %1").arg(frame);
    }
    _dialog->setLabelText(text);
    // Set on the bar rather than through QProgressDialog::setValue, which calls processEvents()
    // on a modal dialog and would re-enter these task slots.
    _bar->setValue((int)(1000. * total / n));
} // ForegroundTaskDialog::refresh

void
ForegroundTaskDialog::cancel()
{
    if (_closed || _userCanceled) {
        return;
    }
    _userCanceled = true;

    std::vector<ProgressTaskInfoPtr> toCancel;
    for (std::size_t i = 0; i < _entries.size(); ++i) {
        Entry& entry = _entries[i];
        if (!entry.done && entry.task) {
            toCancel.push_back(entry.task);
            // Until the render has stopped, its job may still refuse edits.
            entry.stopping = true;
        } else {
            entry.done = true;
        }
    }
    // The tasks end as Canceled, not Paused (see ProgressTaskInfo::setForeground).
    for (std::size_t i = 0; i < toCancel.size(); ++i) {
        if (!toCancel[i]->wasCanceled()) {
            toCancel[i]->cancelTask(false, 1);
        }
    }
    // A render still active reports renderFinished once it has stopped; one that never started
    // has left the queue and will not.
    for (std::size_t i = 0; i < toCancel.size(); ++i) {
        Entry* entry = entryFor(toCancel[i].get());
        if (!entry || !entry->stopping) {
            continue;
        }
        NodePtr node = toCancel[i]->getNode();
        OutputEffectInstance* writer = node ? dynamic_cast<OutputEffectInstance*>(node->getEffectInstance().get()) : 0;
        AppInstancePtr app = node ? node->getApp() : AppInstancePtr();
        if (!writer || !app || !app->isRenderActiveOrQueued(writer)) {
            entry->stopping = false;
            entry->done = true;
        }
    }
    if (_onCancel) {
        std::function<void()> onCancel = _onCancel;
        onCancel();
    }
    refresh();
}

ForegroundTaskDialog::Entry*
ForegroundTaskDialog::entryFor(const ProgressTaskInfo* task)
{
    for (std::size_t i = 0; i < _entries.size(); ++i) {
        if (_entries[i].task.get() == task) {
            return &_entries[i];
        }
    }

    return 0;
}

void
ForegroundTaskDialog::setOnCancel(const std::function<void()>& onCancel)
{
    _onCancel = onCancel;
}

void
ForegroundTaskDialog::release()
{
    if (!_held) {
        return;
    }
    _held = false;
    refresh();
}

bool
ForegroundTaskDialog::isClosed() const
{
    return _closed;
}

void
ForegroundTaskDialog::close()
{
    if (_closed) {
        return;
    }
    _closed = true;
    _entries.clear();
    if (_dialog) {
        _dialog->hide();
        _dialog->deleteLater();
    }
    if (!_failedLabels.isEmpty() && _gui) {
        QString message;
        for (int i = 0; i < _failedLabels.size(); ++i) {
            if (i > 0) {
                message += QLatin1Char('\n');
            }
            message += _texts.failureFormat.arg(_failedLabels[i]);
        }
        const std::string title = _texts.failureTitle.toStdString();
        // Deferred because this runs inside the finishing task's own signal, and the error
        // dialog's nested event loop must not run until that task has unwound.
        QTimer::singleShot(0, _gui, [title, message]() {
            Dialogs::errorDialog(title, message.toStdString());
        });
    }
}

NATRON_NAMESPACE_EXIT
