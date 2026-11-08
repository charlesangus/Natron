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

#ifndef Engine_Nodes_Geometry_GeoNormals_h
#define Engine_Nodes_Geometry_GeoNormals_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEONORMALS "GeoNormals"

#define kGeoNormalsParamGroup "group"
#define kGeoNormalsParamAddNormalsTo "addNormalsTo"
#define kGeoNormalsParamWeighting "weighting"
#define kGeoNormalsParamCuspAngle "cuspAngle"
#define kGeoNormalsParamReverse "reverse"

NATRON_NAMESPACE_ENTER

enum GeoNormalsTargetEnum {
    eGeoNormalsTargetPoints = 0,
    eGeoNormalsTargetVertices,
    eGeoNormalsTargetPrimitives,
};

enum GeoNormalsWeightingEnum {
    eGeoNormalsWeightingEqual = 0,
    eGeoNormalsWeightingAngle,
    eGeoNormalsWeightingArea,
};

/** @brief What GeoNormals computes; the defaults are the knobs' defaults. */
struct GeoNormalsParams {
    // A primitive group pattern (see geoSelectElements()); empty for every primitive.
    std::string group;
    GeoNormalsTargetEnum target = eGeoNormalsTargetPoints;
    GeoNormalsWeightingEnum weighting = eGeoNormalsWeightingArea;
    double cuspAngleDegrees = 60.;
    bool reverse = false;
};

/**
 * @brief Computes N (float32 x3, normal type info) on points, vertices or primitives from the
 * polygons. A polygon faces the side its vertices turn counter-clockwise when seen from.
 *
 * Point normals average the normals of the polygons around each point; vertex normals average
 * only the polygons around the vertex's point whose normal is within the cusp angle of the
 * vertex's own polygon, so edges sharper than the cusp angle stay hard. The weighting decides
 * how much each polygon counts: equally, by its corner angle at the point, or by its area.
 *
 * Only the polygons in the group contribute, and only the elements they use are written; the
 * others keep their N. N is removed from the other element classes, which would otherwise
 * shadow or be shadowed by the new one. Polylines and packed primitives contribute nothing.
 **/
class GeoNormals
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = false },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEONORMALS,
        .label = "GeoNormals",
        .description = "Compute normals (N) from the polygons, on the points, the vertices or "
                       "the primitives. Vertex normals keep edges sharper than the cusp angle "
                       "hard. Polygons face the side from which their vertices turn "
                       "counter-clockwise.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoNormals(node);
    }

    explicit GeoNormals(NodePtr node);
    virtual ~GeoNormals();

    /** @brief False with @p error set for a malformed group pattern. */
    static bool computeNormals(GeoDetailBuilder& builder,
                               const GeoNormalsParams& params,
                               std::string* error);

    /** @brief Twice the polygon's area along its normal (Newell's method); zero for other primitives. */
    static std::array<double, 3> polygonAreaNormal(const GeoDetail& detail,
                                                   GeoOffset primitive);

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

    KnobStringWPtr _group;
    KnobChoiceWPtr _addNormalsTo;
    KnobChoiceWPtr _weighting;
    KnobDoubleWPtr _cuspAngle;
    KnobBoolWPtr _reverse;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoNormals_h
