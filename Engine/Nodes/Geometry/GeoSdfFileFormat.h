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

#ifndef Engine_Nodes_Geometry_GeoSdfFileFormat_h
#define Engine_Nodes_Geometry_GeoSdfFileFormat_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <string>

#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

struct GeoUsdInputScene;

/**
 * @brief Geometry served to OpenUSD as layers, without writing files.
 *
 * A provider is registered under a key; the layer "natrongeo:<key>.natrongeo" then opens
 * through SdfLayer::FindOrOpen and reads the provider's current detail, translated by
 * GeoUsdConvert on demand. The layers are read-only: saving or exporting one fails.
 * refresh() brings an open layer to the provider's current detail with field-level edits, so
 * stages see change notices for the prims that changed only.
 **/
namespace GeoLayers {

/**
 * @brief Defines the file format and resolver and registers their plugInfo.json. Called once at
 * startup, before any stage is opened; later calls return the first result.
 **/
bool registerPlugin(std::string* errorMessage);

/**
 * @brief The layer identifier serving @p key. @p rootPath and @p packedMode ("instancer",
 * "native" or "unpack") become file format arguments when not empty.
 **/
std::string identifier(const std::string& key,
                       const std::string& rootPath = std::string(),
                       const std::string& packedMode = std::string());

/**
 * @brief Serves @p key from @p provider, which is called on whichever thread opens or refreshes
 * the layer and must return the detail current at that time.
 **/
void setProvider(const std::string& key,
                 std::function<GeoDetailConstPtr()> provider);
void removeProvider(const std::string& key);

/**
 * @brief The stage @p key's layer is composed over, as GeoUsdConvertOptions::inputScene; null
 * for none. Taken by the next open or refresh of the layer.
 **/
void setInputScene(const std::string& key,
                   std::shared_ptr<const GeoUsdInputScene> scene);
std::shared_ptr<const GeoUsdInputScene> currentInputScene(const std::string& key);
bool hasProvider(const std::string& key);

/** @brief The provider's current detail, null without a provider. */
GeoDetailConstPtr currentDetail(const std::string& key);

/**
 * @brief Applies the difference between the open layer @p identifier and its provider's current
 * detail as field edits in one change block. Prims whose topology or property set changed are
 * removed and authored again; the others only change values. Does nothing when the detail is
 * unchanged, along with the input scene. Must not run concurrently with readers of the layer.
 **/
bool refresh(const std::string& identifier,
             std::string* errorMessage);

} // namespace GeoLayers

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoSdfFileFormat_h
