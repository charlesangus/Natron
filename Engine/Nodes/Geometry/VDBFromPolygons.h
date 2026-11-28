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

#ifndef Engine_Nodes_Geometry_VDBFromPolygons_h
#define Engine_Nodes_Geometry_VDBFromPolygons_h

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

#define PLUGINID_NATRON_VDBFROMPOLYGONS "VDBFromPolygons"

#define kVDBFromPolygonsParamReferenceVDB "referenceVDB"
#define kVDBFromPolygonsParamDistanceVDB "distanceVDB"
#define kVDBFromPolygonsParamDistanceName "distanceName"
#define kVDBFromPolygonsParamFogVDB "fogVDB"
#define kVDBFromPolygonsParamFogName "fogName"
#define kVDBFromPolygonsParamUseVoxelCount "useVoxelCount"
#define kVDBFromPolygonsParamVoxelSize "voxelSize"
#define kVDBFromPolygonsParamVoxelCount "voxelCount"
#define kVDBFromPolygonsParamVoxelCountAxis "voxelCountAxis"
#define kVDBFromPolygonsParamUseWorldSpaceForBand "useWorldSpaceForBand"
#define kVDBFromPolygonsParamExteriorBand "exteriorBand"
#define kVDBFromPolygonsParamInteriorBand "interiorBand"
#define kVDBFromPolygonsParamExteriorBandWorld "exteriorBandWorld"
#define kVDBFromPolygonsParamInteriorBandWorld "interiorBandWorld"
#define kVDBFromPolygonsParamFillInterior "fillInterior"
#define kVDBFromPolygonsParamUnsignedDistanceField "unsignedDistanceField"
#define kVDBFromPolygonsParamAttributes "attributes"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kVDBFromPolygonsInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindGeometry, .role = InputRole::Source, .optional = true },
    InputSpec { .label = "Reference", .kind = eDataKindGeometry, .role = InputRole::Aux, .optional = true },
};

struct VDBFromPolygonsParams {
    GeoPolygonsToSdfParams convert;
    // When set, the voxel size fits voxelCount voxels along voxelCountAxis of the source's bounds.
    bool useVoxelCount = false;
    int voxelCount = 100;
    GeoVDBAxisEnum voxelCountAxis = eGeoVDBAxisLongest;
    // The VDB of the reference input whose transform the grids take, by name pattern.
    std::string referenceVDB = "*";
};

class VDBFromPolygons
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_VDBFROMPOLYGONS,
        .label = "VDBFromPolygons",
        .description = "Turn the polygons of the input into a volume: a narrow-band signed distance field, "
                       "negative inside or unsigned, and a fog volume of density 1 inside. Point, vertex and "
                       "primitive attributes can be carried into extra grids of the same voxels. A VDB on the "
                       "Reference input gives the voxel size and orientation. The output holds only the VDB "
                       "primitives, one per grid.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kVDBFromPolygonsInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new VDBFromPolygons(node);
    }

    explicit VDBFromPolygons(NodePtr node);

    virtual ~VDBFromPolygons();

    /**
     * @brief Appends the VDB primitives converted from @p source to @p out and the info notes to
     * @p notes. A VDB of @p reference, which may be null, matching params.referenceVDB gives the
     * transform, over the voxel size or count. False with @p error set for invalid parameters. A
     * voxel count over a source whose points have no extent along the axis gives empty geometry
     * and a note. The voxel estimate is the first note, and goes to @p estimated before the build.
     **/
    static bool build(const GeoDetail& source,
                      const GeoDetail* reference,
                      const VDBFromPolygonsParams& params,
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

    VDBFromPolygonsParams readParams(double time,
                                     ViewSpec view) const;

    KnobStringWPtr _referenceVDB;
    KnobBoolWPtr _distanceVDB;
    KnobStringWPtr _distanceName;
    KnobBoolWPtr _fogVDB;
    KnobStringWPtr _fogName;
    KnobBoolWPtr _useVoxelCount;
    KnobDoubleWPtr _voxelSize;
    KnobIntWPtr _voxelCount;
    KnobChoiceWPtr _voxelCountAxis;
    KnobBoolWPtr _useWorldSpaceForBand;
    KnobDoubleWPtr _exteriorBand;
    KnobDoubleWPtr _interiorBand;
    KnobDoubleWPtr _exteriorBandWorld;
    KnobDoubleWPtr _interiorBandWorld;
    KnobBoolWPtr _fillInterior;
    KnobBoolWPtr _unsignedDistanceField;
    KnobStringWPtr _attributes;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_VDBFromPolygons_h
