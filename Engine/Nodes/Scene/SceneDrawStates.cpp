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

#include "Engine/Nodes/Scene/SceneDrawStates.h"

#include <algorithm>

NATRON_NAMESPACE_ENTER

namespace {
using StateMap = std::map<std::string, SceneDrawStates::Stored, SceneDrawStates::PathLess>;
using PolicyMap = std::map<std::string, SceneDrawStateEnum, SceneDrawStates::PathLess>;

enum FrameKindEnum {
    eFrameDrawn,
    eFrameExpanded,
    eFrameBlocked
};

struct Frame {
    std::string path;
    FrameKindEnum kind;
    // A Full + Load above asks for every payload here already.
    bool loading;
};

bool
isBelow(const std::string& path,
        const std::string& ancestor)
{
    if (ancestor == "/") {
        return path.size() > 1;
    }

    return (path.size() > ancestor.size()) && (path[ancestor.size()] == '/') && (path.compare(0, ancestor.size(), ancestor) == 0);
}

std::string
parentOf(const std::string& path)
{
    if (path == "/") {
        return std::string();
    }
    const std::size_t slash = path.rfind('/');

    return (slash == 0) ? std::string("/") : path.substr(0, slash);
}

std::vector<std::string>
ancestorsOf(const std::string& path)
{
    std::vector<std::string> ancestors;
    if (path == "/") {
        return ancestors;
    }
    ancestors.push_back("/");
    for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        ancestors.push_back(path.substr(0, slash));
    }

    return ancestors;
}

// Sorts after everything below `path` and before its next sibling: PathLess ranks '/' under every
// other character.
std::string
pastSubtree(const std::string& path)
{
    return path + '\x01';
}

std::string
childTowards(const std::string& ancestor,
             const std::string& path)
{
    const std::size_t start = (ancestor == "/") ? 1 : (ancestor.size() + 1);
    const std::size_t slash = path.find('/', start);

    return (slash == std::string::npos) ? path : path.substr(0, slash);
}

bool
stops(SceneDrawStateEnum state)
{
    return (state == eSceneDrawStateBBox) || (state == eSceneDrawStateHidden);
}

bool
lookUp(const StateMap& states,
       const PolicyMap& policy,
       const std::string& path,
       SceneDrawStateEnum* state)
{
    const StateMap::const_iterator set = states.find(path);
    if (set != states.end()) {
        *state = set->second.state;

        return true;
    }
    const PolicyMap::const_iterator policed = policy.find(path);
    if (policed != policy.end()) {
        *state = policed->second;

        return true;
    }

    return false;
}

// The frame a row's children live in, or for an unset row (`found` false) the frame it passes on.
Frame
descend(const Frame& parent,
        const std::string& path,
        bool found,
        SceneDrawStateEnum state)
{
    if (parent.kind == eFrameBlocked) {
        return parent;
    }
    Frame frame { path, parent.kind, parent.loading };
    if (!found) {
        if (parent.kind == eFrameExpanded) {
            frame.kind = eFrameBlocked;
        }

        return frame;
    }
    switch (state) {
    case eSceneDrawStateBBox:
    case eSceneDrawStateHidden:
        frame.kind = eFrameBlocked;
        break;
    case eSceneDrawStateChildren:
        frame.kind = eFrameExpanded;
        break;
    case eSceneDrawStateFull:
        frame.kind = eFrameDrawn;
        break;
    case eSceneDrawStateFullLoad:
        frame.kind = eFrameDrawn;
        frame.loading = true;
        break;
    }

    return frame;
}

void
appendRule(SceneDrawDelta* delta,
           const SceneDrawStates::Effect& effect)
{
    switch (effect.rule) {
    case SceneDrawStates::eRuleNone:
        delta->cleared.push_back(effect.path);
        break;
    case SceneDrawStates::eRuleDrawn:
        delta->drawn.push_back(effect.path);
        break;
    case SceneDrawStates::eRuleExpanded:
        delta->expanded.push_back(effect.path);
        break;
    case SceneDrawStates::eRuleBoxed:
        delta->boxed.push_back(effect.path);
        break;
    case SceneDrawStates::eRuleHidden:
        delta->hidden.push_back(effect.path);
        break;
    }
}

void
appendLoad(SceneDrawDelta* delta,
           const SceneDrawStates::Effect& effect)
{
    if (effect.load != SceneDrawStates::eLoadNone) {
        delta->loads.push_back(SceneDrawLoadRequest { effect.path, effect.load == SceneDrawStates::eLoadWithDescendants });
    }
}
} // anon namespace

const char*
sceneDrawStateName(SceneDrawStateEnum state)
{
    switch (state) {
    case eSceneDrawStateBBox:
        return "bbox";
    case eSceneDrawStateChildren:
        return "children";
    case eSceneDrawStateFull:
        return "full";
    case eSceneDrawStateFullLoad:
        return "fullLoad";
    case eSceneDrawStateHidden:
        return "hidden";
    }

    return "";
}

