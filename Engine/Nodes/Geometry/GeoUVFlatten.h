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

#ifndef Engine_Nodes_Geometry_GeoUVFlatten_h
#define Engine_Nodes_Geometry_GeoUVFlatten_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoUVFlattenSolver.h"
#include "Engine/Geo/GeoUVPack.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOUVFLATTEN "GeoUVFlatten"

#define kGeoUVFlattenParamGroup "group"
#define kGeoUVFlattenParamUVAttribute "uvAttribute"
#define kGeoUVFlattenParamFlatteningMethod "flatteningMethod"
#define kGeoUVFlattenParamPreserveSeams "preserveSeams"
#define kGeoUVFlattenParamPreserveLayout "preserveLayout"
#define kGeoUVFlattenParamPreserveIslandBoundaryShapes "preserveIslandBoundaryShapes"
#define kGeoUVFlattenParamSeams "seams"
#define kGeoUVFlattenParamRectifyGroup "rectifyGroup"
#define kGeoUVFlattenParamEnablePins "enablePins"
#define kGeoUVFlattenParamPins "pins"
#define kGeoUVFlattenParamEnableAxisAlignment "enableAxisAlignment"
#define kGeoUVFlattenParamAlignments "alignments"
#define kGeoUVFlattenParamEnableLoopStraightening "enableLoopStraightening"
#define kGeoUVFlattenParamStraightenings "straightenings"
#define kGeoUVFlattenParamEnableManualLayout "enableManualLayout"
#define kGeoUVFlattenParamLayoutSeams "layoutSeams"
#define kGeoUVFlattenParamOutputSeams "outputSeams"
#define kGeoUVFlattenParamOutputSeamsGroup "outputSeamsGroup"
#define kGeoUVFlattenParamOutputConstrainedIslands "outputConstrainedIslands"
#define kGeoUVFlattenParamOutputConstrainedIslandsGroup "outputConstrainedIslandsGroup"
#define kGeoUVFlattenParamCenter "center"
#define kGeoUVFlattenParamSize "size"

NATRON_NAMESPACE_ENTER

/** @brief A vertex pinned by its polygon's element number and its place in the polygon. */
struct GeoUVFlattenPinSpec {
    GeoIndex primitive = 0;
    int vertex = 0;
    GeoUV uv = { 0., 0. };
    // Where the polygon's next vertex goes when this is the only pin of its island.
    GeoUV referenceUV = { 0., 0. };
};

struct GeoUVFlattenAlignmentSpec {
    // A vertex group pattern; one vertex of those wired to a point is enough.
    std::string group;
    GeoUVFlattenAxisEnum axis = eGeoUVFlattenAxisU;
};

struct GeoUVFlattenParams {
    // A primitive group pattern; empty flattens every polygon.
    std::string group;
    std::string uvAttribute = "uv";
    GeoUVFlattenMethodEnum method = eGeoUVFlattenMethodSCP;
    bool preserveSeams = false;
    bool preserveLayout = false;
    bool preserveIslandBoundaryShapes = false;
    // Edge patterns; unlike a group, empty selects no edge.
    std::string seams;
    // A vertex pattern of vertex edges; empty selects none.
    std::string rectifyGroup;
    bool enablePins = true;
    std::vector<GeoUVFlattenPinSpec> pins;
    bool enableAxisAlignment = true;
    std::vector<GeoUVFlattenAlignmentSpec> alignments;
    bool enableLoopStraightening = true;
    // Vertex patterns of vertex edges, each one loop.
    std::vector<std::string> straightenings;
    bool enableManualLayout = true;
    std::string layoutSeams;
    bool outputSeams = false;
    std::string outputSeamsGroup = "seams";
    bool outputConstrainedIslands = false;
    std::string outputConstrainedIslandsGroup = "constrainedprims";
    GeoUV center = { 0.5, 0.5 };
    GeoUV size = { 1., 1. };
    std::function<bool()> aborted;
};

