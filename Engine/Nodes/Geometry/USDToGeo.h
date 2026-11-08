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

#ifndef Engine_Nodes_Geometry_USDToGeo_h
#define Engine_Nodes_Geometry_USDToGeo_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/PrimSelector.h"

#define PLUGINID_NATRON_USDTOGEO "USDToGeo"
#define kUSDToGeoParamPrimPattern "primPattern"
#define kUSDToGeoParamUnpack "unpack"
#define kUSDToGeoParamFlattenTransforms "flattenTransforms"

NATRON_NAMESPACE_ENTER

/**
 * @brief Turns the prims a pattern matches in the input scene into geometry at the current frame.
 * Packed, each imageable prim becomes one packed USD primitive and no point array is read;
 * unpacked, meshes, points and curves become polygons, points and curves in one point pool.
 * Every primitive's `path` holds its source prim path.
 **/
class USDToGeo
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDTOGEO,
        .label = "USDToGeo",
        .description = "Turn the prims a pattern matches in a scene into geometry. By default each prim becomes one "
                       "packed primitive carrying its path, transforms and bounds, so moving prims stays cheap; "
                       "Unpack reads meshes, points and curves as editable geometry. Each primitive's path "
                       "attribute names the prim it came from.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDToGeo(node);
    }

    explicit USDToGeo(NodePtr node);

    virtual ~USDToGeo();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void updateFlattenEnabled();

    PrimSelector _primPattern;
    KnobBoolWPtr _unpack;
    KnobBoolWPtr _flatten;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_USDToGeo_h
