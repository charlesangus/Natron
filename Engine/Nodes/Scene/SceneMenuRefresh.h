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

#ifndef Engine_Nodes_Scene_SceneMenuRefresh_h
#define Engine_Nodes_Scene_SceneMenuRefresh_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>

#include "Engine/EngineFwd.h"
#include "Global/GlobalDefines.h"

class QObject;

NATRON_NAMESPACE_ENTER

class ScenePayload;

/**
 * @brief Refreshes a node's menus from its input scene without evaluating that scene on the main
 * thread, where an upstream edit would otherwise stall the GUI for as long as the scene takes.
 * Free of pxr types, so node headers can hold one.
 *
 * request() is called on the main thread. The Read it prepares runs on a pool thread, and the
 * Apply that returns runs back on the main thread. At most one Read per node runs at a time: a
 * request made while one runs supersedes it, so that Read's Apply is dropped and the newest
 * request's Read starts once it ends.
 **/
class SceneMenuRefresh {
public:
    // Main thread, only while the context given to request() lives.
    typedef std::function<void()> Apply;
    // A pool thread; it owns what it reads, captured by the prepare function on the main thread.
    typedef std::function<Apply()> Read;
    typedef std::function<Read()> Prepare;

    SceneMenuRefresh();

    ~SceneMenuRefresh();

    /**
     * @brief Main thread. Calls prepare now, or once the running Read ends, and runs its Read on
     * a pool thread. Nothing more runs once context is destroyed. Without a Qt application, no
     * event loop could deliver the Apply, so both run on the calling thread before this returns.
     **/
    void request(QObject* context, const Prepare& prepare);

    // Main thread. Whether a Read runs or waits for one to end.
    bool isBusy() const;

    // How many Applies ran, dropped ones excluded. Main thread.
    U64 getApplyCount() const;

    /**
     * @brief The scene input gives at time and view 0, empty when input is null, its node is
     * deactivated or its project closes, or the evaluation fails. Any thread.
     **/
    static ScenePayload evaluateInput(const EffectInstancePtr& input, double time);

    struct State;

private:
    SceneMenuRefresh(const SceneMenuRefresh&) = delete;
    SceneMenuRefresh& operator=(const SceneMenuRefresh&) = delete;

    std::shared_ptr<State> _state;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneMenuRefresh_h
