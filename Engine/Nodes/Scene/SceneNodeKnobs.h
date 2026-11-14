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

#ifndef Engine_Nodes_Scene_SceneNodeKnobs_h
#define Engine_Nodes_Scene_SceneNodeKnobs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QString>

#include "Engine/ChoiceOption.h"
#include "Engine/EngineFwd.h"
#include "Engine/SceneLoadSet.h"
#include "Global/Enums.h"

// Free of USD, like the node headers that include it: the knobs the scene create nodes share.
// Their authoring is in SceneNodeAuthoring.h.

#define kSceneParamPrimPath "primPath"
#define kSceneParamTranslate "translate"
#define kSceneParamRotate "rotate"
#define kSceneParamScale "scale"
#define kSceneParamUniformScale "uniformScale"
#define kSceneParamRotateOrder "rotateOrder"
// Neither a prim path nor a variant name can hold '<'.
#define kSceneMenuUnsearchedId "<unsearched>"

NATRON_NAMESPACE_ENTER

/**
 * @brief The order of the rotate knob's three rotations, named as USD names its xformOp:rotate
 * ops: XYZ turns about X first, then Y, then Z.
 **/
enum SceneRotateOrderEnum {
    eSceneRotateOrderXYZ = 0,
    eSceneRotateOrderXZY,
    eSceneRotateOrderYXZ,
    eSceneRotateOrderYZX,
    eSceneRotateOrderZXY,
    eSceneRotateOrderZYX
};

/**
 * @brief A prim's local transform as translate, rotate (degrees, in the rotate order) and scale
 * knobs, authored as the prim's xformOp:translate, xformOp:rotate<order> and xformOp:scale, in
 * that order, so the prim is scaled, then turned, then moved.
 **/
struct SceneTransformKnobs {
    KnobDoubleWPtr translate;
    KnobDoubleWPtr rotate;
    KnobDoubleWPtr scale;
    KnobDoubleWPtr uniformScale;
    KnobChoiceWPtr rotateOrder;

    void create(KnobHolder* holder,
                const KnobPagePtr& page);

    // Hides and disables the knobs, for a prim that has no transform.
    void setSecret(bool secret) const;
};

/**
 * @brief The non-animatable string knob naming the prim a node authors, an absolute prim path.
 **/
KnobStringPtr createScenePrimPathKnob(KnobHolder* holder,
                                      const KnobPagePtr& page,
                                      const std::string& defaultPath,
                                      const QString& toolTip);

/**
 * @brief The prim path a node authors by default: "/" followed by its script name, so each
 * node's prim is distinct and named after it (/USDCamera1). fallbackName stands in for a node
 * that has no script name yet.
 **/
std::string getSceneNodeDefaultPrimPath(const EffectInstance* effect,
                                        const std::string& fallbackName);

/**
 * @brief Points knob's default at newDefaultPath after the node is renamed, and its value too
 * when the value is still the old default; a path the user typed stays.
 **/
void followSceneNodeRename(const KnobStringPtr& knob,
                           const std::string& newDefaultPath);

/**
 * @brief The camera a node reads, by path. The path knob decides; the menu beside it lists the
 * cameras the node can see and, picked by the user, writes the path of its pick. A path no camera
 * has yet is kept and listed as missing, for a camera that arrives upstream later. An empty path
 * stands for the first camera.
 **/
struct SceneCameraPathKnobs {
    KnobChoiceWPtr menu;
    KnobStringWPtr path;

    void create(KnobHolder* holder,
                const KnobPagePtr& page,
                const char* menuName,
                const char* pathName,
                const QString& menuToolTip,
                const QString& pathToolTip);

    std::string getPath() const;

    /**
     * @brief Lists cameras, then the path when none of them has it, and selects the path's entry.
     * A node that reads its camera on the stage it renders in (readDownstream) lists the path as
     * itself, not as missing. unsearched unloaded branches, when there are some, end the menu
     * with makeSceneUnsearchedOption().
     **/
    void refreshMenu(const std::vector<std::string>& cameras,
                     bool readDownstream = false,
                     std::size_t unsearched = 0,
                     bool noViewer = false) const;

    // True when k is one of the knobs, with *menuStale set when the menu must be refreshed.
    bool knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason,
                     bool* menuStale) const;
};

/**
 * @brief The row a menu filled from a scene ends with when unsearched unloaded branches may hold
 * more entries: "N unloaded branches not searched". It names no entry, so it is disabled and the
 * GUI never lets it be picked. noViewer says no viewer shows the node, so nothing inside a payload
 * was searched.
 **/
ChoiceOption makeSceneUnsearchedOption(std::size_t unsearched,
                                       bool noViewer);

// Whether id is that of makeSceneUnsearchedOption().
bool isSceneUnsearchedOption(const std::string& id);

/**
 * @brief One thread that runs a node's scene jobs one at a time, started on the first post().
 * It holds the running job and at most one waiting: a post() replaces the waiting one, which is
 * then dropped. stop() drops the waiting job and lets the thread end once the running one
 * returns, without waiting for it: the owner cancels that one first, and its jobs reach the owner
 * through weak references only. joinAll() waits for every such thread at application shutdown.
 **/
class SceneWorker {
public:
    typedef std::function<void()> Task;

    SceneWorker();

    // Calls stop().
    ~SceneWorker();

