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

#ifndef Engine_Nodes_Geometry_GeoCopyToPoints_h
#define Engine_Nodes_Geometry_GeoCopyToPoints_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoPacked.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOCOPYTOPOINTS "GeoCopyToPoints"

#define kGeoCopyToPointsParamSourceGroup "sourceGroup"
#define kGeoCopyToPointsParamTargetGroup "targetGroup"
#define kGeoCopyToPointsParamPackAndInstance "packAndInstance"
#define kGeoCopyToPointsParamPivot "pivot"
#define kGeoCopyToPointsParamUseTargetOrientations "useTargetOrientations"
#define kGeoCopyToPointsParamUseImplicitNormal "useImplicitNormal"
#define kGeoCopyToPointsParamTargetAttributes "targetAttributes"
#define kGeoCopyToPointsParamApplyTo "applyTo"

NATRON_NAMESPACE_ENTER

enum GeoCopyPivotEnum {
    eGeoCopyPivotOrigin = 0,
    eGeoCopyPivotCentroid,
};

enum GeoCopyApplyToEnum {
    eGeoCopyApplyToPoints = 0,
    eGeoCopyApplyToVertices,
    eGeoCopyApplyToPrimitives,
};

inline constexpr const char* kGeoCopyDefaultTargetAttributes = "* ^v ^Alpha ^N ^up ^pscale ^scale ^orient ^rot ^pivot ^trans ^transform";

struct GeoCopyToPointsParams {
    std::string sourceGroup;
    std::string targetGroup;
    bool packAndInstance = false;
    GeoCopyPivotEnum pivot = eGeoCopyPivotCentroid;
    bool useTargetOrientations = true;
    bool useImplicitNormal = true;
    std::string targetAttributes = kGeoCopyDefaultTargetAttributes;
    GeoCopyApplyToEnum applyTo = eGeoCopyApplyToPoints;
};

/**
 * @brief The matrix placing a copy on @p point of @p target, applied to column vectors:
 * translate by P + trans, align (transform, else orient, else +Z to N or v with +Y towards up),
 * rotate by rot, scale by scale * pscale, then translate by -pivot. transform, a 3x3 or 4x4
 * matrix acting on row vectors, replaces the alignment, rot and scale. Without
 * @p useOrientations only P and trans apply. @p implicitNormal stands in for N when given and
 * the point has no N, orient or transform.
 **/
GeoMatrix4 geoCopyPointTransform(const GeoDetail& target,
                                 GeoOffset point,
                                 bool useOrientations,
                                 const std::array<double, 3>* implicitNormal);

/**
 * @brief Appends to @p output one copy of @p source's primitives in the source group, and the
 * points they use, per target point in the target group. Expanded copies carry the source's
 * attributes and groups, with positions, vectors and normals transformed. Packed, each copy is
 * one packed geometry primitive and all of them embed the same detail. The target attributes
 * the pattern selects are then copied onto every element of the apply-to class of each copy.
 * Fails, with *errorMessage set, for a group name that names no group.
 **/
bool copyGeometryToPoints(const GeoDetailConstPtr& source,
                          const GeoDetail& target,
                          const GeoCopyToPointsParams& params,
                          GeoDetailBuilder& output,
                          std::string* errorMessage);

/**
 * @brief Copies geometry onto points.
 **/
class GeoCopyToPoints
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry },
        InputSpec { .label = "Target", .kind = eDataKindGeometry },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOCOPYTOPOINTS,
        .label = "GeoCopyToPoints",
        .description = "Copy the Source geometry onto every point of the Target. Each copy is moved to its point "
                       "and, from the point's attributes, oriented (transform, orient, or N with up), rotated "
                       "(rot), scaled (pscale, scale), offset (trans) and pivoted (pivot). The copies are either "
                       "real geometry, or packed primitives that all share the Source, which costs almost "
                       "nothing per copy. Chosen attributes of each target point are copied onto its copy.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoCopyToPoints(node);
    }

    explicit GeoCopyToPoints(NodePtr node);

    virtual ~GeoCopyToPoints();

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

    GeoCopyToPointsParams readParams(double time,
                                     ViewSpec view) const;
    void updateEnabled();

    KnobStringWPtr _sourceGroup;
    KnobStringWPtr _targetGroup;
    KnobBoolWPtr _packAndInstance;
    KnobChoiceWPtr _pivot;
    KnobBoolWPtr _useTargetOrientations;
    KnobBoolWPtr _useImplicitNormal;
    KnobStringWPtr _targetAttributes;
    KnobChoiceWPtr _applyTo;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoCopyToPoints_h
