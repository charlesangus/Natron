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

#ifndef Engine_Nodes_Scene_DelegateSupport_h
#define Engine_Nodes_Scene_DelegateSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>

#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What of a stage a render delegate cannot render. Hydra drops a prim of a type its
 * delegate does not make, and a delegate renders a material it cannot build with its fallback,
 * both without a word; these name them for the node's warning.
 **/
namespace DelegateSupport {
struct Support {
    // The delegate's sprim, rprim and bprim types; empty when it could not be made, which checks
    // nothing.
    PXR_NS::TfTokenVector primTypes;
    // The material render contexts the delegate asks for; the universal one is always taken.
    PXR_NS::TfTokenVector materialRenderContexts;
    PXR_NS::TfToken materialBindingPurpose;
    // Types a scene index registered for the delegate makes meshes of before it sees them.
    PXR_NS::TfTokenVector tessellated;
    // Whether the delegate builds a node of a shader id; every id when unset.
    std::function<bool(const PXR_NS::TfToken&)> takesShader;
};

// The types, contexts and binding purpose of a delegate of rendererId made with no settings, for
// a delegate that starts no render on being made.
Support probe(const std::string& rendererId) WARN_UNUSED_RETURN;

/**
 * @brief The node's warning about what rendererName cannot render of stage, one line per kind:
 * per Hydra prim type it does not make, per shader id it does not build, and for materials with
 * no surface in its render contexts, each line naming the kind and its prims. Empty when it
 * renders everything.
 **/
std::string warningText(const PXR_NS::UsdStageRefPtr& stage,
                        const Support& support,
                        const std::string& rendererName) WARN_UNUSED_RETURN;
} // namespace DelegateSupport

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_DelegateSupport_h
