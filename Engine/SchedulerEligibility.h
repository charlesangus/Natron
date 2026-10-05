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

#ifndef NATRON_ENGINE_SCHEDULERELIGIBILITY_H
#define NATRON_ENGINE_SCHEDULERELIGIBILITY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>

#include "Engine/EngineFwd.h"
#include "Engine/ParallelRenderArgs.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What decides whether a frame may be rendered by the RenderScheduler.
 **/
struct SchedulerEligibility {
    bool hasFrameArgs = false;

    // The input of a viewer outside the project (a file dialog preview) gets its frame args outside the setter.
    bool inputHasFrameArgs = false;
    bool isDoingPartialUpdates = false;
    bool deepUpstream = false;
    bool paintStroke = false;

    // A refresh bypasses the cache for the viewer input, which the scheduler's tasks never do.
    bool forceRender = false;
    bool openGLRender = false;
    bool analysis = false;
    bool onPoolThread = false;
    bool onMainThread = false;
};

/**
 * @brief The cause reported for each flag of a SchedulerEligibility that keeps a frame off the RenderScheduler.
 **/
struct SchedulerIneligibilityReasons {
    const char* onPoolThread;
    const char* onMainThread;
    const char* noFrameArgs;
    const char* inputOutsideFrameArgs;
    const char* partialUpdates;
    const char* deepUpstream;
    const char* paintStroke;
    const char* forceRender;
    const char* openGLRender;
    const char* analysis;
};

/**
 * @brief Sets the OpenGL, paint stroke and analysis flags of eligibility from the frame args of every node of a frame.
 **/
inline void
scanFrameArgsForScheduler(const std::map<NodePtr, ParallelRenderArgsPtr>& args,
                          SchedulerEligibility* eligibility)
{
    for (std::map<NodePtr, ParallelRenderArgsPtr>::const_iterator it = args.begin(); it != args.end(); ++it) {
        const ParallelRenderArgsPtr& nodeArgs = it->second;
        if (!nodeArgs) {
            continue;
        }
        // The context was attached to the frame's render thread; a task on a pool thread cannot make it current.
        if (nodeArgs->openGLContext.lock() && (nodeArgs->currentOpenglSupport != ePluginOpenGLRenderSupportNone)) {
            eligibility->openGLRender = true;
        }
        if (nodeArgs->isDuringPaintStrokeCreation) {
            eligibility->paintStroke = true;
        }
        if (nodeArgs->isAnalysis) {
            eligibility->analysis = true;
        }
    }
}

/**
 * @brief Whether the frame eligibility describes may be rendered by the RenderScheduler. When it may not and reason
 * is not null, *reason is the entry of reasons for the first cause.
 **/
inline bool
isFrameEligibleForScheduler(const SchedulerEligibility& eligibility,
                            const SchedulerIneligibilityReasons& reasons,
                            const char** reason)
{
    const char* cause = 0;

    if (eligibility.onPoolThread) {
        // FrameFuture::wait() on a pool thread could wait for the very thread it blocks.
        cause = reasons.onPoolThread;
    } else if (eligibility.onMainThread) {
        // Code a task runs may wait for the main thread, which would be waiting for the frame.
        cause = reasons.onMainThread;
    } else if (!eligibility.hasFrameArgs) {
        cause = reasons.noFrameArgs;
    } else if (!eligibility.inputHasFrameArgs) {
        cause = reasons.inputOutsideFrameArgs;
    } else if (eligibility.isDoingPartialUpdates) {
        cause = reasons.partialUpdates;
    } else if (eligibility.deepUpstream) {
        cause = reasons.deepUpstream;
    } else if (eligibility.paintStroke) {
        cause = reasons.paintStroke;
    } else if (eligibility.forceRender) {
        cause = reasons.forceRender;
    } else if (eligibility.openGLRender) {
        cause = reasons.openGLRender;
    } else if (eligibility.analysis) {
        cause = reasons.analysis;
    }
    if (reason) {
        *reason = cause;
    }

    return cause == 0;
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_SCHEDULERELIGIBILITY_H
