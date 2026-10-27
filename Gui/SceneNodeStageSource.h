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

#include <QtCore/QObject>
#include <QtCore/QString>

#include "Engine/EngineFwd.h"
#include "Gui/SceneGraphModel.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A node's scene, evaluated on a worker into a stage of its own. A stage leased from the
 * query pool would be bound to the thread that leased it, and a view may hold this one for as
 * long as it shows the node.
 **/
class SceneNodeStageSource
    : public SceneGraphStageSource,
      public std::enable_shared_from_this<SceneNodeStageSource> {
public:
    // Called on context's thread after each evaluation.
    typedef std::function<void(bool ok, const QString& error)> SyncedFunc;

    SceneNodeStageSource(const NodePtr& node,
                         QObject* context,
                         const SyncedFunc& synced);

    ~SceneNodeStageSource() override;

    void stop();

    void readStage(const std::function<void(const SceneStage* stage)>& read) const override;

    U64 getEditCount() const override;

    NodePtr getNode() const;

    // Evaluates the node again at time.
    void request(double time);

private:
    void run();

    NodeWPtr _node;
    QObject* _context;
    SyncedFunc _synced;
    mutable std::mutex _stageMutex;
    std::unique_ptr<SceneStage> _stage;
    std::atomic<bool> _shown { false };
    std::atomic<U64> _editCount { 0 };
    std::mutex _queueMutex;
    std::condition_variable _queueChanged;
    bool _pending = false;
    bool _quit = false;
    double _pendingTime = 0.;
    std::thread _worker;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneNodeStageSource_h
