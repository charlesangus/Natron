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

#ifndef Engine_Nodes_Scene_SceneDrawStates_h
#define Engine_Nodes_Scene_SceneDrawStates_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

enum SceneDrawStateEnum {
    eSceneDrawStateBBox,
    eSceneDrawStateChildren,
    eSceneDrawStateFull,
    eSceneDrawStateLoadAll,
    eSceneDrawStateHidden
};

enum SceneDrawOriginEnum {
    eSceneDrawOriginViewer,
    eSceneDrawOriginGraphPolicy,
    eSceneDrawOriginInherited,
    eSceneDrawOriginDefault
};

/** @brief "bbox", "children", "full", "loadAll" or "hidden". */
const char* sceneDrawStateName(SceneDrawStateEnum state);
bool sceneDrawStateFromName(const std::string& name, SceneDrawStateEnum* state);

struct SceneDrawResolved {
    SceneDrawStateEnum state;
    SceneDrawOriginEnum origin;
    // An ancestor stops this row, so whatever it has set is only remembered.
    bool blocked;
};

struct SceneDrawLoadRequest {
    std::string path;
    bool withDescendants;
    // Subtrees a load with descendants leaves alone. They stay as they are: one already loaded is
    // not unloaded, and the rows drawn inside one come as requests of their own.
    std::vector<std::string> excluded;
};

/**
 * @brief What one edit changed, as rules over subtrees. A path's nearest listed ancestor decides it,
 * and no rule at all means drawn.
 *
 * The model knows set paths and not the hierarchy, so a Children row is its own list: the row's own
 * prim is drawn and every child not listed is a box, which only the scene can enumerate.
 */
struct SceneDrawDelta {
    std::vector<std::string> drawn;
    std::vector<std::string> expanded;
    std::vector<std::string> boxed;
    std::vector<std::string> hidden;
    // The rule on these paths is gone; the nearest rule above now decides them.
    std::vector<std::string> cleared;
    std::vector<SceneDrawLoadRequest> loads;
    // Stored states this edit looked at, which bounds what it cost.
    std::size_t visited = 0;

    bool isEmpty() const
    {
        return drawn.empty() && expanded.empty() && boxed.empty() && hidden.empty() && cleared.empty() && loads.empty();
    }
};

/**
 * @brief One viewer's draw states by prim path. Paths are absolute USD prim paths and need not exist
 * at the viewed node: a missing path is kept, so returning to a node with it restores the state.
 *
 * Resolution is the viewer's state, then the graph policy, then Full. The nearest set state wins. A
 * Children row's unset children are boxes. BBox and Hidden stop everything below, and what is set
 * below is remembered.
 *
 * Load All loads what it draws. A BBox or Hidden row below it stays out of the load, and so does a
 * Children row's subtree, whose unset children are boxes the model cannot name: the Children row
 * loads on its own and the rows drawn below it load by their own requests.
 */
class SceneDrawStates {
public:
    SceneDrawDelta setState(const std::string& path, SceneDrawStateEnum state);
    SceneDrawDelta clearState(const std::string& path);
    /** @brief Forgets every state set below the row, remembered ones too. The row's own stays. */
    SceneDrawDelta clearStatesBelow(const std::string& path);
    SceneDrawDelta clearAllStates();

    SceneDrawDelta setPolicyState(const std::string& path, SceneDrawStateEnum state);
    SceneDrawDelta clearPolicyState(const std::string& path);

    /** @brief The state set on this very row by the viewer, effective or only remembered. */
    bool getSetState(const std::string& path, SceneDrawStateEnum* state) const;
    SceneDrawResolved resolve(const std::string& path) const;
    std::size_t storedCount() const;
    // Every load the rows ask for as they stand, in path order; visits every stored row.
    std::vector<SceneDrawLoadRequest> wantedLoads() const;

    struct PathLess {
        bool operator()(const std::string& a, const std::string& b) const;
    };

    struct Stored {
        SceneDrawStateEnum state;
        // False on an ancestor switched to Children for a row below: nobody asked to load that row.
        bool direct;
    };

    enum RuleEnum {
        eRuleNone,
        eRuleDrawn,
        eRuleExpanded,
        eRuleBoxed,
        eRuleHidden
    };

    enum LoadEnum {
        eLoadNone,
        eLoadOwn,
        eLoadWithDescendants
    };

    struct Effect {
        std::string path;
        RuleEnum rule;
        LoadEnum load;
        std::vector<std::string> excluded;
    };

    struct Effects {
        std::vector<Effect> rows;
        // The root's subtree under a load with descendants asked for above it, and the rows below the
        // root which that load leaves out.
        bool rootCovered = false;
        std::vector<std::string> excludedBelowRoot;
    };

private:
    Effects effectsUnder(const std::string& root, std::size_t* visited) const;
    SceneDrawDelta applyEdit(const std::string& root, const std::string& retryLoad, const Effects& before, std::size_t visited) const;

    std::map<std::string, Stored, PathLess> _states;
    std::map<std::string, SceneDrawStateEnum, PathLess> _policy;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneDrawStates_h