/** @brief What went less than well, for the node's warning. */
struct GeoUVFlattenReport {
    int degenerateFaces = 0;
    // Islands whose ABF solve failed and were flattened with SCP instead.
    int fellBack = 0;
    int unsolved = 0;
    int nonpacked = 0;
    // Pins naming no polygon of the group or no vertex of it.
    int ignoredPins = 0;
    bool aborted = false;
};

/**
 * @brief Cuts the group's polygons into islands along the seams, flattens each with the
 * method and its constraints, and packs them into the packing area, writing the result on the
 * vertex attribute params.uvAttribute (float32 x3 when made here; an existing vertex attribute of
 * two or more numeric components keeps its storage and its other components; a point one is
 * not written in place, a vertex attribute seeded from it receives the result).
 *
 * Packing keeps each island's orientation, so alignments and rectified quads stay on the axes:
 * free islands are first turned to their smallest bounding rectangle. Islands placed by two or
 * more pins, and with Preserve Layout the islands that already had uv, stay where they are and
 * the others are packed round them.
 *
 * With Enable Manual Layout off the islands are cut by the layout seams, flattened without
 * constraints and packed; the islands of the flattening seams are then flattened with their
 * constraints and each is fitted, by rotation, uniform scale and translation, onto where the
 * packing put its vertices.
 *
 * Non-polygon primitives, and polygons outside the group, keep their uv. Fails, with
 * *errorMessage set, for a malformed pattern or an empty attribute name; returns false with
 * report->aborted set when params.aborted asks.
 **/
bool flattenGeoUVs(GeoDetailBuilder& detail,
                   const GeoUVFlattenParams& params,
                   GeoUVFlattenReport* report,
                   std::string* errorMessage);

/** @brief "u v" or "u, v"; false when it is neither. */
bool geoUVFlattenParseUV(const std::string& text,
                         GeoUV* uv);

/**
 * @brief Flattens polygons into texture space with the least angular distortion and packs the
 * pieces.
 **/
class GeoUVFlatten
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOUVFLATTEN,
        .label = "GeoUVFlatten",
        .description = "Cut the polygons of the input geometry into islands along seams, flatten each island into "
                       "texture space with the least angular distortion, and pack the islands into the packing "
                       "area, writing the vertex uv. Pins, axis-aligned vertex groups, straightened loops and a "
                       "rectify group steer the flattening; existing uv can keep its seams, its layout or the "
                       "shape of its island boundaries.",
        .grouping = PLUGIN_GROUP_GEOMETRY,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
        .geoGroupKnob = kGeoUVFlattenParamGroup,
        .geoGroupClasses = eGeoGroupClassPrimitives,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoUVFlatten(node);
    }

    explicit GeoUVFlatten(NodePtr node);

    virtual ~GeoUVFlatten();

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

    GeoUVFlattenParams readParams(double time,
                                  ViewSpec view) const;
    void updateEnabled();

    KnobStringWPtr _group;
    KnobStringWPtr _uvAttribute;
    KnobChoiceWPtr _flatteningMethod;
    KnobBoolWPtr _preserveSeams;
    KnobBoolWPtr _preserveLayout;
    KnobBoolWPtr _preserveIslandBoundaryShapes;
    KnobStringWPtr _seams;
    KnobStringWPtr _rectifyGroup;
    KnobBoolWPtr _enablePins;
    KnobRowTableWPtr _pins;
    KnobBoolWPtr _enableAxisAlignment;
    KnobRowTableWPtr _alignments;
    KnobBoolWPtr _enableLoopStraightening;
    KnobRowTableWPtr _straightenings;
    KnobBoolWPtr _enableManualLayout;
    KnobStringWPtr _layoutSeams;
    KnobBoolWPtr _outputSeams;
    KnobStringWPtr _outputSeamsGroup;
    KnobBoolWPtr _outputConstrainedIslands;
    KnobStringWPtr _outputConstrainedIslandsGroup;
    KnobDoubleWPtr _center;
    KnobDoubleWPtr _size;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoUVFlatten_h
