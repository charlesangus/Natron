/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef Engine_VectorFollowTask_h
#define Engine_VectorFollowTask_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/GenericSchedulerThread.h"

NATRON_NAMESPACE_ENTER

struct VectorFollowParams;
struct VectorFollowReport;

/**
 * @brief Runs a follow off the main thread: it reads vectors and reduces there, and applies each
 * frame back on the main thread as it arrives, so the viewer stays live and shows the shapes moving.
 * The report is kept here until the next follow starts and stored on the node as an information
 * message that carries no warning or error mark. A follow that stopped on a point it could not
 * track, failed to read or lost its source also opens a dialog; a clean one, or one the user
 * stopped, opens none. While it runs, the host refuses edits to its parameters, inputs and, from
 * Python, its shapes, as during a render or a track that reads it. Everything public is main thread.
 **/
class VectorFollowTask
    : public GenericSchedulerThread {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit VectorFollowTask(const NodePtr& host);

    virtual ~VectorFollowTask();

    // False, with the reason in error, when the follow cannot start.
    bool startFollow(const VectorFollowParams& params, std::string* error);

    bool isFollowing() const;

    // A follow that was running ends with what it had done so far. With wait, it has ended on return.
    void abortFollow(bool wait);

    // False when no follow has ended since the last forgetLastReport() or startFollow().
    bool getLastReport(VectorFollowReport* report) const;
    void forgetLastReport();

Q_SIGNALS:

    void framesReady();
    void phaseDone(int phase, int generation);

private Q_SLOTS:

    void onFramesReady();
    void onProgressPoll();
    void onPhaseDone(int phase, int generation);
    void onSourceDeactivated();

private:
    void requestAbort();

    virtual TaskQueueBehaviorEnum tasksQueueBehaviour() const OVERRIDE FINAL;
    virtual ThreadStateEnum threadLoopOnce(const GenericThreadStartArgsPtr& inArgs) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void onAbortRequested(bool keepOldestRender) OVERRIDE FINAL;
    virtual void onQuitRequested(bool allowRestarts) OVERRIDE FINAL;
    virtual void onQueuedTasksDropped(const std::list<GenericThreadStartArgsPtr>& tasks) OVERRIDE FINAL;

    struct Imp;
    std::unique_ptr<Imp> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_VectorFollowTask_h
