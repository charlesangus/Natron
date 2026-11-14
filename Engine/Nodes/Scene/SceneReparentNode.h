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
#include <vector>

#include <QString>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define kSceneReparentParamKind "kind"
#define kSceneReparentParamAutoRename "autoRename"
#define kSceneReparentParamMode "mode"
#define kSceneReparentParamWrapName "wrapName"

#define kSceneReparentInputCount 64

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

// What a SceneReparentNode that offers both does with its Prim Path.
enum SceneReparentModeEnum {
    // Defines the prim at Prim Path and grafts every input's root prims under it.
    eSceneReparentModeGraft = 0,
    // Defines the prim beside the input 0 prim at Prim Path, named Wrap Name, and moves that prim
    // and its descendants under it.
    eSceneReparentModeWrap
};

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

    // Whether the node is in wrap mode.
    bool isWrapping() const;

    /**
     * @brief The path of the prim the node defines: Prim Path, or in wrap mode Prim Path's parent
     * with Wrap Name appended. Empty when the knobs name no valid path.
     **/
    std::string getDefinedPrimPath() const;

    /**
     * @brief Where the prim at outputPath in the node's scene comes from in the scenes of the inputs
     * the node reads: every input, or input 0 alone in wrap mode. Empty for the defined prim
     * itself, for a path under none of the grafts, and for a path in wrap mode left without a prim
     * by the move. Grafts renamed by Auto Rename are not followed.
     **/
    std::string mapToInputPath(const std::string& outputPath) const;

protected:
    explicit SceneReparentNode(NodePtr node);

    /**
     * @brief Creates the Prim Path, Kind and Auto Rename knobs on page, and the Mode and Wrap Name
     * knobs when offersWrapMode().
     **/
    void createReparentKnobs(const KnobPagePtr& page);

    // Whether the node offers wrap mode besides grafting.
    virtual bool offersWrapMode() const
    {
        return false;
    }

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

    virtual SceneEvaluationContext getSceneInputContext(int inputNb,
                                                        const SceneEvaluationContext& ctx,
                                                        double time,
                                                        ViewIdx view) const OVERRIDE FINAL;

    virtual void mapSceneInputReach(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    const std::vector<std::string>& in,
                                    std::vector<std::string>* out) const OVERRIDE FINAL;

    /**
     * @brief Reads the knobs that place the input's prims: the graft destination and whether it
     * renames, or with *wrapping set, the wrapped prim and where it moves. False when they are
     * invalid, in which case the node renders nothing to map.
     **/
    bool getPlacement(bool* wrapping,
                      std::string* destination,
                      std::string* moved,
                      bool* autoRename) const;

    // getPlacement() with the knobs read at time and view.
    bool getPlacement(double time,
                      ViewIdx view,
                      bool* wrapping,
                      std::string* destination,
                      std::string* moved,
                      bool* autoRename) const;

    std::string getPassThroughRoot() const;

    bool authorOverrides(const SceneLayerEdit& edit,
                         const std::string& primPath,
                         int kind,
                         std::string* errorMessage) const;

    StatusEnum renderWrap(const SceneRenderActionArgs& args,
                          const std::string& wrappedPath,
                          int kind,
                          ScenePayload* outputScene,
                          std::string* errorMessage);

    std::string getDefaultWrapName() const;

    KnobStringWPtr _primPath;
    KnobChoiceWPtr _kind;
    KnobBoolWPtr _autoRename;
    KnobChoiceWPtr _mode;
    KnobStringWPtr _wrapName;
    // Mutable for mapSceneInputReach(), which reads what the last graft of a context named.
    mutable SceneGraftMemo _graftMemo;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneReparentNode_h
