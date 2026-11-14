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

#ifndef Gui_SceneNodeStageSource_h
#define Gui_SceneNodeStageSource_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/SceneLoadSet.h"
#include "Gui/SceneGraphModel.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A node's scene, evaluated on a worker into a stage of its own. A stage leased from the
 * query pool would be bound to the thread that leased it, and a view may hold this one for as
 * long as it shows the node. Without loadPayloads the stage opens with no payload loaded, and
 * requestLoad() loads them from the same worker, which hands the files to a thread of their own,
 * opened with no lock held and given up on once the source is stopped, as the viewer's stage
 * does. The node is evaluated in a context of the source's own, with the payloads
 * its stage includes, so that the pattern nodes upstream load no more than the stage shows; a
 * batch of loads or unloads commits along with the node evaluated again for the new set.
 **/
class SceneNodeStageSource
    : public SceneGraphStageSource,
      public std::enable_shared_from_this<SceneNodeStageSource> {
public:
    // Called on context's thread after each evaluation.
    typedef std::function<void(bool ok, const QString& error)> SyncedFunc;

    SceneNodeStageSource(const NodePtr& node,
                         QObject* context,
                         const SyncedFunc& synced,
                         bool loadPayloads = true);

    ~SceneNodeStageSource() override;

    // Asks the worker to quit, aborting the evaluation it is running, without waiting for it.
    // Nothing is called on context from then on.
    void requestStop();

    // Whether the worker has quit, so destroying the source waits for nothing.
    bool isFinished() const;

    void stop();

    void readStage(const std::function<void(const SceneStage* stage)>& read) const override;

    bool tryReadStage(const std::function<void(const SceneStage* stage)>& read) const override;

    U64 getEditCount() const override;

    NodePtr getNode() const;

    // Evaluates the node again at time.
    void request(double time);

    // How many evaluations reached the stage. An evaluation still running when stop is asked for
    // is dropped.
    U64 getComposeCount() const;

    // Called on context's thread after each batch of payload loads or unloads.
    void setLoadsChanged(const std::function<void()>& loadsChanged);

    // Loads the payloads of paths, each prim's own or also those below it, on the worker.
    void requestLoad(const QStringList& paths,
                     bool withDescendants);

    void requestUnload(const QStringList& paths);

    /**
     * @brief Loads what a viewer's load rules load, as one batch: one setLoadsChanged() call
     * follows it, or none if the rules load nothing. A subtree a rule leaves out of its load
     * starts unloaded.
     **/
    void requestLoadRules(const SceneLoadRules& rules);

    // Stops sources on a thread of their own, so that whoever lets them go waits for no worker.
    static void reap(const std::vector<std::shared_ptr<SceneNodeStageSource>>& sources);

    // Waits until every source handed to reap() has stopped. For the app's shutdown.
    static void joinReaped();

private:
    struct LoadRequest {
        QStringList paths;
        bool load = true;
        bool withDescendants = false;
    };

    void run();

    void serviceLoads(const std::vector<LoadRequest>& requests);

    void postToContext(const std::function<void()>& call);

    NodeWPtr _node;
    // Null once stop is asked for, so that a worker posting then cannot reach a destroyed context.
    QObject* _context;
    std::mutex _contextMutex;
    const AbortableRenderInfoPtr _abortInfo;
    SyncedFunc _synced;
    const bool _loadPayloads;
    const U64 _contextId;
    // Worker only: the set the stage includes, and the time of the last evaluation.
    SceneLoadSetPtr _loadSet;
    double _time = 0.;
    mutable std::mutex _stageMutex;
    std::unique_ptr<SceneStage> _stage;
    std::atomic<bool> _shown { false };
    std::atomic<U64> _editCount { 0 };
    std::atomic<U64> _composeCount { 0 };
    std::function<void()> _loadsChanged;
    std::atomic<bool> _stopRequested { false };
    std::atomic<bool> _finished { false };
    std::mutex _queueMutex;
    std::condition_variable _queueChanged;
    bool _pending = false;
    bool _quit = false;
    double _pendingTime = 0.;
    std::vector<LoadRequest> _loadRequests;
    std::thread _worker;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneNodeStageSource_h
