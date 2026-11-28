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

#ifndef Engine_Nodes_Scene_ScenePredicates_h
#define Engine_Nodes_Scene_ScenePredicates_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <pxr/usd/sdf/pathExpressionEval.h>
#include <pxr/usd/usd/collectionPredicateLibrary.h>

NATRON_NAMESPACE_ENTER

/**
 * @brief The predicates prim patterns are evaluated with, in nodes and in the scene graph's filter,
 * so that a pattern matches the same prims in both on the same stage.
 **/
namespace ScenePredicates {
typedef PXR_NS::SdfPathExpressionEval<PXR_NS::UsdObject> PathEval;

// The prim at a path of stage, for an incremental search over its prims.
struct PrimOfPath {
    PXR_NS::UsdStageWeakPtr stage;

    PXR_NS::UsdObject operator()(const PXR_NS::SdfPath& path) const
    {
        return stage ? PXR_NS::UsdObject(stage->GetPrimAtPath(path)) : PXR_NS::UsdObject();
    }
};

typedef PathEval::IncrementalSearcher<PrimOfPath> Searcher;

/**
 * @brief USD's collection predicates ({isa:Mesh}, {kind:component}, ...) and {purpose:<value>},
 * which matches the imageable prims whose computed purpose, inherited or authored, is value:
 * default, render, proxy or guide. A bare {purpose} is an error. Nothing here depends on what is
 * loaded in a viewer, so a node's output does not either. MT-safe.
 **/
const PXR_NS::UsdObjectPredicateLibrary& library();

// Whether prim is imageable and its computed purpose is purpose.
bool hasPurpose(const PXR_NS::UsdPrim& prim,
                const PXR_NS::TfToken& purpose);

/**
 * @brief Links expression with library into *eval. Returns false with *eval empty and *errorMessage
 * naming the failure when a predicate is unknown or takes other arguments.
 **/
bool link(const PXR_NS::SdfPathExpression& expression,
          const PXR_NS::UsdObjectPredicateLibrary& library,
          PathEval* eval,
          std::string* errorMessage);
} // namespace ScenePredicates

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_ScenePredicates_h
