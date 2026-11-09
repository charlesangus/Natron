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

#ifndef Engine_Nodes_Geometry_GeoPrimitive_h
#define Engine_Nodes_Geometry_GeoPrimitive_h

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

#define PLUGINID_NATRON_GEOPRIMITIVE "GeoPrimitive"

#define kGeoPrimitiveParamType "type"
#define kGeoPrimitiveParamCardSize "cardSize"
#define kGeoPrimitiveParamCardRows "cardRows"
#define kGeoPrimitiveParamCardColumns "cardColumns"
#define kGeoPrimitiveParamGridSize "gridSize"
#define kGeoPrimitiveParamGridRows "gridRows"
#define kGeoPrimitiveParamGridColumns "gridColumns"
#define kGeoPrimitiveParamCubeSize "cubeSize"
#define kGeoPrimitiveParamCubeSubdivisions "cubeSubdivisions"
#define kGeoPrimitiveParamSphereRadius "sphereRadius"
#define kGeoPrimitiveParamSphereRows "sphereRows"
#define kGeoPrimitiveParamSphereColumns "sphereColumns"
#define kGeoPrimitiveParamCylinderRadius "cylinderRadius"
#define kGeoPrimitiveParamCylinderHeight "cylinderHeight"
#define kGeoPrimitiveParamCylinderRows "cylinderRows"
#define kGeoPrimitiveParamCylinderColumns "cylinderColumns"
#define kGeoPrimitiveParamCylinderTopCap "cylinderTopCap"
#define kGeoPrimitiveParamCylinderBottomCap "cylinderBottomCap"

NATRON_NAMESPACE_ENTER

enum GeoShapeEnum {
    eGeoShapeCard = 0,
    eGeoShapeGrid,
    eGeoShapeCube,
    eGeoShapeSphere,
    eGeoShapeCylinder,
};

/**
 * @brief What GeoPrimitive builds: the shape and, for each shape, its own sizes and segment
 * counts. Only the chosen shape's fields are read. The defaults are the knobs' defaults.
 **/
struct GeoShapeParams {
    GeoShapeEnum shape = eGeoShapeCard;

    std::array<double, 2> cardSize = { 1., 1. };
    int cardRows = 1;
    int cardColumns = 1;

    std::array<double, 2> gridSize = { 1., 1. };
    int gridRows = 10;
    int gridColumns = 10;

    std::array<double, 3> cubeSize = { 1., 1., 1. };
    std::array<int, 3> cubeSubdivisions = { 2, 2, 2 };

    double sphereRadius = 1.;
    int sphereRows = 30;
    int sphereColumns = 30;

    double cylinderRadius = 1.;
    double cylinderHeight = 2.;
    int cylinderRows = 30;
    int cylinderColumns = 30;
    bool cylinderTopCap = true;
    bool cylinderBottomCap = true;
};

/**
 * @brief Appends the shape to \p detail as polygons over one shared point pool, Y up. The
 * vertex class carries uv (float32 x3, w = 0, spanning 0 to 1) and N (float32 x3, normal), so a
 * hard edge keeps one normal per side. Segment counts below a shape's minimum are raised to it.
 *
 * A card stands in the XY plane facing +Z; a grid lies in the XZ plane facing +Y, its rows
 * along -Z. The sphere, cube and cylinder are closed.
 **/
void buildGeoShape(const GeoShapeParams& params,
                   GeoDetailBuilder& detail);

/**
 * @brief A generator of native geometry: a card, grid, cube, sphere or cylinder.
 **/
class GeoPrimitive
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOPRIMITIVE,
        .label = "GeoPrimitive",
        .description = "Create geometry holding a card, grid, cube, sphere or cylinder: polygons "
                       "over one pool of points, with texture coordinates (uv) and normals (N) "
                       "on the vertices. The card stands in the XY plane facing +Z and the grid "
                       "lies in the XZ plane facing +Y; the others are centred on the origin "
                       "with Y up.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoPrimitive(node);
    }

    explicit GeoPrimitive(NodePtr node);

    virtual ~GeoPrimitive();

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

    GeoShapeParams readParams(double time,
                              ViewSpec view) const;
    void updateVisibility();

    KnobChoiceWPtr _type;
    KnobDoubleWPtr _cardSize;
    KnobIntWPtr _cardRows;
    KnobIntWPtr _cardColumns;
    KnobDoubleWPtr _gridSize;
    KnobIntWPtr _gridRows;
    KnobIntWPtr _gridColumns;
    KnobDoubleWPtr _cubeSize;
    KnobIntWPtr _cubeSubdivisions;
    KnobDoubleWPtr _sphereRadius;
    KnobIntWPtr _sphereRows;
    KnobIntWPtr _sphereColumns;
    KnobDoubleWPtr _cylinderRadius;
    KnobDoubleWPtr _cylinderHeight;
    KnobIntWPtr _cylinderRows;
    KnobIntWPtr _cylinderColumns;
    KnobBoolWPtr _cylinderTopCap;
    KnobBoolWPtr _cylinderBottomCap;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoPrimitive_h
