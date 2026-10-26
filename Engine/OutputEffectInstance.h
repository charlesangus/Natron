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

#ifndef Engine_OutputEffectInstance_h
#define Engine_OutputEffectInstance_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include <QMutex>

#include "Engine/EffectInstance.h"
#include "Engine/ViewIdx.h"
#include "Engine/EngineFwd.h"


class QThread;


NATRON_NAMESPACE_ENTER

class OutputEffectInstance
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

private:
    struct RenderSequenceArgs
    {
        BlockingBackgroundRender* renderController;
        std::vector<ViewIdx> viewsToRender;
        int firstFrame;
        int lastFrame;
        int frameStep;
        // The range the render was asked for, which differs from [firstFrame, lastFrame] for a
        // node that writesFrameRangeAtOnce().
        int requestedFirstFrame;
        int requestedLastFrame;
        bool useStats;
        bool blocking;
    };

    mutable QMutex _outputEffectDataLock;
    std::list<RenderSequenceArgs> _renderSequenceRequests;
    RenderEnginePtr _engine;

public:

    OutputEffectInstance(NodePtr node);

protected:
    OutputEffectInstance(const OutputEffectInstance& other);

public:
    virtual ~OutputEffectInstance();

    virtual bool isOutput() const OVERRIDE
    {
        return true;
    }

    RenderEnginePtr getRenderEngine() const
    {
        return _engine;
    }

    /**
     * @brief Returns true if a sequential render is being aborted
     **/
    bool isSequentialRenderBeingAborted() const;

    /**
     * @brief Is this node currently doing playback ?
     **/
    bool isDoingSequentialRender() const;

    /**
     * @brief Starts rendering of all the sequence available, from start to end.
     * This function is meant to be called for on-disk renderer only (i.e: not viewers).
     **/
    void renderFullSequence(bool isBlocking, bool enableRenderStats, BlockingBackgroundRender* renderController, int first, int last, int frameStep);

    void notifyRenderFinished();

    /**
     * @brief Returns true if a sequence render requested by anything other than renderController
     * is queued or running.
     **/
    bool hasRenderSequenceRequestOtherThan(const BlockingBackgroundRender* renderController) const;

    void renderCurrentFrame(bool canAbort);

    void renderCurrentFrameWithRenderStats(bool canAbort);

    bool ifInfiniteclipRectToProjectDefault(RectD* rod) const;

    /**
     * @brief Called before each frame and view of this node's own render to disk is rendered. A
     * writer that knows the frame cannot be written, including when its render action would never
     * be reached because it declares no plane to render, sets a persistent error and returns
     * eStatusFailed, which fails the render.
     **/
    virtual StatusEnum checkFrameToWrite(double /*time*/,
                                         ViewIdx /*view*/)
    {
        return eStatusOK;
    }

    /**
     * @brief Whether one render of this node writes its whole frame range at once, as a scene
     * export does, rather than one file or image per frame. Such a render runs a single frame,
     * the first of the range, on the main view only, and that frame calls writeFrameRange() with
     * the range the render was asked for instead of rendering the node.
     **/
    virtual bool writesFrameRangeAtOnce() const
    {
        return false;
    }

    /**
     * @brief For a node that writesFrameRangeAtOnce(): writes [firstFrame, lastFrame] for view.
     * Called once per render, from a render thread. On failure, sets a persistent error naming
     * the cause and returns eStatusFailed, which fails the render.
     **/
    virtual StatusEnum writeFrameRange(int /*firstFrame*/,
                                       int /*lastFrame*/,
                                       ViewIdx /*view*/)
    {
        return eStatusOK;
    }

    /**
     * @brief The frame range the running render of this node was asked for. Returns false when
     * no render of it is running.
     **/
    bool getRunningRenderRequestedRange(int* firstFrame,
                                        int* lastFrame) const;

    virtual void initializeData() OVERRIDE FINAL;
    virtual void reportStats(int time, ViewIdx view, double wallTime, const std::map<NodePtr, NodeRenderStats>& stats, const RenderStatsPtr& renderStats);

protected:

    void createWriterPath();

    void launchRenderSequence(const RenderSequenceArgs& args);

    /**
     * @brief Creates the engine that will control the output rendering
     **/
    virtual RenderEngine* createRenderEngine();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_OutputEffectInstance_h
