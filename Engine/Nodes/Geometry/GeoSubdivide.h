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

#ifndef Engine_Nodes_Geometry_GeoSubdivide_h
#define Engine_Nodes_Geometry_GeoSubdivide_h

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

#define PLUGINID_NATRON_GEOSUBDIVIDE "GeoSubdivide"

#define kGeoSubdivideParamGroup "group"
#define kGeoSubdivideParamAlgorithm "algorithm"
#define kGeoSubdivideParamDepth "depth"
#define kGeoSubdivideParamBoundaryInterpolation "boundaryInterpolation"
#define kGeoSubdivideParamVertexInterpolation "vertexInterpolation"
#define kGeoSubdivideParamRecomputeNormals "recomputeNormals"

NATRON_NAMESPACE_ENTER

enum GeoSubdivideAlgorithmEnum {
    eGeoSubdivideCatmullClark = 0,
    eGeoSubdivideLoop,
    eGeoSubdivideBilinear,
};

enum GeoSubdivideBoundaryEnum {
    eGeoSubdivideBoundaryEdgesOnly = 0,
    eGeoSubdivideBoundaryEdgesAndCorners,
};

// How vertex attributes, uv first of all, are smoothed where their values split along seams and
// borders: from smooth everywhere to linear everywhere.
enum GeoSubdivideVertexInterpolationEnum {
    eGeoSubdivideVertexSmooth = 0,
    eGeoSubdivideVertexCornersOnly,
    eGeoSubdivideVertexCornersPlus1,
    eGeoSubdivideVertexCornersPlus2,
    eGeoSubdivideVertexBoundaries,
    eGeoSubdivideVertexLinear,
};

struct GeoSubdivideParams {
    // A primitive group pattern (see geoSelectElements()); empty for every polygon.
    std::string group;
    GeoSubdivideAlgorithmEnum algorithm = eGeoSubdivideCatmullClark;
    int depth = 1;
    GeoSubdivideBoundaryEnum boundary = eGeoSubdivideBoundaryEdgesAndCorners;
    GeoSubdivideVertexInterpolationEnum vertexInterpolation = eGeoSubdivideVertexCornersPlus1;
    bool recomputeNormals = true;
};

/**
 * @brief Replaces the polygons the group selects by their subdivision surface, refined depth
 * times (1 to 4). Catmull-Clark turns every polygon into quads, Loop needs triangles and keeps
 * them, Bilinear splits without smoothing.
 *
 * Point attributes, positions included, follow the refinement rule; vertex attributes are
 * interpolated face-varying, so values that differ at a point (uv seams, hard normals) stay
 * apart. Every child face carries its parent's primitive attributes and primitive groups; a new
 * point joins the point groups of the point it is mostly made of. With recomputeNormals, an N
 * the detail had is computed afresh from the new faces on the same element class instead.
 * Other primitives, and points only they use, are untouched; vertex groups and edge groups on
 * the replaced polygons are not carried. Fails, with *errorMessage set, for a malformed group,
 * non-triangles under Loop, or topology the refiner rejects.
 **/
bool subdivideGeometry(GeoDetailBuilder& detail,
                       const GeoSubdivideParams& params,
                       std::string* errorMessage);

/**
 * @brief Smooths polygons into a subdivision surface.
 **/
class GeoSubdivide
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOSUBDIVIDE,
        .label = "GeoSubdivide",
        .description = "Refine polygons into a smooth subdivision surface: Catmull-Clark for any polygons, Loop for "
                       "triangles, or Bilinear to split faces without smoothing. Point and vertex attributes, uv "
                       "included, are interpolated across the new faces, which keep their parent's primitive "
                       "attributes and groups.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoSubdivide(node);
    }

    explicit GeoSubdivide(NodePtr node);

    virtual ~GeoSubdivide();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobStringWPtr _group;
    KnobChoiceWPtr _algorithm;
    KnobIntWPtr _depth;
    KnobChoiceWPtr _boundary;
    KnobChoiceWPtr _vertexInterpolation;
    KnobBoolWPtr _recomputeNormals;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoSubdivide_h
