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

#ifndef Engine_Nodes_Scene_ScenePrimitiveNode_h
#define Engine_Nodes_Scene_ScenePrimitiveNode_h

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
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/ScenePayload.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define kScenePrimitiveParamDoubleSided "doubleSided"
#define kScenePrimitiveParamPivot "pivot"

NATRON_NAMESPACE_ENTER

struct PrimitiveMesh;
struct SceneLayerEdit;
struct SceneMaterialGraft;

/**
 * @brief A scene source authoring one UsdGeomMesh at its Prim Path (/<script name> by default)
 * from a PrimitiveMeshes builder: faceVarying normals and st, no subdivision, the project's up
 * axis and units. Its transform is one xformOp:transform op, the translate, rotate and scale knobs
 * about the pivot. An optional Material input's material is grafted under the mesh and bound to it.
 *
 * Shape knobs that change the topology (segment counts, caps) are not animatable, so the
 * topology is authored once; animated sizes author the points and extent as time samples.
 **/
class ScenePrimitiveNode
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Material", .kind = eDataKindScene, .optional = true },
    };

    virtual ~ScenePrimitiveNode();

    const SceneTransformKnobs& getTransformKnobs() const
    {
        return _transform;
    }

    KnobDoublePtr getPivotKnob() const;

protected:
    explicit ScenePrimitiveNode(NodePtr node);

    // The prim name standing in for the script name before the node has one.
    virtual std::string getFallbackPrimName() const = 0;

    // Creates the type's shape knobs on page, between the Prim Path and Double Sided.
    virtual void initializeShapeKnobs(const KnobPagePtr& page) = 0;

    // Whether a knob the points depend on is animated.
    virtual bool isShapeAnimated() const = 0;

    // The mesh at frame. Its topology may only depend on knobs that cannot animate.
    virtual bool buildMesh(double frame,
                           SceneMetadata::UpAxisEnum upAxis,
                           PrimitiveMesh* mesh) const
        = 0;

    // Called once the transform knobs exist, for a type whose default scale is not 1.
    virtual void initializeTransformDefaults(const SceneTransformKnobs& /*transform*/)
    {
    }

    virtual bool getDefaultDoubleSided() const
    {
        return false;
    }

    KnobDoublePtr createSizeKnob(const KnobPagePtr& page,
                                 const QString& label,
                                 const char* name,
                                 const QString& toolTip,
                                 double defaultValue,
                                 int dimension = 1);

    // A non-animatable segment count, at least minimum, in each dimension.
    KnobIntPtr createSegmentsKnob(const KnobPagePtr& page,
                                  const QString& label,
                                  const char* name,
                                  const QString& toolTip,
                                  int defaultValue,
                                  int minimum,
                                  int dimension = 1);

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool authorLayer(const SceneLayerEdit& edit,
                     const SceneMaterialGraft& graft,
                     std::string* errorMessage) const;

    KnobStringWPtr _primPath;
    KnobBoolWPtr _doubleSided;
    SceneTransformKnobs _transform;
    KnobDoubleWPtr _pivot;
    SceneGraftMemo _graftMemo;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_ScenePrimitiveNode_h
