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

#ifndef Engine_Nodes_Geometry_GeoScatter_h
#define Engine_Nodes_Geometry_GeoScatter_h

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

#define PLUGINID_NATRON_GEOSCATTER "GeoScatter"

#define kGeoScatterParamGroup "group"
#define kGeoScatterParamGenerateBy "generateBy"
#define kGeoScatterParamCount "count"
#define kGeoScatterParamDensity "density"
#define kGeoScatterParamSeed "seed"
#define kGeoScatterParamRelaxPoints "relaxPoints"
#define kGeoScatterParamRelaxIterations "relaxIterations"
#define kGeoScatterParamTransferN "transferN"
#define kGeoScatterParamTransferUV "transferUV"
#define kGeoScatterParamTransferCd "transferCd"
#define kGeoScatterParamOutputSourcePrim "outputSourcePrim"
#define kGeoScatterParamOutputSourcePrimUV "outputSourcePrimUV"

NATRON_NAMESPACE_ENTER

enum GeoScatterGenerateEnum {
    eGeoScatterGenerateByCount = 0,
    eGeoScatterGenerateByDensity,
};

struct GeoScatterParams {
    std::string group;
    GeoScatterGenerateEnum generateBy = eGeoScatterGenerateByCount;
    int count = 1000;
    double density = 10.;
    int seed = 0;
    bool relaxPoints = false;
    int relaxIterations = 10;
    bool transferN = false;
    bool transferUV = false;
    bool transferCd = false;
    bool outputSourcePrim = false;
    bool outputSourcePrimUV = false;
};

/**
 * @brief Appends to @p output points scattered over @p input's polygons, area-weighted, with no
 * primitives. The count is exact. Polygons are split into fans of triangles; a point records
 * (when asked) sourceprim, the index of its polygon, and sourceprimuv: the barycentric weights
 * of the fan triangle's second and third corners, with the triangle's place in the fan as the
 * third component. Relaxing spreads points apart but keeps each on its own triangle. Fails,
 * with *errorMessage set, for a group name that names no primitive group.
 **/
bool scatterGeometry(const GeoDetail& input,
                     const GeoScatterParams& params,
                     GeoDetailBuilder& output,
                     std::string* errorMessage);

/**
 * @brief Scatters points over the polygons of its input.
 **/
class GeoScatter
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOSCATTER,
        .label = "GeoScatter",
        .description = "Scatter points over the polygons of the input geometry, more where the surface is larger, "
                       "as a given count or as a density per unit of area. The output is points only. They can "
                       "carry the surface's normals (N), texture coordinates (uv) and colour (Cd), interpolated at "
                       "each point, and which polygon each came from (sourceprim, sourceprimuv). The same seed "
                       "always gives the same points.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoScatter(node);
    }

    explicit GeoScatter(NodePtr node);

    virtual ~GeoScatter();

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

    GeoScatterParams readParams(double time,
                                ViewSpec view) const;
    void updateEnabled();

    KnobStringWPtr _group;
    KnobChoiceWPtr _generateBy;
    KnobIntWPtr _count;
    KnobDoubleWPtr _density;
    KnobIntWPtr _seed;
    KnobBoolWPtr _relaxPoints;
    KnobIntWPtr _relaxIterations;
    KnobBoolWPtr _transferN;
    KnobBoolWPtr _transferUV;
    KnobBoolWPtr _transferCd;
    KnobBoolWPtr _outputSourcePrim;
    KnobBoolWPtr _outputSourcePrimUV;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoScatter_h
