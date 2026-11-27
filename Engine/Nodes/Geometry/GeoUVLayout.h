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

#ifndef Engine_Nodes_Geometry_GeoUVLayout_h
#define Engine_Nodes_Geometry_GeoUVLayout_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoUVPack.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOUVLAYOUT "GeoUVLayout"

#define kGeoUVLayoutParamUVAttribute "uvAttribute"
#define kGeoUVLayoutParamGroup "group"
#define kGeoUVLayoutParamSeparatorEdges "separatorEdges"
#define kGeoUVLayoutParamIslandAttribute "islandAttribute"
#define kGeoUVLayoutParamTargetAttribute "targetAttribute"
#define kGeoUVLayoutParamTargetOverrides "targetOverrides"
#define kGeoUVLayoutParamIslandScaleAttribute "islandScaleAttribute"
#define kGeoUVLayoutParamScaleOverrides "scaleOverrides"
#define kGeoUVLayoutParamIslandOffsetAttribute "islandOffsetAttribute"
#define kGeoUVLayoutParamOffsetOverrides "offsetOverrides"
#define kGeoUVLayoutParamScaleIslandsToMatchSurfaceArea "scaleIslandsToMatchSurfaceArea"
#define kGeoUVLayoutParamAxisAlignment "axisAlignment"
#define kGeoUVLayoutParamScale "scale"
#define kGeoUVLayoutParamFixedScale "fixedScale"
#define kGeoUVLayoutParamRotationStep "rotationStep"
#define kGeoUVLayoutParamCustomRotationDivisions "customRotationDivisions"
#define kGeoUVLayoutParamAvoidPackingOverNonGroupIslands "avoidPackingOverNonGroupIslands"
#define kGeoUVLayoutParamPackInCavities "packInCavities"
#define kGeoUVLayoutParamIslandPadding "islandPadding"
#define kGeoUVLayoutParamPadBoundaries "padBoundaries"
#define kGeoUVLayoutParamSpreadIslands "spreadIslands"
#define kGeoUVLayoutParamIterations "iterations"
#define kGeoUVLayoutParamSearchResolution "searchResolution"
#define kGeoUVLayoutParamConnectivityTolerance "connectivityTolerance"
#define kGeoUVLayoutParamScaleSearchTolerance "scaleSearchTolerance"
#define kGeoUVLayoutParamRandomSeed "randomSeed"
#define kGeoUVLayoutParamPackInto "packInto"
#define kGeoUVLayoutParamUseDefaultTarget "useDefaultTarget"
#define kGeoUVLayoutParamDefaultTarget "defaultTarget"
#define kGeoUVLayoutParamTargetRectangles "targetRectangles"
#define kGeoUVLayoutParamTileSize "tileSize"
#define kGeoUVLayoutParamColumns "columns"
#define kGeoUVLayoutParamStartingUdim "startingUdim"
#define kGeoUVLayoutParamTargetUVAttribute "targetUVAttribute"
#define kGeoUVLayoutParamTargetGroup "targetGroup"
#define kGeoUVLayoutParamTargetSeparatorEdges "targetSeparatorEdges"
#define kGeoUVLayoutParamTargetIslandAttribute "targetIslandAttribute"
#define kGeoUVLayoutParamStackIdenticalIslands "stackIdenticalIslands"
#define kGeoUVLayoutParamMatchMirroredIslands "matchMirroredIslands"
#define kGeoUVLayoutParamAllowStackingOnNonGroupIslands "allowStackingOnNonGroupIslands"
#define kGeoUVLayoutParamShapeTolerance "shapeTolerance"
#define kGeoUVLayoutParamOutputNonpacked "outputNonpacked"
#define kGeoUVLayoutParamNonpackedGroup "nonpackedGroup"
#define kGeoUVLayoutParamOutputIslandNumber "outputIslandNumber"
#define kGeoUVLayoutParamIslandNumberAttribute "islandNumberAttribute"
#define kGeoUVLayoutParamOutputAssignedTarget "outputAssignedTarget"
#define kGeoUVLayoutParamAssignedTargetAttribute "assignedTargetAttribute"

NATRON_NAMESPACE_ENTER

enum GeoUVLayoutAxisAlignmentEnum {
    eGeoUVLayoutAxisAlignmentNone = 0,
    eGeoUVLayoutAxisAlignmentIslandSymmetry,
    eGeoUVLayoutAxisAlignmentIslandPosition,
};

/** @brief Where the islands are found in the main or the target geometry. */
struct GeoUVLayoutIslandSource {
    std::string uvAttribute = "uv";
    // A primitive group pattern; empty for every polygon.
    std::string group;
    // An edge pattern; not empty, islands are cut along these edges instead of by uv.
    std::string separatorEdges;
    std::string islandAttribute;
};

struct GeoUVLayoutParams {
    GeoUVLayoutIslandSource islands;
    std::string targetAttribute;
    // Space-separated polygon:target pairs, later ones winning.
    std::string targetOverrides;
    std::string islandScaleAttribute;
    std::string scaleOverrides;
    std::string islandOffsetAttribute;
    std::string offsetOverrides;
    bool scaleIslandsToMatchSurfaceArea = false;
    GeoUVLayoutAxisAlignmentEnum axisAlignment = eGeoUVLayoutAxisAlignmentIslandSymmetry;
    bool avoidPackingOverNonGroupIslands = false;
    bool allowStackingOnNonGroupIslands = false;
    // Scale, rotations, padding, iterations, resolution, tolerances, seed, targets and
    // stacking; its targetIslands and aborted are filled in here.
    GeoUVPackParams pack;
    double connectivityTolerance = 1e-5;
    GeoUVLayoutIslandSource targets;
    bool outputNonpacked = true;
    std::string nonpackedGroup = "nonpacked";
    bool outputIslandNumber = false;
    std::string islandNumberAttribute = "island";
    bool outputAssignedTarget = false;
    std::string assignedTargetAttribute = "target";
    std::function<bool()> aborted;
};