bool
sceneDrawStateFromName(const std::string& name,
                       SceneDrawStateEnum* state)
{
    for (SceneDrawStateEnum candidate : { eSceneDrawStateBBox, eSceneDrawStateChildren, eSceneDrawStateFull, eSceneDrawStateFullLoad, eSceneDrawStateHidden }) {
        if (name == sceneDrawStateName(candidate)) {
            *state = candidate;

            return true;
        }
    }

    return false;
}

// Keeps a subtree contiguous, right after its root: in plain byte order "/a.b" or "/a-b" would land
// between "/a" and "/a/b". Walking a subtree then costs its own size and no more.
bool
SceneDrawStates::PathLess::operator()(const std::string& a,
                                      const std::string& b) const
{
    const std::size_t common = (std::min)(a.size(), b.size());
    for (std::size_t i = 0; i < common; ++i) {
        const unsigned char ca = (a[i] == '/') ? 0 : static_cast<unsigned char>(a[i]);
        const unsigned char cb = (b[i] == '/') ? 0 : static_cast<unsigned char>(b[i]);
        if (ca != cb) {
            return ca < cb;
        }
    }

    return a.size() < b.size();
}

std::vector<SceneDrawStates::Effect>
SceneDrawStates::effectsUnder(const std::string& root,
                              std::size_t* visited) const
{
    std::vector<Effect> effects;
    Frame base { std::string(), eFrameDrawn, false };
    for (const std::string& ancestor : ancestorsOf(root)) {
        SceneDrawStateEnum state = eSceneDrawStateFull;
        const bool found = lookUp(_states, _policy, ancestor, &state);
        base = descend(base, ancestor, found, state);
    }
    if (base.kind == eFrameBlocked) {
        return effects;
    }
    base.path = parentOf(root);

    StateMap::const_iterator set = _states.lower_bound(root);
    PolicyMap::const_iterator policed = _policy.lower_bound(root);
    const PathLess less;
    std::vector<Frame> stack(1, base);
    for (;;) {
        const bool hasSet = (set != _states.end()) && ((set->first == root) || isBelow(set->first, root));
        const bool hasPolicy = (policed != _policy.end()) && ((policed->first == root) || isBelow(policed->first, root));
        if (!hasSet && !hasPolicy) {
            break;
        }
        const bool takeSet = hasSet && (!hasPolicy || !less(policed->first, set->first));
        const std::string path = takeSet ? set->first : policed->first;
        const SceneDrawStateEnum state = takeSet ? set->second.state : policed->second;
        const bool direct = takeSet && set->second.direct;
        ++*visited;

        while ((stack.size() > 1) && !isBelow(path, stack.back().path)) {
            stack.pop_back();
        }
        const Frame parent = stack.back();
        std::string skip;
        if ((parent.kind == eFrameExpanded) && (parentOf(path) != parent.path)) {
            // An unset child of a Children row is a box, which stops this row and its neighbours.
            skip = pastSubtree(childTowards(parent.path, path));
        } else {
            Effect effect { path, eRuleNone, eLoadNone };
            switch (state) {
            case eSceneDrawStateBBox:
                // A box is what a Children row's child is already.
                effect.rule = (parent.kind == eFrameExpanded) ? eRuleNone : eRuleBoxed;
                break;
            case eSceneDrawStateHidden:
                effect.rule = eRuleHidden;
                break;
            case eSceneDrawStateChildren:
                effect.rule = eRuleExpanded;
                effect.load = (direct && !parent.loading) ? eLoadOwn : eLoadNone;
                break;
            case eSceneDrawStateFull:
                effect.rule = (parent.kind == eFrameDrawn) ? eRuleNone : eRuleDrawn;
                break;
            case eSceneDrawStateFullLoad:
                effect.rule = (parent.kind == eFrameDrawn) ? eRuleNone : eRuleDrawn;
                effect.load = parent.loading ? eLoadNone : eLoadWithDescendants;
                break;
            }
            if ((effect.rule != eRuleNone) || (effect.load != eLoadNone)) {
                effects.push_back(effect);
            }
            if (stops(state)) {
                skip = pastSubtree(path);
            } else {
                stack.push_back(descend(parent, path, true, state));
            }
        }

        if (skip.empty()) {
            if (takeSet) {
                ++set;
            }
            if (hasPolicy && (!takeSet || (policed->first == path))) {
                ++policed;
            }
        } else if (path == "/") {
            break;
        } else {
            set = _states.lower_bound(skip);
            policed = _policy.lower_bound(skip);
        }
    }

    return effects;
}

