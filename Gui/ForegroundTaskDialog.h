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

#ifndef Gui_ForegroundTaskDialog_h
#define Gui_ForegroundTaskDialog_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPointer>
#include <QString>
#include <QStringList>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"

class QProgressBar;
class QProgressDialog;

NATRON_NAMESPACE_ENTER

class ForegroundTaskDialog;
typedef std::shared_ptr<ForegroundTaskDialog> ForegroundTaskDialogPtr;

/**
 * @brief An application-modal progress dialog over the main window that follows progress-panel
 * tasks running in this process, so that nothing in the interface can edit what they read.
 * Cancel, Escape or closing the window cancels every task still running. The dialog closes once
 * each task has finished or been cancelled and nothing holds it open, then reports the tasks that
 * failed in one error dialog. Its object name is "ForegroundTaskDialog".
 **/
class ForegroundTaskDialog
    : public std::enable_shared_from_this<ForegroundTaskDialog> {
public:
    struct TaskDesc {
        QString label;
        ProgressTaskInfoPtr task;
    };

    struct Texts {
        // The window title, such as "Rendering".
        QString title;
        // The current task's line: %1 is its label and, when the format has them, %2 its index
        // from 1 and %3 the task count.
        QString taskFormat;
        // One line of the error dialog per failed task: %1 is its label.
        QString failureFormat;
        // The error dialog's title.
        QString failureTitle;
    };

    /**
     * @brief Shows the dialog for the tasks of `tasks` that have not ended yet, marking them
     * foreground so the progress panel does not raise itself for them. With `held`, the dialog
     * stays open after its tasks end, until release() is called, for a job that outlives its
     * progress task. Returns null and shows nothing when no task is running and nothing holds it.
     **/
    static ForegroundTaskDialogPtr show(Gui* gui,
                                        const Texts& texts,
                                        const std::vector<TaskDesc>& tasks,
                                        bool held = false);

    ~ForegroundTaskDialog();

    /**
     * @brief Lets the dialog close once its tasks have ended; closes it now if they have.
     **/
    void release();

    /**
     * @brief Calls `onCancel` when the user cancels, after the tasks have been cancelled, for a
     * job that stops faster when told directly than through its progress task.
     **/
    void setOnCancel(const std::function<void()>& onCancel);

    /**
     * @brief Whether the dialog has closed.
     **/
    bool isClosed() const;

private:
    struct Entry {
        QString label;
        ProgressTaskInfoPtr task;
        double progress;
        bool done;
    };

    ForegroundTaskDialog();

    void connectTasks();

    void refresh();

    void cancel();

    void close();

    Gui* _gui;
    Texts _texts;
    QPointer<QProgressDialog> _dialog;
    QPointer<QProgressBar> _bar;
    std::vector<Entry> _entries;
    QStringList _failedLabels;
    bool _held;
    bool _userCanceled;
    bool _closed;
    std::function<void()> _onCancel;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ForegroundTaskDialog_h
