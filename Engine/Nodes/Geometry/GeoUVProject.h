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

#ifndef Engine_Nodes_Geometry_GeoUVProject_h
#define Engine_Nodes_Geometry_GeoUVProject_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOUVPROJECT "GeoUVProject"

#define kGeoUVProjectParamUVAttribute "uvAttribute"
#define kGeoUVProjectParamGroup "group"
#define kGeoUVProjectParamGroupAttribType "groupAttribType"
#define kGeoUVProjectParamProjection "projection"
#define kGeoUVProjectParamInnerRadius "innerRadius"
#define kGeoUVProjectParamTransformOrder "transformOrder"
#define kGeoUVProjectParamRotateOrder "rotateOrder"
#define kGeoUVProjectParamTranslate "translate"
#define kGeoUVProjectParamRotate "rotate"
#define kGeoUVProjectParamScale "scale"
#define kGeoUVProjectParamPivot "pivot"
#define kGeoUVProjectParamInitializeMethod "initializeMethod"
#define kGeoUVProjectParamInitialize "initialize"
#define kGeoUVProjectParamURange "uRange"
#define kGeoUVProjectParamVRange "vRange"
#define kGeoUVProjectParamAngle "angle"
#define kGeoUVProjectParamFixBoundarySeams "fixBoundarySeams"
#define kGeoUVProjectParamFixPoles "fixPoles"
#define kGeoUVProjectParamPoleRadius "poleRadius"
#define kGeoUVProjectParamCameraPath "cameraPath"
#define kGeoUVProjectParamFitMode "fitMode"
#define kGeoUVProjectParamUseFrameHold "useFrameHold"
#define kGeoUVProjectParamFrameHold "frameHold"

NATRON_NAMESPACE_ENTER

enum GeoUVProjectionEnum {
    eGeoUVProjectionOrthographic = 0,
    eGeoUVProjectionPolar,
    eGeoUVProjectionCylindrical,
    eGeoUVProjectionToroidal,
    eGeoUVProjectionPlasticWrap,
    eGeoUVProjectionCamera,
};

/** @brief The class the Group selects, which is also the class the uv is written on. */
enum GeoUVProjectGroupAttribTypeEnum {
    eGeoUVProjectGroupAttribTypePoints = 0,
    eGeoUVProjectGroupAttribTypeVertices,
};

/** @brief The order scale, rotate and translate apply in: SRT scales first and translates last. */
enum GeoTransformOrderEnum {
    eGeoTransformOrderSRT = 0,
    eGeoTransformOrderSTR,
    eGeoTransformOrderRST,
    eGeoTransformOrderRTS,
    eGeoTransformOrderTSR,
    eGeoTransformOrderTRS,
};

enum GeoUVProjectInitializeMethodEnum {
    // Translate to the bounds' centre and scale each axis to the bounds' size along it.
    eGeoUVProjectInitializeBoundingBox = 0,
    // The same with every axis scaled to the bounds' largest size, so the texture keeps its aspect.
    eGeoUVProjectInitializeUniformBoundingBox,
};

/** @brief Where the projection geometry sits: about the pivot, in transformOrder and rotateOrder. */
struct GeoUVProjectTransform {
    GeoTransformOrderEnum transformOrder = eGeoTransformOrderSRT;
    GeoRotateOrderEnum rotateOrder = eGeoRotateOrderXYZ;
    std::array<double, 3> translate = { 0., 0., 0. };
    // Degrees.
    std::array<double, 3> rotate = { 0., 0., 0. };
    std::array<double, 3> scale = { 1., 1., 1. };
    std::array<double, 3> pivot = { 0., 0., 0. };
};

struct GeoUVProjectParams {
    std::string uvAttribute = "uv";
    // A group pattern over primitives for the vertex type, over points for the point type; empty
    // selects every element.
    std::string group;
    GeoUVProjectGroupAttribTypeEnum groupAttribType = eGeoUVProjectGroupAttribTypeVertices;
    GeoUVProjectionEnum projection = eGeoUVProjectionOrthographic;
    // The torus's hole radius; its outer radius is 0.5.
    double innerRadius = 0.2;
    GeoUVProjectTransform transform;
    // The places on the projection geometry, 0 to 1 across it, of the texture's left and right
    // edges and of its bottom and top edges.
    std::array<double, 2> uRange = { 0., 1. };
    std::array<double, 2> vRange = { 0., 1. };
    // Degrees, counter-clockwise about (0.5, 0.5) in uv.
    double angle = 0.;
    bool fixBoundarySeams = true;
    bool fixPoles = true;
    // In the projection geometry's own space, where the sphere and cylinder have radius 0.5.
    double poleRadius = 0.01;
    // For the camera projection: world points as row vectors [x y z 1] to the camera's frame,
    // 0 to 1 across it after dividing by w.
    std::array<double, 16> cameraWorldToFrame = { 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1. };
};

/** @brief The projection geometry's placement, mapping its own space to world space. */
GeoMatrix4 geoUVProjectMatrix(const GeoUVProjectTransform& transform);

