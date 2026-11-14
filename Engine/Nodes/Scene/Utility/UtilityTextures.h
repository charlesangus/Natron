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

#ifndef Engine_Nodes_Scene_Utility_UtilityTextures_h
#define Engine_Nodes_Scene_Utility_UtilityTextures_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <pxr/base/gf/vec4f.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One decoded image and its box-filtered mips, linear RGBA. Each level halves the one before,
 * never below one texel, and its rows run from the bottom as texture coordinates do.
 **/
struct UtilityTextureImage {
    int width = 0;
    int height = 0;
    int levels = 0;
    // Every level, the full-size one first.
    std::vector<PXR_NS::GfVec4f> texels;
};

typedef std::shared_ptr<const UtilityTextureImage> UtilityTextureImageConstPtr;

/**
 * @brief A texture asset as loaded: one image, or for a <UDIM> path one per tile from 1001 to 1100,
 * null where the tile has no file.
 **/
struct UtilityTextureAsset {
    static const int kUdimTileCount = 100;

    bool udim = false;
    std::vector<UtilityTextureImageConstPtr> tiles;

    bool loaded() const;
};

typedef std::shared_ptr<const UtilityTextureAsset> UtilityTextureAssetConstPtr;

/**
 * @brief Every texture the scene's materials read, each loaded once through HioImage, so files,
 * UDIM tiles and natron:// live textures share one path. A live texture's URI carries its content
 * version, so a new version is a new asset. Failed loads are kept as failures rather than retried
 * every frame. MT-safe.
 **/
class UtilityTexturePool {
public:
    typedef std::pair<std::string, int> Key;

    UtilityTexturePool();
    ~UtilityTexturePool();

    // Null when the asset could not be read, which failures() then names.
    UtilityTextureAssetConstPtr acquire(const std::string& asset,
                                        UtilityTextureRef::ColorSpace colorSpace);
    // Drops every asset not in keys, failures included.
    void retainOnly(const std::set<Key>& keys);
    // One line per asset that could not be read, naming it.
    std::vector<std::string> failures() const;
    std::size_t size() const;

    // Decodes one file, or returns null and says why in error.
    static UtilityTextureImageConstPtr loadImage(const std::string& path,
                                                 UtilityTextureRef::ColorSpace colorSpace,
                                                 std::string* error);

private:
    struct Entry {
        UtilityTextureAssetConstPtr asset;
        std::string error;
    };

    mutable std::mutex _mutex;
    std::map<Key, Entry> _entries;
};

typedef std::shared_ptr<UtilityTexturePool> UtilityTexturePoolPtr;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityTextures_h
