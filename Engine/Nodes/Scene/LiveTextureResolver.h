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

#ifndef Engine_Nodes_Scene_LiveTextureResolver_h
#define Engine_Nodes_Scene_LiveTextureResolver_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief OpenUSD's resolver for natron:// URIs, which Ar hands every asset path of that scheme. A
 * URI resolves to itself while LiveTextureStore holds its texture, and to nothing otherwise; opening
 * it serves the stored bytes without a copy. It never renders: consumers fill the store before
 * Hydra reads the stage (LiveTextureStore::prepare). The URIs are read-only and carry their
 * content's hash, so their extension is the image type LiveTextureImage reads and their timestamp
 * never changes.
 *
 * Registered under its type name, Natron::LiveTextureResolver, by SceneCaches::registerLiveTextures,
 * with the plugInfo.json that declares its scheme.
 **/
class LiveTextureResolver
    : public PXR_NS::ArResolver {
public:
    LiveTextureResolver();
    virtual ~LiveTextureResolver() OVERRIDE;

protected:
    virtual std::string _CreateIdentifier(const std::string& assetPath,
                                          const PXR_NS::ArResolvedPath& anchorAssetPath) const OVERRIDE;
    virtual std::string _CreateIdentifierForNewAsset(const std::string& assetPath,
                                                     const PXR_NS::ArResolvedPath& anchorAssetPath) const OVERRIDE;
    virtual PXR_NS::ArResolvedPath _Resolve(const std::string& assetPath) const OVERRIDE;
    virtual PXR_NS::ArResolvedPath _ResolveForNewAsset(const std::string& assetPath) const OVERRIDE;
    virtual std::string _GetExtension(const std::string& assetPath) const OVERRIDE;
    virtual PXR_NS::ArTimestamp _GetModificationTimestamp(const std::string& assetPath,
                                                          const PXR_NS::ArResolvedPath& resolvedPath) const OVERRIDE;
    virtual std::shared_ptr<PXR_NS::ArAsset> _OpenAsset(const PXR_NS::ArResolvedPath& resolvedPath) const OVERRIDE;
    virtual bool _CanWriteAssetToPath(const PXR_NS::ArResolvedPath& resolvedPath,
                                      std::string* whyNot) const OVERRIDE;
    virtual std::shared_ptr<PXR_NS::ArWritableAsset> _OpenAssetForWrite(const PXR_NS::ArResolvedPath& resolvedPath,
                                                                        WriteMode writeMode) const OVERRIDE;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_LiveTextureResolver_h
