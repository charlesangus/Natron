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

#ifndef Engine_Nodes_Geometry_VDBFromParticles_h
#define Engine_Nodes_Geometry_VDBFromParticles_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoVDBConvert.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_VDBFROMPARTICLES "VDBFromParticles"

#define kVDBFromParticlesParamReferenceVDB "referenceVDB"
#define kVDBFromParticlesParamMergeWithReferenceVDB "mergeWithReferenceVDB"
#define kVDBFromParticlesParamDistanceVDB "distanceVDB"
#define kVDBFromParticlesParamDistanceName "distanceName"
#define kVDBFromParticlesParamFogVDB "fogVDB"
#define kVDBFromParticlesParamFogName "fogName"
#define kVDBFromParticlesParamMaskVDB "maskVDB"
#define kVDBFromParticlesParamMaskName "maskName"
#define kVDBFromParticlesParamMaskWidthScale "maskWidthScale"
#define kVDBFromParticlesParamVoxelSize "voxelSize"
#define kVDBFromParticlesParamRadiusScale "radiusScale"
#define kVDBFromParticlesParamMinRadius "minRadius"
#define kVDBFromParticlesParamPrune "prune"
#define kVDBFromParticlesParamUseWorldSpaceForBand "useWorldSpaceForBand"
#define kVDBFromParticlesParamBandWidth "bandWidth"
#define kVDBFromParticlesParamBandWidthWorld "bandWidthWorld"
#define kVDBFromParticlesParamParticleFootprint "particleFootprint"
#define kVDBFromParticlesParamVelocityScale "velocityScale"
#define kVDBFromParticlesParamTrailSpacing "trailSpacing"
#define kVDBFromParticlesParamAttributes "attributes"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kVDBFromParticlesInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindGeometry, .role = InputRole::Source, .optional = true },
    InputSpec { .label = "Reference", .kind = eDataKindGeometry, .role = InputRole::Aux, .optional = true },
};

class VDBFromParticles
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_VDBFROMPARTICLES,
        .label = "VDBFromParticles",
        .description = "Turn the points of the input into a volume: the union of a sphere on each point, of "
                       "radius pscale (1 without one) times Radius Scale, as a narrow-band signed distance "
                       "field, a fog volume and a mask of the surface's shell. Velocity trails stretch each sphere "
                       "back along its velocity v. Point attributes can be carried into extra grids. A VDB on the "
                       "Reference input gives the voxel size and orientation and can be merged into. The output "
                       "holds only the VDB primitives, one per grid.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kVDBFromParticlesInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new VDBFromParticles(node);
    }

    explicit VDBFromParticles(NodePtr node);

    virtual ~VDBFromParticles();

    /**
     * @brief Appends the VDB primitives converted from @p source to @p out and the info notes to
     * @p notes. @p reference, which may be null, gives the transform and the grid merged into
     * through the VDB @p referenceVDB names. False with @p error set for invalid parameters. The
     * voxel estimate is the first note, and goes to @p estimated before the build.
     **/
    static bool build(const GeoDetail& source,
                      const GeoDetail* reference,
                      const std::string& referenceVDB,
                      bool mergeWithReferenceVDB,
                      GeoPointsToSdfParams params,
                      GeoDetailBuilder& out,
                      std::vector<std::string>* notes,
                      std::string* error,
                      const GeoVDBEstimateCallback& estimated = GeoVDBEstimateCallback());

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

    GeoPointsToSdfParams readParams(double time,
                                    ViewSpec view) const;

    KnobStringWPtr _referenceVDB;
    KnobBoolWPtr _mergeWithReferenceVDB;
    KnobBoolWPtr _distanceVDB;
    KnobStringWPtr _distanceName;
    KnobBoolWPtr _fogVDB;
    KnobStringWPtr _fogName;
    KnobBoolWPtr _maskVDB;
    KnobStringWPtr _maskName;
    KnobDoubleWPtr _maskWidthScale;
    KnobDoubleWPtr _voxelSize;
    KnobDoubleWPtr _radiusScale;
    KnobDoubleWPtr _minRadius;
    KnobBoolWPtr _prune;
    KnobBoolWPtr _useWorldSpaceForBand;
    KnobDoubleWPtr _bandWidth;
    KnobDoubleWPtr _bandWidthWorld;
    KnobChoiceWPtr _particleFootprint;
    KnobDoubleWPtr _velocityScale;
    KnobDoubleWPtr _trailSpacing;
    KnobStringWPtr _attributes;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_VDBFromParticles_h