/**
 * @brief Writes the attribute params.uvAttribute (float32 x3, w = 0, unless one with two or more
 * numeric components exists, whose first two are overwritten) on the elements params.group
 * selects. The vertex type reads the group over primitives and writes the vertices of the
 * selected polygons and polylines; other primitives keep their vertices' uv. The point type reads
 * it over points and writes the selected points, including those no primitive uses. Writing
 * vertices, a point attribute of the name is removed, its values seeding the vertices not
 * written; writing points, a vertex attribute of the name, which would shadow the point one, is
 * removed, and a point not written takes the value its lowest-numbered vertex had.
 *
 * Each projection maps a point, taken into the projection geometry's own space by the inverse
 * of geoUVProjectMatrix(), to a place 0 to 1 across that geometry:
 *   - Orthographic: the unit square in XY centred on the origin, seen down Z: (x + 0.5, y + 0.5).
 *   - Polar: the sphere about the origin with its poles on Y: longitude atan2(x, z) / 2pi + 0.5,
 *     so +Z gets 0.5 and the seam is at -Z, and latitude asin(y / |p|) / pi + 0.5.
 *   - Cylindrical: the cylinder of radius 0.5 and height 1 about Y: longitude as for Polar and
 *     y + 0.5.
 *   - Toroidal: the torus about Y of inner radius innerRadius and outer radius 0.5: longitude as
 *     for Polar and the angle round the tube, 0.5 on its outside, the seam on its inside.
 *   - Plastic Wrap: the direction from the origin, +Z at (0.5, 0.5), wrapping out to the square's
 *     edge at -Z; the angle from +Z sets the distance from the centre.
 *   - Camera: the frame position through cameraWorldToFrame, untransformed.
 * The place then goes to uv through the U and V Ranges, (s - range[0]) / (range[1] - range[0]),
 * and is turned by angle about (0.5, 0.5).
 *
 * Fix Boundary Seams, on the vertex type, unwraps a polygon's longitudes (and the Toroidal tube
 * angle) so its vertices never jump across the seam, the polygon's lowest value staying in
 * 0 to 1. Fix Poles gives a vertex within poleRadius of a pole, where the longitude (or for Plastic
 * Wrap both coordinates) is undefined, the mean of its polygon's nearest neighbours not at the
 * pole on either side; on the point type, of the points it shares an edge with.
 *
 * Fails, with *errorMessage set, for a malformed group pattern, an empty attribute name, an empty
 * U or V Range or a singular transform.
 **/
bool projectGeoUVs(GeoDetailBuilder& detail,
                   const GeoUVProjectParams& params,
                   std::string* errorMessage);

/**
 * @brief The transform that fits the projection geometry to the bounds of the points params.group
 * selects (those of the selected primitives for the vertex type): translate to the centre and
 * scale by method, with no rotation and the pivot at the origin, the translate allowing for
 * params.transform.transformOrder, which it keeps along with the rotate order. A flat side gets
 * scale 1. Fails, with *errorMessage set, for a malformed pattern or when it selects no point.
 **/
bool geoUVProjectInitialize(const GeoDetail& detail,
                            const GeoUVProjectParams& params,
                            GeoUVProjectInitializeMethodEnum method,
                            GeoUVProjectTransform* transform,
                            std::string* errorMessage);

/**
 * @brief Writes texture coordinates by projecting points onto a plane, sphere, cylinder, torus or
 * wrap, or through a camera.
 **/
class GeoUVProject
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry },
        InputSpec { .label = "Camera", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOUVPROJECT,
        .label = "GeoUVProject",
        .description = "Write texture coordinates (uv) on the vertices of the input geometry, or on its points, by "
                       "projecting its points onto a plane, sphere, cylinder, torus or plastic wrap placed by the "
                       "transform, or through a camera taken from the scene on the Camera input. The camera "
                       "projection maps its frame to 0 to 1, so an image the camera sees lands where it saw it. "
                       "Writing vertices lets the seams of the round projections be fixed polygon by polygon.",
        .grouping = PLUGIN_GROUP_GEOMETRY,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
        .geoGroupKnob = kGeoUVProjectParamGroup,
        .geoGroupClasses = eGeoGroupClassPoints | eGeoGroupClassPrimitives,
        .geoGroupTypeKnob = kGeoUVProjectParamGroupAttribType,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoUVProject(node);
    }

    explicit GeoUVProject(NodePtr node);

    virtual ~GeoUVProject();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
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

    GeoUVProjectParams readParams(double time,
                                  ViewSpec view) const;
    double getFormatAspect() const;
    void updateEnabled();
    bool initializeTransform(std::string* errorMessage);

    KnobStringWPtr _uvAttribute;
    KnobStringWPtr _group;
    KnobChoiceWPtr _groupAttribType;
    KnobChoiceWPtr _projection;
    KnobDoubleWPtr _innerRadius;
    KnobChoiceWPtr _transformOrder;
    KnobChoiceWPtr _rotateOrder;
    KnobDoubleWPtr _translate;
    KnobDoubleWPtr _rotate;
    KnobDoubleWPtr _scale;
    KnobDoubleWPtr _pivot;
    KnobChoiceWPtr _initializeMethod;
    KnobButtonWPtr _initialize;
    KnobDoubleWPtr _uRange;
    KnobDoubleWPtr _vRange;
    KnobDoubleWPtr _angle;
    KnobBoolWPtr _fixBoundarySeams;
    KnobBoolWPtr _fixPoles;
    KnobDoubleWPtr _poleRadius;
    KnobStringWPtr _cameraPath;
    KnobChoiceWPtr _format;
    KnobIntWPtr _formatSize;
    KnobDoubleWPtr _formatPar;
    KnobChoiceWPtr _fitMode;
    KnobBoolWPtr _useFrameHold;
    KnobDoubleWPtr _frameHold;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoUVProject_h
