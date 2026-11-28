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

#ifndef Engine_Nodes_Geometry_ConvertVDB_h
#define Engine_Nodes_Geometry_ConvertVDB_h

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

#define PLUGINID_NATRON_CONVERTVDB "ConvertVDB"

#define kConvertVDBParamGroup "group"
#define kConvertVDBParamConvertTo "convertTo"
#define kConvertVDBParamVDBClass "vdbClass"
#define kConvertVDBParamVDBType "vdbType"
#define kConvertVDBParamVDBPrecision "vdbPrecision"
#define kConvertVDBParamIsovalue "isovalue"
#define kConvertVDBParamFogIsovalue "fogIsovalue"
#define kConvertVDBParamPruneTolerance "pruneTolerance"
#define kConvertVDBParamSignedFloodFill "signedFloodFill"
#define kConvertVDBParamActivateInsideVoxels "activateInsideVoxels"
#define kConvertVDBParamAdaptivity "adaptivity"
#define kConvertVDBParamComputeVertexNormals "computeVertexNormals"
#define kConvertVDBParamInternalAdaptivity "internalAdaptivity"
#define kConvertVDBParamTransferSurfaceAttributes "transferSurfaceAttributes"
#define kConvertVDBParamReferenceAttributes "referenceAttributes"
#define kConvertVDBParamSharpenFeatures "sharpenFeatures"
#define kConvertVDBParamEdgeTolerance "edgeTolerance"
#define kConvertVDBParamSurfaceGroup "surfaceGroup"
#define kConvertVDBParamInteriorGroup "interiorGroup"
#define kConvertVDBParamSeamLineGroup "seamLineGroup"
#define kConvertVDBParamSeamPoints "seamPoints"
#define kConvertVDBParamUseSurfaceMask "useSurfaceMask"
#define kConvertVDBParamSurfaceMask "surfaceMask"
#define kConvertVDBParamMaskOffset "maskOffset"
#define kConvertVDBParamInvertSurfaceMask "invertSurfaceMask"
#define kConvertVDBParamUseAdaptivityField "useAdaptivityField"
#define kConvertVDBParamAdaptivityField "adaptivityField"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kConvertVDBInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindGeometry, .role = InputRole::Source },
    InputSpec { .label = "Reference", .kind = eDataKindGeometry, .role = InputRole::Aux, .optional = true },
    InputSpec { .label = "Mask", .kind = eDataKindGeometry, .role = InputRole::Aux, .optional = true },
};

enum ConvertVDBTargetEnum {
    eConvertVDBTargetPolygons = 0,
    eConvertVDBTargetVDB,
};

enum ConvertVDBClassEnum {
    eConvertVDBClassNoChange = 0,
    eConvertVDBClassFogToSDF,
    eConvertVDBClassSDFToFog,
};

struct ConvertVDBParams {
    // Globs over VDB primitive names, ^ excluding.
    std::string group = "*";
    ConvertVDBTargetEnum target = eConvertVDBTargetPolygons;

    // VDB target: the class conversion, then, without one, the value type and precision.
    ConvertVDBClassEnum vdbClass = eConvertVDBClassNoChange;
    GeoVDBValueTypeEnum vdbType = eGeoVDBValueTypeSame;
    GeoVDBPrecisionEnum vdbPrecision = eGeoVDBPrecisionSame;
    GeoVDBCleanupParams cleanup;

    // The surface level of grids that are not fog volumes.
    double isovalue = 0.;
    // The surface level of fog volumes, for polygons and for an SDF.
    double fogIsovalue = 0.5;
    // Polygons target; its isovalue, surfaceMask and adaptivityField are set per grid.
    GeoSdfToPolygonsParams mesh;
    // VDBs of the mask input, by name pattern, when the matching toggle is on.
    bool useSurfaceMask = false;
    std::string surfaceMask;
    bool useAdaptivityField = false;
    std::string adaptivityField;
};

class ConvertVDB
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_CONVERTVDB,
        .label = "ConvertVDB",
        .description = "Convert the VDB primitives of the input: into polygons along an isosurface, or into "
                       "other VDBs, between signed distance fields and fog volumes or to another value type and "
                       "precision, pruned and flood filled. Other geometry, and VDB primitives the group leaves "
                       "out, pass through. With the polygons the VDB was made from on the Reference input, the "
                       "polygons take its attributes and groups and keep its sharp edges. VDBs on the Mask input "
                       "limit where polygons are made and how far they adapt.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kConvertVDBInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ConvertVDB(node);
    }

    explicit ConvertVDB(NodePtr node);

    virtual ~ConvertVDB();

    /**
     * @brief Converts the VDB primitives of @p builder that match params.group. Polygons replace
     * their primitive and its point; they come from SDFs and fog volumes, and from other float
     * and double grids only when a name in params.group is exactly theirs. VDB conversions
     * replace the grid and keep the name: SDF to fog takes SDFs only, fog to SDF fog volumes
     * only. Grids that cannot convert are left with a note in @p notes, or in @p info when it is
     * set and no name in params.group is exactly theirs, as a glob picking them up is routine.
     * @p reference and @p mask may be null. False with @p error set when OpenVDB fails.
     **/
    static bool apply(GeoDetailBuilder& builder,
                      const ConvertVDBParams& params,
                      const GeoDetail* reference,
                      const GeoDetail* mask,
                      std::vector<std::string>* notes,
                      std::string* error,
                      std::vector<std::string>* info = nullptr);

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

    ConvertVDBParams readParams(double time,
                                ViewSpec view) const;

    KnobStringWPtr _group;
    KnobChoiceWPtr _convertTo;
    KnobChoiceWPtr _vdbClass;
    KnobChoiceWPtr _vdbType;
    KnobChoiceWPtr _vdbPrecision;
    KnobDoubleWPtr _isovalue;
    KnobDoubleWPtr _fogIsovalue;
    KnobDoubleWPtr _pruneTolerance;
    KnobBoolWPtr _signedFloodFill;
    KnobBoolWPtr _activateInsideVoxels;
    KnobDoubleWPtr _adaptivity;
    KnobBoolWPtr _computeVertexNormals;
    KnobDoubleWPtr _internalAdaptivity;
    KnobBoolWPtr _transferSurfaceAttributes;
    KnobStringWPtr _referenceAttributes;
    KnobBoolWPtr _sharpenFeatures;
    KnobDoubleWPtr _edgeTolerance;
    KnobStringWPtr _surfaceGroup;
    KnobStringWPtr _interiorGroup;
    KnobStringWPtr _seamLineGroup;
    KnobStringWPtr _seamPoints;
    KnobBoolWPtr _useSurfaceMask;
    KnobStringWPtr _surfaceMask;
    KnobDoubleWPtr _maskOffset;
    KnobBoolWPtr _invertSurfaceMask;
    KnobBoolWPtr _useAdaptivityField;
    KnobStringWPtr _adaptivityField;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_ConvertVDB_h
