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

#include <string>

#include <QString>

#include "Engine/EngineFwd.h"

// Free of USD, like the node headers that include it: the knobs the scene create nodes share.
// Their authoring is in SceneNodeAuthoring.h.

#define kSceneParamPrimPath "primPath"
#define kSceneParamTranslate "translate"
#define kSceneParamRotate "rotate"
#define kSceneParamScale "scale"
#define kSceneParamUniformScale "uniformScale"
#define kSceneParamRotateOrder "rotateOrder"

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

// Hides and disables knob, or shows and enables it.
void setSceneKnobSecret(const KnobIPtr& knob,
                        bool secret);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneNodeKnobs_h
