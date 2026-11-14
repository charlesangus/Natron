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
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Engine/ScenePayload.h"
#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER

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
    // The subtrees the edit looked at: nothing outside them changed, and no load outside them is
    // wanted or unwanted because of it.
    std::vector<std::string> roots;
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
// One row's edit in SceneDrawStates::editRows().
struct SceneDrawRowEdit {
    enum KindEnum {
        eKindSet,
        eKindClear,
        // SceneDrawStates::clearStatesBelow().
        eKindClearBelow
    };

    KindEnum kind;
    std::string path;
    // For eKindSet.
    SceneDrawStateEnum state;
};

class SceneDrawStates {
public:
    SceneDrawDelta setState(const std::string& path, SceneDrawStateEnum state);
    SceneDrawDelta clearState(const std::string& path);
    /** @brief Forgets every state set below the row, remembered ones too. The row's own stays. */
    SceneDrawDelta clearStatesBelow(const std::string& path);
    SceneDrawDelta clearAllStates();
    /**
     * @brief Makes the edits in order, as one edit with one delta: what an edit undoes of an earlier
     * one in the list is never in the delta, nor asked to load.
     **/
    SceneDrawDelta editRows(const std::vector<SceneDrawRowEdit>& edits);

    SceneDrawDelta setPolicyState(const std::string& path, SceneDrawStateEnum state);
    SceneDrawDelta clearPolicyState(const std::string& path);

    struct PathLess {
        bool operator()(const std::string& a, const std::string& b) const;
    };

    struct Policy {
        SceneDrawStateEnum state;
        // The node whose rule set it, by script name.
        std::string origin;

        bool operator==(const Policy& other) const
        {
            return (state == other.state) && (origin == other.origin);
        }

        bool operator!=(const Policy& other) const
        {
            return !(*this == other);
        }
    };

    // The graph policy by literal path, as SceneOps::matchDrawRules() resolves a payload's draw rules.
    typedef std::map<std::string, Policy, PathLess> PolicyStates;

    /**
     * @brief Puts policy in place of the whole graph policy, as one edit: only the subtrees of the
     * paths whose policy changed are looked at, and none for the same policy.
     **/
    SceneDrawDelta replacePolicyStates(const PolicyStates& policy);
    const PolicyStates& getPolicyStates() const
    {
        return _policy;
    }

    // The graph policy set on this very row, effective or only remembered.
    bool getPolicyState(const std::string& path, Policy* policy) const;

    /** @brief The state set on this very row by the viewer, effective or only remembered. */
    bool getSetState(const std::string& path, SceneDrawStateEnum* state) const;
    SceneDrawResolved resolve(const std::string& path) const;
    std::size_t storedCount() const;
    // Every load the rows ask for as they stand, in path order; visits every stored row.
    std::vector<SceneDrawLoadRequest> wantedLoads() const;
    /**
     * @brief The loads the rows at and below root ask for, in path order; visits only those rows. A
     * load asked for above root that covers it is not listed.
     **/
    std::vector<SceneDrawLoadRequest> wantedLoadsUnder(const std::string& root) const;

    struct Stored {
        SceneDrawStateEnum state;
        // False on an ancestor switched to Children for a row below: nobody asked to load that row.
        bool direct;
    };

    /** @brief Every state set by the viewer, effective or remembered, in path order. */
    std::vector<std::pair<std::string, Stored>> getSetStates() const;
    /** @brief Puts states in place of every state set by the viewer, as one edit. */
    SceneDrawDelta replaceSetStates(const std::vector<std::pair<std::string, Stored>>& states);
    /**
     * @brief Puts states in place of the viewer's at their paths, as one edit that looks only below
     * them: the states set at other paths stay.
     **/
    SceneDrawDelta mergeSetStates(const std::vector<std::pair<std::string, Stored>>& states);
    // Clears the viewer's states at paths, as one edit that looks only below them.
    SceneDrawDelta clearSetStates(const std::vector<std::string>& paths);

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
    // The subtree a row edit changes, given the states as they stand.
    std::string rootOf(const SceneDrawRowEdit& edit) const;
    void applyRowEdit(const SceneDrawRowEdit& edit);
    SceneDrawDelta applyEdit(const std::string& root, const std::vector<std::string>& retryLoads, const Effects& before, std::size_t visited) const;
    // Makes change, which edits only the subtrees of roots, and returns the delta over them.
    SceneDrawDelta editUnder(std::vector<std::string> roots, const std::vector<std::string>& retryLoads, const std::function<void()>& change);

    std::map<std::string, Stored, PathLess> _states;
    PolicyStates _policy;
};

/**
 * @brief Adds one prim a draw rule decides to *policy, over what earlier rules put there: a later
 * rule wins on the same path, and a later Full or Full + Load switches to Children the ancestors
 * that would block it, as setting it in the viewer does.
 **/
void sceneAddDrawRuleMatch(const std::string& path,
                           SceneDrawStateEnum state,
                           const std::string& origin,
                           SceneDrawStates::PolicyStates* policy);

// A stage's payload load rule on a path, as UsdStageLoadRules has them.
enum SceneLoadRuleEnum {
    eSceneLoadRuleAll,
    eSceneLoadRuleOnly,
    eSceneLoadRuleNone
};

typedef std::map<std::string, SceneLoadRuleEnum, SceneDrawStates::PathLess> SceneLoadRules;

/** @brief "all", "only" or "none". */
const char* sceneLoadRuleName(SceneLoadRuleEnum rule);
bool sceneLoadRuleFromName(const std::string& name, SceneLoadRuleEnum* rule);

/**
 * @brief The requests that bring a stage loading nothing to rules: a load with descendants for each
 * "all" rule, leaving out the subtrees of the rules nearest below it, and an own load for each "only"
 * rule. A "none" rule asks for nothing.
 */
std::vector<SceneDrawLoadRequest> sceneLoadRequestsFor(const SceneLoadRules& rules);

/** @brief The viewer's set states and its stage's load rules, as a project saves them. */
void sceneViewToSerialization(const std::vector<std::pair<std::string, SceneDrawStates::Stored>>& states,
                              const SceneLoadRules& rules,
                              Serialization::SceneViewSerialization* serialization);

/**
 * @brief Reads back what sceneViewToSerialization() wrote. A path that is not an absolute path, or
 * a state or rule name that is not one, is left out.
 */
void sceneViewFromSerialization(const Serialization::SceneViewSerialization& serialization,
                                std::vector<std::pair<std::string, SceneDrawStates::Stored>>* states,
                                SceneLoadRules* rules);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneDrawStates_h
