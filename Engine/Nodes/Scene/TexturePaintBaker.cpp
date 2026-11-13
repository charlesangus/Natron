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

#include "TexturePaintBaker.h"

#include <algorithm>

#include "Engine/AppManager.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_ENTER

namespace {
const std::chrono::milliseconds kProgressInterval(50);
}

class TexturePaintBaker::Task
    : public GenericThreadStartArgs {
public:
    explicit Task(const Bake& b)
        : bake(b)
    {
    }

    Bake bake;
    int epoch = 0;
};

void
TexturePaintBaker::Context::progress(double fraction,
                                     const QString& label)
{
    _baker->reportProgress(fraction, label);
}

TexturePaintBaker::TexturePaintBaker()
    : GenericSchedulerThread()
    , _cancel(false)
    , _pending(0)
    , _generation(0)
    , _stopped(false)
    , _epoch(0)
    , _lastProgress()
    , _lastFraction(0.)
{
    setThreadName("TexturePaintBaker");
    // The thread object lives on the main thread, so this slot does too.
    connect(this, &TexturePaintBaker::workDone, this, &TexturePaintBaker::onWorkDone, Qt::QueuedConnection);
}

TexturePaintBaker::~TexturePaintBaker()
{
    stop();
}

bool
TexturePaintBaker::enqueue(const Bake& bake)
{
    if (_stopped.load()) {
        return false;
    }
    std::shared_ptr<Task> task = std::make_shared<Task>(bake);
    task->epoch = _epoch.load();
    ++_pending;
    if (!startTask(task)) {
        --_pending;

        return false;
    }

    return true;
}

void
TexturePaintBaker::requestStop(bool allowRestart)
{
    if (!allowRestart) {
        _stopped = true;
    }
    ++_epoch;
    _pending = 0;
    _cancel = true;
    quitThread(allowRestart);
}

void
TexturePaintBaker::stop(bool allowRestart)
{
    requestStop(allowRestart);
    waitForThreadToQuit_enforce_blocking();
    std::lock_guard<std::mutex> lock(_mutex);
    _finished.clear();
}

void
TexturePaintBaker::getLastProgress(double* fraction,
                                   QString* label) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    *fraction = _lastFraction;
    *label = _lastLabel;
}

void
TexturePaintBaker::onAbortRequested(bool /*keepOldestRender*/)
{
    _cancel = true;
}

void
TexturePaintBaker::onQuitRequested(bool /*allowRestarts*/)
{
    _cancel = true;
}

void
TexturePaintBaker::reportProgress(double fraction,
                                  const QString& label)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _lastFraction = fraction;
        _lastLabel = label;
        if ((now - _lastProgress) < kProgressInterval) {
            return;
        }
        _lastProgress = now;
    }
    Q_EMIT progress(fraction, label);
}

GenericSchedulerThread::ThreadStateEnum
TexturePaintBaker::threadLoopOnce(const GenericThreadStartArgsPtr& inArgs)
{
    const std::shared_ptr<Task> task = std::dynamic_pointer_cast<Task>(inArgs);

    // A quit asked before this point has set mustQuit already; one asked after calls onQuitRequested().
    _cancel = mustQuitThread();
    bool ok = false;
    if (task && !_cancel.load()) {
        Context context(this);
        context.progress(0., task->bake.label);
        ok = task->bake.work && task->bake.work(context) && !_cancel.load();
    }
    appPTR->getAppTLS()->cleanupTLSForThread();
    if (task) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _finished.push_back(std::make_pair(task, ok));
        }
        Q_EMIT workDone();
    }

    return resolveState();
}

void
TexturePaintBaker::onWorkDone()
{
    for (;;) {
        std::pair<std::shared_ptr<Task>, bool> done;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_stopped.load() || _finished.empty()) {
                return;
            }
            done = _finished.front();
            _finished.pop_front();
        }
        if (done.first->epoch != _epoch.load()) {
            continue;
        }
        if (done.second) {
            ++_generation;
        }
        if (done.first->bake.completion) {
            done.first->bake.completion(done.second);
        }
        const int left = --_pending;
        Q_EMIT bakeFinished(done.second, _generation.load());
        if (left <= 0) {
            Q_EMIT idle();
        }
    }
}

NATRON_NAMESPACE_EXIT
