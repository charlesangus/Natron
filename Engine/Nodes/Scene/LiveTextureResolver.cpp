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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "LiveTextureResolver.h"

#include <filesystem>
#include <mutex>
#include <system_error>

#ifdef emit
#define NATRON_LIVETEXTURERESOLVER_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/tf/type.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/defineResolver.h>
#include <pxr/usd/ar/inMemoryAsset.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/timestamp.h>
#include <pxr/usd/ar/writableAsset.h>
#ifdef NATRON_LIVETEXTURERESOLVER_RESTORE_EMIT
#undef NATRON_LIVETEXTURERESOLVER_RESTORE_EMIT
#define emit
#endif

#include "Engine/Nodes/Scene/LiveTextureImage.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneCaches.h"

NATRON_NAMESPACE_ENTER

namespace {
// The name plugInfo.json registers the resolver and image reader under.
const char* const kPluginName = "natronLiveTexture";
}

LiveTextureResolver::LiveTextureResolver()
{
}

LiveTextureResolver::~LiveTextureResolver()
{
}

std::string
LiveTextureResolver::_CreateIdentifier(const std::string& assetPath,
                                       const PXR_NS::ArResolvedPath& /*anchorAssetPath*/) const
{
    // A URI is absolute: no anchor changes what it names.
    return assetPath;
}

std::string
LiveTextureResolver::_CreateIdentifierForNewAsset(const std::string& assetPath,
                                                  const PXR_NS::ArResolvedPath& /*anchorAssetPath*/) const
{
    return assetPath;
}

PXR_NS::ArResolvedPath
LiveTextureResolver::_Resolve(const std::string& assetPath) const
{
    return LiveTextureStore::instance().contains(assetPath) ? PXR_NS::ArResolvedPath(assetPath) : PXR_NS::ArResolvedPath();
}

PXR_NS::ArResolvedPath
LiveTextureResolver::_ResolveForNewAsset(const std::string& /*assetPath*/) const
{
    return PXR_NS::ArResolvedPath();
}

std::string
LiveTextureResolver::_GetExtension(const std::string& /*assetPath*/) const
{
    // Hio picks its reader by extension, which the URI has none of.
    return kLiveTextureScheme;
}

PXR_NS::ArTimestamp
LiveTextureResolver::_GetModificationTimestamp(const std::string& /*assetPath*/,
                                               const PXR_NS::ArResolvedPath& /*resolvedPath*/) const
{
    // A URI's content never changes: its version is in it.
    return PXR_NS::ArTimestamp(0.);
}

std::shared_ptr<PXR_NS::ArAsset>
LiveTextureResolver::_OpenAsset(const PXR_NS::ArResolvedPath& resolvedPath) const
{
    const LiveTextureBufferConstPtr buffer = LiveTextureStore::instance().find(resolvedPath.GetPathString());

    if (!buffer) {
        return std::shared_ptr<PXR_NS::ArAsset>();
    }

    return PXR_NS::ArInMemoryAsset::FromBuffer(buffer->getBytes(), buffer->getByteSize());
}

bool
LiveTextureResolver::_CanWriteAssetToPath(const PXR_NS::ArResolvedPath& /*resolvedPath*/,
                                          std::string* whyNot) const
{
    if (whyNot) {
        *whyNot = "Live textures are rendered by Natron and cannot be written";
    }

    return false;
}

std::shared_ptr<PXR_NS::ArWritableAsset>
LiveTextureResolver::_OpenAssetForWrite(const PXR_NS::ArResolvedPath& /*resolvedPath*/,
                                        WriteMode /*writeMode*/) const
{
    return std::shared_ptr<PXR_NS::ArWritableAsset>();
}

namespace SceneCaches {
bool
registerLiveTextures(std::string* errorMessage)
{
    static std::once_flag once;
    static std::string error;

    std::call_once(once, []() {
        // Defined before the plugInfo.json declares them, so Ar and Hio find their factories
        // without loading a library: the types live in this executable.
        PXR_NS::Ar_DefineResolver<LiveTextureResolver, PXR_NS::ArResolver>();
        PXR_NS::TfType::Define<LiveTextureImage, PXR_NS::TfType::Bases<PXR_NS::HioImage>>()
            .SetFactory<PXR_NS::HioImageFactory<LiveTextureImage>>();

        std::error_code ec;
        const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        const std::filesystem::path dir = ec ? std::filesystem::path() : (exe.parent_path() / ".." / "Resources" / "usd" / kPluginName).lexically_normal();
        if (dir.empty() || !std::filesystem::is_regular_file(dir / "plugInfo.json", ec)) {
            error = "Live textures are unavailable: Resources/usd/natronLiveTexture/plugInfo.json is missing from this installation";

            return;
        }
        PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(dir.string());
        if (!PXR_NS::PlugRegistry::GetInstance().GetPluginWithName(kPluginName)) {
            error = "Live textures are unavailable: OpenUSD did not register " + (dir / "plugInfo.json").string();
        }
    });
    if (!error.empty() && errorMessage) {
        *errorMessage = error;
    }

    return error.empty();
}
} // namespace SceneCaches

NATRON_NAMESPACE_EXIT
