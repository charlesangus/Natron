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

#ifndef Engine_Nodes_Scene_SceneReparentNode_h
#define Engine_Nodes_Scene_SceneReparentNode_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include <QString>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define kSceneReparentParamKind "kind"
#define kSceneReparentParamAutoRename "autoRename"

#define kSceneReparentInputCount 64

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

/**
 * @brief A node that makes the node graph's shape the scene's: it defines one prim at its Prim
 * Path and grafts every root prim of every input under it, inputs weakest first, through
 * SceneOps::reparentRoots. Missing ancestors of a nested path get the prim's own type. The prim's
 * own opinions (its kind, and what authorPrimOverrides() adds) go in an over-only layer of their
 * own, so editing them leaves the grafted prims alone.
 *
 * A relationship target or connection a graft cannot carry sets a node warning naming it; an
 * input root at the node's own path is a node error.
 **/
class SceneReparentNode
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "", .kind = eDataKindScene, .role = InputRole::Variadic, .optional = true, .count = kSceneReparentInputCount, .firstNumber = 0 },
    };

    virtual ~SceneReparentNode();

protected:
    explicit SceneReparentNode(NodePtr node);

    // Creates the Prim Path, Kind and Auto Rename knobs on page.
    void createReparentKnobs(const KnobPagePtr& page);

    // The schema type of the prim and of its missing ancestors, such as Xform.
    virtual std::string getPrimTypeName() const = 0;

    /**
     * @brief Authors the prim's own opinions into edit.layer, which holds no upstream, as "over"
     * specs only. overridesReadFrameRange() says whether they sample the project frame range.
     **/
    virtual bool authorPrimOverrides(const SceneLayerEdit& edit,
                                     const std::string& primPath,
                                     std::string* errorMessage) const;

    virtual bool overridesReadFrameRange() const
    {
        return false;
    }

private:
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;

    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool authorOverrides(const SceneLayerEdit& edit,
                         const std::string& primPath,
                         int kind,
                         std::string* errorMessage) const;

    KnobStringWPtr _primPath;
    KnobChoiceWPtr _kind;
    KnobBoolWPtr _autoRename;
    SceneGraftMemo _graftMemo;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneReparentNode_h
