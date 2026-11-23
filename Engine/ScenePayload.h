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

#ifndef NATRON_ENGINE_SCENEPAYLOAD_H
#define NATRON_ENGINE_SCENEPAYLOAD_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

enum SceneDrawStateEnum {
    eSceneDrawStateBBox,
    eSceneDrawStateChildren,
    eSceneDrawStateFull,
    eSceneDrawStateLoadAll,
    eSceneDrawStateHidden
};

/**
 * @brief A draw state the node graph asks of the viewers below it, for the prims a pattern names.
 * Viewers resolve it against the prims they have loaded; renders and writes never read it.
 *
 * pattern is a prim path expression as a PrimSelector holds it. A union of plain paths needs no
 * stage, so it applies to prims inside unloaded payloads too. With levels and toKind unset, each
 * prim matched takes state. With levels set, each prim matched and the prims down to levels - 1
 * below it are Children and the prims levels below it take state. With toKind set, the prims
 * from each match down to the first prim whose kind is toKind or derives from it are Children,
 * and that prim takes state. An unloaded payload reached by either walk takes state, as nothing
 * below it can be seen yet.
 **/
struct SceneDrawRule {
    std::string pattern;
    SceneDrawStateEnum state = eSceneDrawStateBBox;
    int levels = 0;
    std::string toKind;
    // The script name of the node that set the rule, for the rows it decides.
    std::string origin;
    // The identifiers of the layers a walk below a match keeps to: it stops at a prim that only
    // other layers of the stage's own layer stack author, which another node added. Empty, it
    // walks every prim.
    std::vector<std::string> layers;

    bool operator==(const SceneDrawRule& other) const
    {
        return (pattern == other.pattern) && (state == other.state) && (levels == other.levels) && (toKind == other.toKind) && (origin == other.origin) && (layers == other.layers);
    }

    bool operator!=(const SceneDrawRule& other) const
    {
        return !(*this == other);
    }
};

// In graph order: of two rules deciding one prim, the later one wins.
typedef std::vector<SceneDrawRule> SceneDrawRules;

/**
 * @brief Appends rules to *to, leaving out any rule *to held before: a rule reached through
 * several inputs of a merge comes from one upstream node, and keeps its first, weakest place, as
 * that node's layers do in the merged stack. Repeats within rules all go, in order: they are one
 * node's or one input's, in the order that decides between them.
 **/
void appendSceneDrawRules(const SceneDrawRules& rules,
                          SceneDrawRules* to);

/**
 * @brief The stage-level metadata of a scene, resolved over its layer stack: for each field the
 * strongest layer that authors it on its pseudo-root wins, and USD's fallback applies when none
 * does. Composition writes these onto the stage's root layer, since USD reads stage metadata from
 * the root layer only.
 **/
struct SceneMetadata {
    enum UpAxisEnum {
        eUpAxisY = 0,
        eUpAxisZ
    };

    UpAxisEnum upAxis = eUpAxisY;
    double metersPerUnit = 0.01;
    std::string defaultPrim;
    double timeCodesPerSecond = 24.;
    // The stage's active UsdRenderSettings prim (UsdRenderTokens->renderSettingsPrimPath); empty when none.
    std::string renderSettingsPrimPath;

    bool operator==(const SceneMetadata& other) const
    {
        return upAxis == other.upAxis && metersPerUnit == other.metersPerUnit && defaultPrim == other.defaultPrim && timeCodesPerSecond == other.timeCodesPerSecond && renderSettingsPrimPath == other.renderSettingsPrimPath;
    }

    bool operator!=(const SceneMetadata& other) const
    {
        return !(*this == other);
    }
};

struct ScenePayloadImpl;

/**
 * @brief What a scene-kind edge carries: an ordered stack of USD layers, strongest last, each one
 * holding exactly one node's edits. It is time-independent (one payload per layer stack, not per
 * frame): consumers pick the time when they query the composed stage.
 *
 * Immutable and cheap to copy. A payload made by appending a layer (SceneOps) points at its
 * base's stack, so a chain of N scene nodes holds N layer entries in all, not N squared. The
 * layers themselves are never edited once they are in a payload.
 *
 * Beside the stack it carries the graph's draw rules, which no layer holds: they leave the stack
 * hash and isSameStack() as they are, so changing them never authors or composes anything.
 * EffectInstance::evaluateScene() gathers them along the edges.
 *
 * USD types never appear here. Code that needs the layers or the composed stage goes through
 * Engine/Nodes/Scene/ (SceneOps, SceneStage, SceneQueryPool), the only place pxr headers are included.
 **/
class ScenePayload {
public:
    // An empty stack: no layers, stack hash 0, fallback metadata, no draw rules.
    ScenePayload();

    // No draw rules.
    explicit ScenePayload(const std::shared_ptr<const ScenePayloadImpl>& imp);

    ~ScenePayload();

    bool isEmpty() const;

    std::size_t getNumLayers() const;

    /**
     * @brief Combines the hashes of the layers in stack order, so equal stacks hash equal and
     * appending a layer moves it. 0 only for the empty stack.
     **/
    U64 getStackHash() const;

    const SceneMetadata& getMetadata() const;

    /**
     * @brief True if both hold the same layers, by identity, in the same order.
     **/
    bool isSameStack(const ScenePayload& other) const;

    const SceneDrawRules& getDrawRules() const;

    // The same stack with rules in place of this payload's draw rules.
    ScenePayload withDrawRules(const SceneDrawRules& rules) const;

    // For Engine/Nodes/Scene/ only; null for the empty stack.
    const std::shared_ptr<const ScenePayloadImpl>& getImpl() const
    {
        return _imp;
    }

private:
    std::shared_ptr<const ScenePayloadImpl> _imp;
    // Null when there are none.
    std::shared_ptr<const SceneDrawRules> _drawRules;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_SCENEPAYLOAD_H