    SceneWorker(const SceneWorker&) = delete;
    SceneWorker& operator=(const SceneWorker&) = delete;

    /**
     * @brief Queues run. dropped is called instead when run is replaced before it starts or the
     * worker stops first, so whoever waits on the job is released. After stop(), dropped is
     * called at once.
     **/
    void post(const Task& run,
              const Task& dropped);

    // Never blocks. Idempotent.
    void stop();

    /**
     * @brief Stops every worker and waits, with the Python GIL released, up to timeoutMs for
     * their threads to end. After it, post() drops its job at once. Called at application
     * shutdown, before the statics the jobs read are torn down. False if a thread still runs.
     **/
    static bool joinAll(int timeoutMs);

    struct Implementation;

private:
    // Shared with the thread, which outlives the worker once stopped.
    std::shared_ptr<Implementation> _imp;
};

/**
 * @brief One refresh of a menu filled from a scene, run on a worker: the names it found, list by
 * list as the node fills its menus, and how many unloaded branches it did not search.
 **/
class SceneMenuJob {
public:
    struct Result {
        std::vector<std::vector<std::string>> lists;
        std::size_t unsearched = 0;
    };

    explicit SceneMenuJob(bool noViewer);

    // Stops the job: its scene evaluation aborts and its result is never applied.
    void cancel();

    bool isCancelled() const;

    // Aborted by cancel(), for the job's scene evaluation.
    const AbortableRenderInfoPtr& getAbortInfo() const
    {
        return _abortInfo;
    }

    // Whether no viewer showed the node, so the job searched with nothing loaded.
    bool hasNoViewer() const
    {
        return _noViewer;
    }

    // Blocks until the worker is done.
    void wait() const;

    bool isDone() const;

    Result getResult() const;

    // Ends the job with result; for the worker.
    void finish(const Result& result);

private:
    const bool _noViewer;
    AbortableRenderInfoPtr _abortInfo;
    mutable std::mutex _lock;
    mutable std::condition_variable _done;
    bool _finished = false;
    Result _result;
};

/**
 * @brief A node's menu refreshes. Each runs on a worker, so the main thread never evaluates the
 * scene for a menu, and is applied on the main thread when it ends; until then the menu keeps its
 * entries. A refresh supersedes the one before it, whose result is then never applied. A load
 * change in a viewer runs the refresh again when it changes what the last one searched
 * (followLoadChanges()).
 **/
class SceneMenuRefresh {
public:
    typedef std::function<void(const SceneMenuJob& job, SceneMenuJob::Result* result)> WorkFunc;
    typedef std::function<void(const std::shared_ptr<SceneMenuJob>& job)> ApplyFunc;
    typedef std::function<std::shared_ptr<SceneMenuJob>()> RerunFunc;

    SceneMenuRefresh();

    ~SceneMenuRefresh();

    SceneMenuRefresh(const SceneMenuRefresh&) = delete;
    SceneMenuRefresh& operator=(const SceneMenuRefresh&) = delete;

    /**
     * @brief The node's own refresh, which starts a refresh as the node does for any other
     * reason. followLoadChanges() calls it. Main thread.
     **/
    void setRerun(const RerunFunc& rerun);

    /**
     * @brief The context a refresh of effect's menus searches in: that of the viewer showing
     * effect (SceneNodeAuthoring::getShowingViewerContext()), else nothing loaded, with
     * *noViewer set; a set the graph maps away from the viewer's own gets getMappedContextId().
     * It has a reach of its own, which gathers what the search depends on. Main thread.
     **/
    SceneEvaluationContext getContext(EffectInstance* effect,
                                      bool* noViewer) const;

    // The id for an input that a refresh evaluates on another load set than getContext()'s.
    U64 getMappedContextId() const;

    /**
     * @brief Cancels the refresh still running and runs work, which searches on context, on a
     * worker. When work ends, apply is queued to the main thread with the job, while effect and
     * this object live; it fills the menus when take() accepts the job. context's load set and
     * reach are kept, for followLoadChanges() to tell whether a load changes what work found:
     * work adds to the reach every prefix its result depends on. Called from followLoadChanges(),
     * it starts nothing, returning null, when the load change does not reach what the last refresh
     * searched. Main thread.
     **/
    std::shared_ptr<SceneMenuJob> start(EffectInstance* effect,
                                        const SceneEvaluationContext& context,
                                        bool noViewer,
                                        const WorkFunc& work,
                                        const ApplyFunc& apply);

    /**
     * @brief Whether job is the latest refresh, done and not cancelled: true once per job, for
     * the call that fills the menus with its result. Main thread.
     **/
    bool take(const std::shared_ptr<SceneMenuJob>& job);

    /**
     * @brief Runs a menu's refresh again (setRerun()) when the showing viewer's load set differs
     * from the one its last refresh used on a payload in that refresh's reach, or when a viewer
     * starts or stops showing its node. Returns the jobs started. Main thread.
     **/
    static std::vector<std::shared_ptr<SceneMenuJob>> followLoadChanges();

    // Queues followLoadChanges() to the main thread, once for any number of calls before it runs. Any thread.
    static void scheduleFollowLoadChanges();

    // Defined where the refreshes are, which keep a registry of them.
    struct State;

private:
    std::shared_ptr<State> _state;
};

// Hides and disables knob, or shows and enables it.
void setSceneKnobSecret(const KnobIPtr& knob,
                        bool secret);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneNodeKnobs_h