SceneDrawDelta
SceneDrawStates::applyEdit(const std::string& root,
                           const std::string& retryLoad,
                           const std::vector<Effect>& before,
                           std::size_t visited) const
{
    SceneDrawDelta delta;
    delta.visited = visited;
    const std::vector<Effect> after = effectsUnder(root, &delta.visited);
    const PathLess less;
    std::size_t b = 0;
    std::size_t a = 0;
    while ((b < before.size()) || (a < after.size())) {
        if ((a == after.size()) || ((b < before.size()) && less(before[b].path, after[a].path))) {
            if (before[b].rule != eRuleNone) {
                delta.cleared.push_back(before[b].path);
            }
            ++b;
        } else if ((b == before.size()) || less(after[a].path, before[b].path)) {
            if (after[a].rule != eRuleNone) {
                appendRule(&delta, after[a]);
            }
            appendLoad(&delta, after[a]);
            ++a;
        } else {
            if (before[b].rule != after[a].rule) {
                appendRule(&delta, after[a]);
            }
            // Setting the row again asks again: an earlier load of it may have been cancelled.
            if ((before[b].load != after[a].load) || (after[a].path == retryLoad)) {
                appendLoad(&delta, after[a]);
            }
            ++b;
            ++a;
        }
    }

    return delta;
}

SceneDrawDelta
SceneDrawStates::setState(const std::string& path,
                          SceneDrawStateEnum state)
{
    std::string root = path;
    std::vector<std::string> switched;
    if ((state == eSceneDrawStateFull) || (state == eSceneDrawStateFullLoad)) {
        Frame frame { std::string(), eFrameDrawn, false };
        for (const std::string& ancestor : ancestorsOf(path)) {
            SceneDrawStateEnum ancestorState = eSceneDrawStateFull;
            const bool found = lookUp(_states, _policy, ancestor, &ancestorState);
            if (found ? stops(ancestorState) : (frame.kind == eFrameExpanded)) {
                if (switched.empty()) {
                    root = ancestor;
                }
                switched.push_back(ancestor);
                ancestorState = eSceneDrawStateChildren;
                frame = descend(frame, ancestor, true, ancestorState);
            } else {
                frame = descend(frame, ancestor, found, ancestorState);
            }
        }
    }
    std::size_t visited = 0;
    const std::vector<Effect> before = effectsUnder(root, &visited);
    for (const std::string& ancestor : switched) {
        _states[ancestor] = Stored { eSceneDrawStateChildren, false };
    }
    _states[path] = Stored { state, true };

    return applyEdit(root, path, before, visited);
}

SceneDrawDelta
SceneDrawStates::clearState(const std::string& path)
{
    std::size_t visited = 0;
    const std::vector<Effect> before = effectsUnder(path, &visited);
    _states.erase(path);

    return applyEdit(path, std::string(), before, visited);
}

SceneDrawDelta
SceneDrawStates::clearAllStates()
{
    std::size_t visited = 0;
    const std::vector<Effect> before = effectsUnder("/", &visited);
    _states.clear();

    return applyEdit("/", std::string(), before, visited);
}

SceneDrawDelta
SceneDrawStates::setPolicyState(const std::string& path,
                                SceneDrawStateEnum state)
{
    std::size_t visited = 0;
    const std::vector<Effect> before = effectsUnder(path, &visited);
    _policy[path] = state;

    return applyEdit(path, std::string(), before, visited);
}

SceneDrawDelta
SceneDrawStates::clearPolicyState(const std::string& path)
{
    std::size_t visited = 0;
    const std::vector<Effect> before = effectsUnder(path, &visited);
    _policy.erase(path);

    return applyEdit(path, std::string(), before, visited);
}

bool
SceneDrawStates::getSetState(const std::string& path,
                             SceneDrawStateEnum* state) const
{
    const StateMap::const_iterator it = _states.find(path);
    if (it == _states.end()) {
        return false;
    }
    *state = it->second.state;

    return true;
}

SceneDrawResolved
SceneDrawStates::resolve(const std::string& path) const
{
    SceneDrawResolved resolved { eSceneDrawStateFull, eSceneDrawOriginDefault, false };
    Frame frame { std::string(), eFrameDrawn, false };
    std::vector<std::string> levels = ancestorsOf(path);
    levels.push_back(path);
    for (const std::string& level : levels) {
        if (frame.kind == eFrameBlocked) {
            resolved.origin = eSceneDrawOriginInherited;
            resolved.blocked = true;

            return resolved;
        }
        const StateMap::const_iterator set = _states.find(level);
        const PolicyMap::const_iterator policed = _policy.find(level);
        const bool found = (set != _states.end()) || (policed != _policy.end());
        if (found) {
            resolved.state = (set != _states.end()) ? set->second.state : policed->second;
            resolved.origin = (set != _states.end()) ? eSceneDrawOriginViewer : eSceneDrawOriginGraphPolicy;
        } else if (frame.kind == eFrameExpanded) {
            resolved.state = eSceneDrawStateBBox;
            resolved.origin = eSceneDrawOriginInherited;
        } else if (resolved.origin != eSceneDrawOriginDefault) {
            resolved.origin = eSceneDrawOriginInherited;
        }
        frame = descend(frame, level, found, resolved.state);
    }

    return resolved;
}

std::size_t
SceneDrawStates::storedCount() const
{
    return _states.size();
}

NATRON_NAMESPACE_EXIT
