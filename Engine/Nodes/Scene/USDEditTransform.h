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

#ifndef Engine_Nodes_Scene_USDEditTransform_h
#define Engine_Nodes_Scene_USDEditTransform_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDEDITTRANSFORM "USDEditTransform"

#define kUSDEditTransformParamPrimPattern "primPattern"
#define kUSDEditTransformParamMode "mode"
#define kUSDEditTransformParamPivot "pivot"
#define kUSDEditTransformParamCentrePivot "centrePivot"

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

/**
 * @brief Transforms the prims a prim pattern names in its input scene. Every match gets the same
 * xformOp:transform:<node name> op (numbered if the prim already has an op of that name) as the
 * strongest opinion: in Append mode it goes after the prim's existing ops, so it acts in the
 * prim's local space under them; in Replace mode !resetXformStack! precedes it, discarding the
 * existing ops and the parent transform. The op scales, turns and moves about a pivot in the
 * op's input space; nothing is authored to the XformCommonAPI pivot. A pattern that matches no
 * prim leaves the scene as it is and sets a node warning.
 **/
class USDEditTransform
    : public NativeEffectBase {
public:
    enum ModeEnum {
        eModeAppend = 0,
        eModeReplace
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDEDITTRANSFORM,
        .label = "USDEditTransform",
        .description = "Transform the prims a pattern names (paths, with * and ? wildcards) in the input scene. "
                       "Append adds the transform after each prim's existing ops, in its local space; Replace discards "
                       "the existing ops and the parent transform. The transform turns and scales about a pivot. "
                       "While its settings panel is open, the Viewer's 3D view shows move, rotate and scale handles.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDEditTransform.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDEditTransform(node);
    }

    explicit USDEditTransform(NodePtr node);

    virtual ~USDEditTransform();

    /**
     * @brief What the viewer's handles act on, from the last evaluation: the first matched prim
     * and the name of the op the node authors on it (xformOp:transform:...). False when the last
     * evaluation matched nothing.
     **/
    bool getHandleTarget(std::string* primPath,
                         std::string* opName) const;

    const SceneTransformKnobs& getTransformKnobs() const
    {
        return _transform;
    }

    KnobDoublePtr getPivotKnob() const;

    // Sets the pivot to the centre of the matched prims' bounds in their local space.
    bool centrePivot(std::string* errorMessage);

    // The xformOp:transform:... name the node authors where the prim has no op of that name yet.
    std::string getBaseOpName() const;

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    ModeEnum getMode() const;

    struct Implementation;
    std::unique_ptr<Implementation> _imp;

    KnobStringWPtr _primPattern;
    KnobChoiceWPtr _mode;
    KnobDoubleWPtr _pivot;
    KnobButtonWPtr _centrePivot;
    SceneTransformKnobs _transform;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDEditTransform_h
