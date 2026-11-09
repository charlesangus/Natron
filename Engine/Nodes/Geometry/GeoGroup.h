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

#ifndef Engine_Nodes_Geometry_GeoGroup_h
#define Engine_Nodes_Geometry_GeoGroup_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <set>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOGROUP "GeoGroup"

#define kGeoGroupParamGroupName "groupName"
#define kGeoGroupParamGroupType "groupType"
#define kGeoGroupParamInitialMerge "initialMerge"
#define kGeoGroupParamUsePattern "usePattern"
#define kGeoGroupParamPattern "pattern"
#define kGeoGroupParamUseBounding "useBounding"
#define kGeoGroupParamBoundingType "boundingType"
#define kGeoGroupParamCentre "centre"
#define kGeoGroupParamSize "size"
#define kGeoGroupParamIncludePartlyInside "includePartlyInside"

NATRON_NAMESPACE_ENTER

enum GeoGroupElementEnum {
    eGeoGroupElementPoints = 0,
    eGeoGroupElementPrimitives,
    eGeoGroupElementVertices,
    eGeoGroupElementEdges,
};

enum GeoGroupMergeEnum {
    eGeoGroupMergeReplace = 0,
    eGeoGroupMergeUnion,
    eGeoGroupMergeIntersect,
    eGeoGroupMergeSubtract,
};

enum GeoBoundingTypeEnum {
    eGeoBoundingTypeBox = 0,
    eGeoBoundingTypeSphere,
};

/** @brief What GeoGroup creates; the defaults are the knobs' defaults. */
struct GeoGroupParams {
    std::string name = "group1";
    GeoGroupElementEnum elements = eGeoGroupElementPrimitives;
    GeoGroupMergeEnum merge = eGeoGroupMergeReplace;
    bool usePattern = true;
    std::string pattern;
    bool useBounding = false;
    GeoBoundingTypeEnum boundingType = eGeoBoundingTypeBox;
    std::array<double, 3> centre = { 0., 0., 0. };
    std::array<double, 3> size = { 1., 1., 1. };
    bool includePartlyInside = false;
};

/**
 * @brief Creates or edits a named point, primitive, vertex or edge group.
 *
 * Members are what every enabled test selects. The pattern test takes a group pattern (see
 * geoSelectElements()); for edges its terms are "*", edge group names or globs, point paths
 * "pA-B-C" (element numbers, an edge between each consecutive pair), "@attr op value" on point
 * attributes (both ends must pass), and the '!' and '^' prefixes. An empty pattern selects
 * everything. The bounding test keeps what lies inside a box or an ellipsoid: a point inside
 * it, a vertex on such a point, and primitives and edges with all their points inside (any of
 * them with includePartlyInside). With no test enabled the selection is empty.
 *
 * The selection then replaces the group's members, or is unioned with, intersected with or
 * subtracted from them. Nothing but the group is written.
 **/
class GeoGroup
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = false },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOGROUP,
        .label = "GeoGroup",
        .description = "Create or edit a named group of points, primitives, vertices or edges: "
                       "by element numbers, existing groups and attribute comparisons, by a "
                       "bounding box or sphere, or both.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoGroup(node);
    }

    explicit GeoGroup(NodePtr node);
    virtual ~GeoGroup();

    /** @brief False with @p error set for an empty name or a malformed pattern. */
    static bool createGroup(GeoDetailBuilder& builder,
                            const GeoGroupParams& params,
                            std::string* error);

    /** @brief The edges of the live polygons and polylines that @p pattern selects. */
    static bool selectEdges(const GeoDetail& detail,
                            const std::string& pattern,
                            std::set<GeoEdge>* edges,
                            std::string* error);

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void updateVisibility();

    KnobStringWPtr _groupName;
    KnobChoiceWPtr _groupType;
    KnobChoiceWPtr _initialMerge;
    KnobBoolWPtr _usePattern;
    KnobStringWPtr _pattern;
    KnobBoolWPtr _useBounding;
    KnobChoiceWPtr _boundingType;
    KnobDoubleWPtr _centre;
    KnobDoubleWPtr _size;
    KnobBoolWPtr _includePartlyInside;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoGroup_h
