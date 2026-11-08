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
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOUVPROJECT "GeoUVProject"

#define kGeoUVProjectParamGroup "group"
#define kGeoUVProjectParamProjection "projection"
#define kGeoUVProjectParamAxis "axis"
#define kGeoUVProjectParamTranslate "translate"
#define kGeoUVProjectParamRotate "rotate"
#define kGeoUVProjectParamScale "scale"
#define kGeoUVProjectParamFitToBounds "fitToBounds"
#define kGeoUVProjectParamCameraPath "cameraPath"
#define kGeoUVProjectParamFitMode "fitMode"
#define kGeoUVProjectParamUseFrameHold "useFrameHold"
#define kGeoUVProjectParamFrameHold "frameHold"
#define kGeoUVProjectParamInvertU "invertU"
#define kGeoUVProjectParamInvertV "invertV"
#define kGeoUVProjectParamUVAttribute "uvAttribute"

NATRON_NAMESPACE_ENTER

enum GeoUVProjectionEnum {
    eGeoUVProjectionPlanar = 0,
    eGeoUVProjectionCamera,
};

enum GeoUVAxisEnum {
    eGeoUVAxisX = 0,
    eGeoUVAxisY,
    eGeoUVAxisZ,
};

struct GeoUVProjectParams {
    std::string group;
    GeoUVProjectionEnum projection = eGeoUVProjectionPlanar;
    GeoUVAxisEnum axis = eGeoUVAxisZ;
    std::array<double, 3> translate = { 0., 0., 0. };
    // Degrees about X, then Y, then Z.
    std::array<double, 3> rotate = { 0., 0., 0. };
    std::array<double, 3> scale = { 1., 1., 1. };
    bool fitToBounds = false;
    bool invertU = false;
    bool invertV = false;
    std::string uvAttribute = "uv";
    // For the camera projection: world points as row vectors [x y z 1] to the camera's frame,
    // 0 to 1 across it after dividing by w.
    std::array<double, 16> cameraWorldToFrame = { 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1. };
};

/**
 * @brief Writes the vertex attribute params.uvAttribute (float32 x3, w = 0, unless one with two
 * or more numeric components exists, whose first two are overwritten) on the vertices of the
 * primitives params.group selects (a group pattern, see geoSelectElements()).
 *
 * Planar: the plane is the unit square centred on translate, rotated and scaled, seen down the
 * axis from its positive side, so a point at its centre gets (0.5, 0.5). Looking down Z, u runs
 * along X and v along Y; down X, along -Z and Y; down Y, along X and -Z. Fit To Bounds then
 * stretches the projected vertices' bounding box to fill 0 to 1. Camera: the frame position
 * through cameraWorldToFrame. Fails, with *errorMessage set, for a malformed group pattern or
 * an empty attribute name.
 **/
bool projectGeoUVs(GeoDetailBuilder& detail,
                   const GeoUVProjectParams& params,
                   std::string* errorMessage);

/**
 * @brief Writes texture coordinates by projecting points onto a plane or through a camera.
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
        .description = "Write texture coordinates (uv) on the vertices of the input geometry by projecting its "
                       "points onto a plane, placed by an axis, translate, rotate and scale, or through a camera "
                       "taken from the scene on the Camera input. The camera projection maps its frame to 0 to 1, "
                       "so an image the camera sees lands where it saw it.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
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

    KnobStringWPtr _group;
    KnobChoiceWPtr _projection;
    KnobChoiceWPtr _axis;
    KnobDoubleWPtr _translate;
    KnobDoubleWPtr _rotate;
    KnobDoubleWPtr _scale;
    KnobBoolWPtr _fitToBounds;
    KnobStringWPtr _cameraPath;
    KnobChoiceWPtr _format;
    KnobIntWPtr _formatSize;
    KnobDoubleWPtr _formatPar;
    KnobChoiceWPtr _fitMode;
    KnobBoolWPtr _useFrameHold;
    KnobDoubleWPtr _frameHold;
    KnobBoolWPtr _invertU;
    KnobBoolWPtr _invertV;
    KnobStringWPtr _uvAttribute;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoUVProject_h