struct GeoUVLayoutReport {
    int nonpacked = 0;
    int maxUdim = 0;
    // Override tokens naming no polygon of an island, or malformed.
    int ignoredOverrides = 0;
    bool aborted = false;
};

/**
 * @brief Packs the uv islands of the group's polygons into the targets, moving, turning and
 * scaling each as a whole and writing the result back on params.islands.uvAttribute, of the
 * class it is on (vertex, or point). Islands come from uv connectivity within
 * connectivityTolerance, or from the separator edges, merged by the island attribute; a merged
 * group moves as one and is never stacked.
 *
 * Before packing, each island is optionally scaled so its uv area follows its surface area, then
 * by its scale attribute or override, and turned by the axis alignment: Island Symmetry turns its
 * smallest bounding rectangle level with its long side along u; Island Position turns it so the
 * coordinate axes in each polygon's plane run along u and v, weighted by area. Targets come from
 * the target attribute, then the overrides, then the default target. With targetGeometry and
 * Islands targets, the islands of the target geometry are the targets, and the third uv component
 * of what lands on one is raised by that target's least third component; the offset attribute and
 * overrides raise it further.
 *
 * Fails, with *errorMessage set, for a malformed pattern, a missing uv attribute or a missing
 * target geometry; returns false with report->aborted set when params.aborted asks.
 **/
bool layoutGeoUVs(GeoDetailBuilder& detail,
                  const GeoDetail* targetGeometry,
                  const GeoUVLayoutParams& params,
                  GeoUVLayoutReport* report,
                  std::string* errorMessage);

/** @brief Packs uv islands into rectangles, UDIM tiles or the islands of a second geometry. */
class GeoUVLayout
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry },
        InputSpec { .label = "Targets", .kind = eDataKindGeometry, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOUVLAYOUT,
        .label = "GeoUVLayout",
        .description = "Pack the uv islands of the input geometry into rectangles, UDIM tiles or the islands of the "
                       "geometry on the Targets input, moving, turning and scaling each island as a whole. Islands "
                       "come from uv connectivity or separator edges; targets, scales and offsets can be set per "
                       "island, and identical islands can be stacked.",
        .grouping = PLUGIN_GROUP_GEOMETRY,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
        .geoGroupKnob = kGeoUVLayoutParamGroup,
        .geoGroupClasses = eGeoGroupClassPrimitives,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoUVLayout(node);
    }

    explicit GeoUVLayout(NodePtr node);

    virtual ~GeoUVLayout();

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

    GeoUVLayoutParams readParams(double time,
                                 ViewSpec view) const;
    void updateEnabled();

    KnobStringWPtr _uvAttribute;
    KnobStringWPtr _group;
    KnobStringWPtr _separatorEdges;
    KnobStringWPtr _islandAttribute;
    KnobStringWPtr _targetAttribute;
    KnobStringWPtr _targetOverrides;
    KnobStringWPtr _islandScaleAttribute;
    KnobStringWPtr _scaleOverrides;
    KnobStringWPtr _islandOffsetAttribute;
    KnobStringWPtr _offsetOverrides;
    KnobBoolWPtr _scaleIslandsToMatchSurfaceArea;
    KnobChoiceWPtr _axisAlignment;
    KnobChoiceWPtr _scale;
    KnobDoubleWPtr _fixedScale;
    KnobChoiceWPtr _rotationStep;
    KnobIntWPtr _customRotationDivisions;
    KnobBoolWPtr _avoidPackingOverNonGroupIslands;
    KnobBoolWPtr _packInCavities;
    KnobIntWPtr _islandPadding;
    KnobBoolWPtr _padBoundaries;
    KnobBoolWPtr _spreadIslands;
    KnobIntWPtr _iterations;
    KnobIntWPtr _searchResolution;
    KnobDoubleWPtr _connectivityTolerance;
    KnobDoubleWPtr _scaleSearchTolerance;
    KnobIntWPtr _randomSeed;
    KnobChoiceWPtr _packInto;
    KnobBoolWPtr _useDefaultTarget;
    KnobIntWPtr _defaultTarget;
    KnobRowTableWPtr _targetRectangles;
    KnobDoubleWPtr _tileSize;
    KnobIntWPtr _columns;
    KnobIntWPtr _startingUdim;
    KnobStringWPtr _targetUVAttribute;
    KnobStringWPtr _targetGroup;
    KnobStringWPtr _targetSeparatorEdges;
    KnobStringWPtr _targetIslandAttribute;
    KnobBoolWPtr _stackIdenticalIslands;
    KnobBoolWPtr _matchMirroredIslands;
    KnobBoolWPtr _allowStackingOnNonGroupIslands;
    KnobDoubleWPtr _shapeTolerance;
    KnobBoolWPtr _outputNonpacked;
    KnobStringWPtr _nonpackedGroup;
    KnobBoolWPtr _outputIslandNumber;
    KnobStringWPtr _islandNumberAttribute;
    KnobBoolWPtr _outputAssignedTarget;
    KnobStringWPtr _assignedTargetAttribute;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoUVLayout_h
