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

#ifndef Engine_Nodes_Scene_ScenePayloadImpl_h
#define Engine_Nodes_Scene_ScenePayloadImpl_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What a layer composes from outside the stack it sits in: anonymous layers its references
 * name by identifier, and the stacks those sublayer. An anonymous layer lives only while something
 * holds it, and a reference to a dead one composes an empty prim without any error, so the entry
 * holding the referencing layer holds these too.
 **/
struct SceneLayerDependencies {
    std::vector<PXR_NS::SdfLayerRefPtr> layers;
    std::vector<ScenePayload> stacks;
};

/**
 * @brief The relationships and attribute connections a layer authors with a target outside the
 * root prim they sit under, by property path, sorted and unique. A reference to that root drops
 * such targets, so code that re-roots prims repairs the properties listed. An index may list more
 * than the composed stage has: the composed targets decide what is repaired.
 **/
typedef std::vector<PXR_NS::SdfPath> SceneLinkIndex;

/**
 * @brief One custom-data entry of a layer region: what USDWrite authors in the customLayerData of
 * the region's file. text is the value as the user typed it, type its type's name.
 **/
struct SceneLayerCustomDatum {
    std::string name;
    std::string type;
    std::string text;
    PXR_NS::VtValue value;
};

/**
 * @brief What a USDConfigureLayer records for a region of the layer stack: the file it is saved
 * to, empty for a region flattened into the file of its parent, and the layer metadata the user
 * set. Unset fields are left to whatever else authors them.
 **/
struct SceneLayerRegionSettings {
    std::string savePath;
    // The USDConfigureLayer the settings come from; empty when none set them.
    std::string node;
    std::optional<std::string> defaultPrim;
    std::optional<std::string> comment;
    std::optional<double> startTime;
    std::optional<double> endTime;
    std::optional<double> timeCodesPerSecond;
    std::optional<double> framesPerSecond;
    std::optional<SceneMetadata::UpAxisEnum> upAxis;
    std::optional<double> metersPerUnit;
    std::optional<std::string> colorConfiguration;
    std::optional<std::string> colorManagementSystem;
    std::optional<std::string> renderSettingsPrimPath;
    std::vector<SceneLayerCustomDatum> customData;

    bool isEmpty() const;

    // Takes every field stronger sets, custom data by name.
    void mergeFrom(const SceneLayerRegionSettings& stronger);

    U64 hash() const;
};

/**
 * @brief A region of the layer stack, which USDWrite saves to a file of its own when its settings
 * name one: the entries tagged with it (SceneLayerEntry::region) and those of the regions nested
 * in it. Immutable; a change of settings makes another region, under another id.
 **/
struct SceneLayerRegion {
    U64 id = 0;
    // The node that opened or claimed the region.
    std::string node;
    std::shared_ptr<const SceneLayerRegion> parent;
    SceneLayerRegionSettings settings;
};

typedef std::shared_ptr<const SceneLayerRegion> SceneLayerRegionPtr;

struct SceneLayerEntry {
    PXR_NS::SdfLayerRefPtr layer;
    U64 hash = 0;
    // Null for a layer that composes nothing outside its stack.
    std::shared_ptr<const SceneLayerDependencies> dependencies;
    // Null for a layer that authors no link out of its root.
    std::shared_ptr<const SceneLinkIndex> links;
    // The live textures the layer authors, which consumers render before Hydra reads them. Null
    // for a layer that authors none.
    std::shared_ptr<const LiveTextureSourceList> liveTextures;
    // The layer leaves projection cameras for the composed stage to resolve.
    bool deferredProjections = false;
    // The layer sits above a USDLayerBreak: it composes like any other, but an export that strips
    // breaks leaves it out of what it writes.
    bool context = false;
    // The innermost region the layer belongs to; null outside every region.
    SceneLayerRegionPtr region;
    // A boundary of a layer region (a file read, a break, another region) lies above the layer on
    // every path a merge reached it by: a USDConfigureLayer gathering the preceding layers stops
    // short of it.
    bool sealed = false;
    // The strongest layer of a merge's result, below which the preceding layers are gathered from
    // every input, up to each one's own boundary.
    bool mergeTop = false;
};

/**
 * @brief The shared state behind a non-empty ScenePayload: its strongest layer and a link to the
 * payload it was appended to, which holds the weaker ones. A payload without layers whose regions
 * are open has one too, with no layer and no base: it is never the base of another link.
 **/
struct ScenePayloadImpl {
    std::shared_ptr<const ScenePayloadImpl> base;
    SceneLayerEntry top;
    std::size_t numLayers = 0;
    // The stack hash before openRegions is folded in.
    U64 closedStackHash = 0;
    U64 stackHash = 0;
    U64 contentHash = 0;
    SceneMetadata metadata;
    // The regions entries appended to the payload go into, outermost first.
    std::vector<SceneLayerRegionPtr> openRegions;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_ScenePayloadImpl_h
