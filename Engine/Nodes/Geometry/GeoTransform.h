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

#ifndef Engine_Nodes_Geometry_GeoTransform_h
#define Engine_Nodes_Geometry_GeoTransform_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <optional>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOTRANSFORM "GeoTransform"

#define kGeoTransformParamGroup "group"
#define kGeoTransformParamGroupType "groupType"
#define kGeoTransformParamTranslate "translate"
#define kGeoTransformParamRotate "rotate"
#define kGeoTransformParamScale "scale"
#define kGeoTransformParamUniformScale "uniformScale"
#define kGeoTransformParamRotateOrder "rotateOrder"
#define kGeoTransformParamPivot "pivot"
#define kGeoTransformParamCentrePivot "centrePivot"
#define kGeoTransformParamInvertTransform "invertTransform"
#define kGeoTransformParamAttributes "attributes"
#define kGeoTransformParamKeepVectorLengths "keepVectorLengths"

NATRON_NAMESPACE_ENTER

/** @brief What GeoTransform does; the defaults are the knobs' defaults. */
struct GeoTransformParams {
    // A group pattern (see geoSelectElements()) over groupType's class; empty for everything.
    std::string group;
    GeoGroupTypeEnum groupType = eGeoGroupTypePrimitives;
    std::array<double, 3> translate = { 0., 0., 0. };
    std::array<double, 3> rotate = { 0., 0., 0. };
    std::array<double, 3> scale = { 1., 1., 1. };
    double uniformScale = 1.;
    GeoRotateOrderEnum rotateOrder = eGeoRotateOrderXYZ;
    std::array<double, 3> pivot = { 0., 0., 0. };
    bool invert = false;
    // Space-separated globs, '^' excluding, over the attributes transformed besides P.
    std::string attributes = "*";
    bool keepVectorLengths = true;
};

/**
 * @brief Moves, turns and scales geometry, and every attribute whose type info says how:
 *   - point type info (P among them): the full transform;
 *   - vector: the transform without its translation, keeping its length when asked;
 *   - normal: the inverse transpose, keeping its length, so normals stay perpendicular to the
 *     surface under non-uniform scale;
 *   - quaternion (x, y, z, w): turned by the rotation;
 *   - matrix, 3x3 or 4x4 (row-major, applied to column vectors): multiplied by the transform.
 * Colour, untyped, integer, string and array attributes are left alone, as are the packed
 * intrinsics, and packed primitives get their transform multiplied by this one instead.
 *
 * A point group transforms its points, the vertices on them, the primitives all of whose
 * points it holds and the packed primitives whose pivot it holds. A primitive group transforms
 * its primitives, their vertices and their points. Detail attributes change only when the group
 * is empty. Pages no write changes stay shared with the input.
 **/
class GeoTransform
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = false },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOTRANSFORM,
        .label = "GeoTransform",
        .description = "Move, turn and scale geometry about a pivot, or only a group of it. "
                       "Attributes tagged as points, vectors, normals, quaternions or matrices "
                       "follow the transform; packed primitives have their transform multiplied "
                       "by it.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoTransform(node);
    }

    explicit GeoTransform(NodePtr node);
    virtual ~GeoTransform();

    /** @brief The transform the params describe, inverted when asked; nullopt when singular and inverted. */
    static std::optional<GeoMatrix4> matrix(const GeoTransformParams& params);

    /** @brief False with @p error set for a malformed group pattern or a singular inverted transform. */
    static bool apply(GeoDetailBuilder& builder,
                      const GeoTransformParams& params,
                      std::string* error);

    /**
     * @brief Applies @p transform to the selected elements. @p rotation is the transform's
     * rotation alone, used for quaternions.
     **/
    static bool applyMatrix(GeoDetailBuilder& builder,
                            const GeoMatrix4& transform,
                            const GeoMatrix4& rotation,
                            const std::string& group,
                            GeoGroupTypeEnum groupType,
                            const std::string& attributes,
                            bool keepVectorLengths,
                            std::string* error);

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    GeoTransformParams readParams(double time,
                                  ViewSpec view) const;
    bool centrePivot(std::string* errorMessage);

    KnobStringWPtr _group;
    KnobChoiceWPtr _groupType;
    KnobDoubleWPtr _translate;
    KnobDoubleWPtr _rotate;
    KnobDoubleWPtr _scale;
    KnobDoubleWPtr _uniformScale;
    KnobChoiceWPtr _rotateOrder;
    KnobDoubleWPtr _pivot;
    KnobButtonWPtr _centrePivot;
    KnobBoolWPtr _invert;
    KnobStringWPtr _attributes;
    KnobBoolWPtr _keepVectorLengths;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoTransform_h
