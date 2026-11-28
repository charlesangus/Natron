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

#ifndef Engine_Nodes_Geometry_VDBResample_h
#define Engine_Nodes_Geometry_VDBResample_h

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

#define PLUGINID_NATRON_VDBRESAMPLE "VDBResample"

#define kVDBResampleParamGroup "group"
#define kVDBResampleParamReferenceVDB "referenceVDB"
#define kVDBResampleParamInterpolation "interpolation"
#define kVDBResampleParamMode "mode"
#define kVDBResampleParamTransformOrder "transformOrder"
#define kVDBResampleParamRotateOrder "rotateOrder"
#define kVDBResampleParamTranslateVoxels "translateVoxels"
#define kVDBResampleParamRotate "rotate"
#define kVDBResampleParamScale "scale"
#define kVDBResampleParamPivot "pivot"
#define kVDBResampleParamVoxelSize "voxelSize"
#define kVDBResampleParamVoxelScale "voxelScale"
#define kVDBResampleParamForceAxisAligned "forceAxisAligned"
#define kVDBResampleParamTransformVectors "transformVectors"
#define kVDBResampleParamRebuildSDF "rebuildSDF"
#define kVDBResampleParamPruneTolerance "pruneTolerance"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kVDBResampleInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindGeometry, .role = InputRole::Source },
    InputSpec { .label = "Reference", .kind = eDataKindGeometry, .role = InputRole::Aux, .optional = true },
};

enum VDBResampleModeEnum {
    eVDBResampleModeVoxelSize = 0,
    eVDBResampleModeReference,
    eVDBResampleModeVoxelScale,
    eVDBResampleModeExplicit,
};

struct VDBResampleParams {
    // Globs over VDB primitive names, ^ excluding.
    std::string group = "*";
    VDBResampleModeEnum mode = eVDBResampleModeVoxelSize;
    double voxelSize = 0.1;
    double voxelScale = 2.;
    // Voxel Size and Voxel Scale modes: a new axis-aligned transform instead of the grid's own.
    bool forceAxisAligned = false;
    // The VDB of the reference input whose transform Reference mode takes, by name pattern.
    std::string referenceVDB = "*";
    // Explicit mode.
    GeoVDBIndexTransform transform;
    GeoVDBResampleParams resample;
};

class VDBResample
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_VDBRESAMPLE,
        .label = "VDBResample",
        .description = "Resample the VDB primitives of the input onto other voxels: of a given size, of a VDB "
                       "on the Reference input, scaled from their own, or moved by an explicit transform in index "
                       "space. Signed distance fields can be rebuilt with the same band width in voxels; other "
                       "grids are sampled. Other geometry, and VDB primitives the group leaves out, pass through.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kVDBResampleInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new VDBResample(node);
    }

    explicit VDBResample(NodePtr node);

    virtual ~VDBResample();

    /**
     * @brief Resamples the VDB primitives of @p builder that match params.group, keeping their
     * names. @p reference is the detail whose VDB matching params.referenceVDB gives the voxels in
     * reference mode; it may be null otherwise. Grids that cannot be resampled are left with a note in
     * @p notes. False with @p error set for a voxel size or scale that is not positive and finite,
     * reference mode without a matching reference VDB, or an explicit transform that cannot be
     * inverted.
     **/
    static bool apply(GeoDetailBuilder& builder,
                      const VDBResampleParams& params,
                      const GeoDetail* reference,
                      std::vector<std::string>* notes,
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

    VDBResampleParams readParams(double time,
                                 ViewSpec view) const;

    KnobStringWPtr _group;
    KnobStringWPtr _referenceVDB;
    KnobChoiceWPtr _interpolation;
    KnobChoiceWPtr _mode;
    KnobChoiceWPtr _transformOrder;
    KnobChoiceWPtr _rotateOrder;
    KnobDoubleWPtr _translateVoxels;
    KnobDoubleWPtr _rotate;
    KnobDoubleWPtr _scale;
    KnobDoubleWPtr _pivot;
    KnobDoubleWPtr _voxelSize;
    KnobDoubleWPtr _voxelScale;
    KnobBoolWPtr _forceAxisAligned;
    KnobBoolWPtr _transformVectors;
    KnobBoolWPtr _rebuildSDF;
    KnobDoubleWPtr _pruneTolerance;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_VDBResample_h
