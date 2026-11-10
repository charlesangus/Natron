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

#ifndef Engine_Nodes_Scene_LiveTextureSceneIndex_h
#define Engine_Nodes_Scene_LiveTextureSceneIndex_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/GlobalDefines.h"
#include "Global/Macros.h"

#include <memory>

#include "Engine/Nodes/Scene/UsdIncludes.h"

#ifdef emit
#define NATRON_LIVETEXTURESCENEINDEX_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/materialNetworkInterface.h>
#include <pxr/usd/sdf/path.h>
#ifdef NATRON_LIVETEXTURESCENEINDEX_RESTORE_EMIT
#undef NATRON_LIVETEXTURESCENEINDEX_RESTORE_EMIT
#define emit
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief What lets Cycles and MoonRay read a natron:// texture: neither goes through Ar for textures,
 * both hand the resolved path of a UsdUVTexture's file to OIIO. A material filter sets that path to the
 * texture's file in shared memory, leaving the asset as authored, and Storm, which reads the asset
 * through its resolver, never sees it.
 *
 * Neither delegate honours a UsdUVTexture's fallback either: with no file to give, the filter removes
 * the texture node and writes kLiveTextureMissingColour onto the inputs it fed, magenta in both.
 **/
namespace LiveTextureSceneIndex {
class TextureHolds;

void registerSceneIndices();

/**
 * @brief The filter one scene index applies, whose copies share what it holds: every file it hands
 * out, and the one before it, because both renderers can close a file and open it again mid-render.
 *
 * Cycles' filter also turns every UsdTransform2d into the Cycles node doing the same, since hdCycles
 * has no mapping of its own for one, and has a live texture read as it is: as raw data, the pixels
 * being in the working space already, and with its alpha a channel of its own, so the colour is the
 * straight colour the file holds whether or not the alpha is used.
 **/
class MaterialFilter {
public:
    explicit MaterialFilter(bool cycles);

    void operator()(PXR_NS::HdMaterialNetworkInterface* network) const;

    /**
     * @brief Lets go of the files held for the materials at and under paths, which the scene no
     * longer has. The delegate can read them until it syncs the removal, so they go at the next call,
     * or with the filter.
     **/
    void forget(const PXR_NS::SdfPathVector& paths) const;

private:
    std::shared_ptr<TextureHolds> _holds;
    bool _cycles;
};

MaterialFilter makeMaterialFilter();
MaterialFilter makeCyclesMaterialFilter();
} // namespace LiveTextureSceneIndex

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_LiveTextureSceneIndex_h
