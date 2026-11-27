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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "GeoPrimitive.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <QString>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/PrimitiveMeshData.h"

NATRON_NAMESPACE_ENTER

namespace {
// The card's mesh stands in XY facing +Z; a grid is that quad laid down onto XZ, facing +Y.
PrimitiveMeshData
buildPlane(const std::array<double, 2>& size,
           int rows,
           int columns,
           bool lieFlat)
{
    PrimitiveMeshData mesh = PrimitiveMeshes::buildCardData(1., 1., rows, columns, SceneMetadata::eUpAxisY);
    for (std::array<float, 3>& p : mesh.points) {
        p[0] *= (float)size[0];
        p[1] *= (float)size[1];
    }
    if (lieFlat) {
        // A quarter turn about X taking +Z to +Y: (x, y, z) -> (x, z, -y).
        for (std::array<float, 3>& p : mesh.points) {
            p = { p[0], p[2], -p[1] };
        }
        for (std::array<float, 3>& n : mesh.normals) {
            n = { n[0], n[2], -n[1] };
        }
    }

    return mesh;
}

PrimitiveMeshData
buildMesh(const GeoShapeParams& params)
{
    const SceneMetadata::UpAxisEnum up = SceneMetadata::eUpAxisY;
    switch (params.shape) {
    case eGeoShapeCard:
        return buildPlane(params.cardSize, params.cardRows, params.cardColumns, false);
    case eGeoShapeGrid:
        return buildPlane(params.gridSize, params.gridRows, params.gridColumns, true);
    case eGeoShapeCube: {
        const int segments[3] = { params.cubeSubdivisions[0], params.cubeSubdivisions[1], params.cubeSubdivisions[2] };

        return PrimitiveMeshes::buildCubeData(params.cubeSize, segments, up);
    }
    case eGeoShapeSphere:
        return PrimitiveMeshes::buildSphereData(params.sphereRadius, params.sphereRows, params.sphereColumns, up);
    case eGeoShapeCylinder:
        return PrimitiveMeshes::buildCylinderData(params.cylinderRadius, params.cylinderHeight, params.cylinderRows,
                                                  params.cylinderColumns, params.cylinderTopCap, params.cylinderBottomCap, up);
    }

    return PrimitiveMeshData();
}

KnobDoublePtr
createSizeKnob(KnobHolder* holder,
               const KnobPagePtr& page,
               const QString& label,
               const char* name,
               const QString& toolTip,
               const std::vector<double>& defaults)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label.toStdString(), (int)defaults.size());

    knob->setName(name);
    knob->setHintToolTip(toolTip.toStdString());
    for (std::size_t d = 0; d < defaults.size(); ++d) {
        knob->setDefaultValue(defaults[d], (int)d);
        knob->setMinimum(0., (int)d);
    }
    page->addKnob(knob);

    return knob;
}

KnobIntPtr
createSegmentsKnob(KnobHolder* holder,
                   const KnobPagePtr& page,
                   const QString& label,
                   const char* name,
                   const QString& toolTip,
                   int defaultValue,
                   int minimum,
                   int dimension = 1)
{
    KnobIntPtr knob = AppManager::createKnob<KnobInt>(holder, label.toStdString(), dimension);

    knob->setName(name);
    knob->setHintToolTip(toolTip.toStdString());
    for (int d = 0; d < dimension; ++d) {
        knob->setDefaultValue(defaultValue, d);
        knob->setMinimum(minimum, d);
        knob->setDisplayMinimum(minimum, d);
        knob->setDisplayMaximum(std::max(minimum, 64), d);
    }
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

KnobBoolPtr
createCapKnob(KnobHolder* holder,
              const KnobPagePtr& page,
              const QString& label,
              const char* name,
              const QString& toolTip)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label.toStdString());

    knob->setName(name);
    knob->setHintToolTip(toolTip.toStdString());
    knob->setDefaultValue(true);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

template <typename KnobPtrType>
void
setShapeVisible(const std::weak_ptr<typename KnobPtrType::element_type>& knob,
                bool visible)
{
    if (const KnobPtrType locked = knob.lock()) {
        locked->setSecret(!visible);
    }
}

template <typename T>
T
valueAt(const std::weak_ptr<Knob<T>>& knob,
        T fallback,
        double time,
        ViewSpec view,
        int dimension = 0)
{
    if (const std::shared_ptr<Knob<T>> locked = knob.lock()) {
        return locked->getValueAtTime(time, dimension, view);
    }

    return fallback;
}
} // anonymous namespace

void
buildGeoShape(const GeoShapeParams& params,
              GeoDetailBuilder& detail)
{
    const PrimitiveMeshData mesh = buildMesh(params);

    const GeoOffset firstPoint = detail.appendPoints((GeoIndex)mesh.points.size());
    for (std::size_t i = 0; i < mesh.points.size(); ++i) {
        const std::array<float, 3>& p = mesh.points[i];
        detail.setPointPosition(firstPoint + (GeoOffset)i, p[0], p[1], p[2]);
    }

    std::vector<GeoOffset> corners(mesh.faceVertexIndices.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        corners[i] = firstPoint + (GeoOffset)mesh.faceVertexIndices[i];
    }
    // Vertex offsets follow the corner order, so corner i of the mesh is vertex firstVertex + i.
    const GeoOffset firstVertex = detail.detail().vertexMap().offsetSize();
    const std::vector<std::int32_t>& counts = mesh.faceVertexCounts;
    detail.appendPrimitives(eGeoPrimitivePolygon, std::span<const std::int32_t>(counts.data(), counts.size()), corners);

    GeoAttributeDesc uvDesc;
    uvDesc.name = "uv";
    uvDesc.storage = eGeoStorageFloat32;
    uvDesc.tupleSize = 3;
    GeoAttribute& uv = detail.addAttribute(eGeoAttribClassVertex, uvDesc);
    GeoAttributeDesc normalDesc;
    normalDesc.name = "N";
    normalDesc.storage = eGeoStorageFloat32;
    normalDesc.tupleSize = 3;
    normalDesc.typeInfo = eGeoTypeInfoNormal;
    GeoAttribute& normal = detail.addAttribute(eGeoAttribClassVertex, normalDesc);
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const std::array<float, 2>& st = mesh.st[(std::size_t)mesh.stIndices[i]];
        const float uvw[3] = { st[0], st[1], 0.f };
        uv.setTuple<float>((std::size_t)(firstVertex + (GeoOffset)i), std::span<const float>(uvw, 3));
        normal.setTuple<float>((std::size_t)(firstVertex + (GeoOffset)i), std::span<const float>(mesh.normals[i].data(), 3));
    }
}

GeoPrimitive::GeoPrimitive(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoPrimitive::~GeoPrimitive()
{
}

const NodeSpec*
GeoPrimitive::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoPrimitive::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobChoicePtr type = createKnob<KnobChoice>(tr("Type").toStdString());
    type->setName(kGeoPrimitiveParamType);
    type->setHintToolTip(tr("The shape to create.").toStdString());
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("card", tr("Card").toStdString(), tr("A flat quad standing in the XY plane, facing +Z.").toStdString()));
    options.push_back(ChoiceOption("grid", tr("Grid").toStdString(), tr("A flat grid lying in the XZ plane, facing +Y.").toStdString()));
    options.push_back(ChoiceOption("cube", tr("Cube").toStdString(), tr("A box.").toStdString()));
    options.push_back(ChoiceOption("sphere", tr("Sphere").toStdString(), tr("A sphere with its poles on the Y axis.").toStdString()));
    options.push_back(ChoiceOption("cylinder", tr("Cylinder").toStdString(), tr("A cylinder along the Y axis.").toStdString()));
    type->populateChoices(options);
    type->setDefaultValue((int)eGeoShapeCard);
    type->setAnimationEnabled(false);
    page->addKnob(type);
    _type = type;

    _cardSize = createSizeKnob(this, page, tr("Card Size"), kGeoPrimitiveParamCardSize, tr("The card's width and height."), { 1., 1. });
    _cardRows = createSegmentsKnob(this, page, tr("Card Rows"), kGeoPrimitiveParamCardRows,
                                   tr("The number of faces from the bottom of the card to its top."), 1, PrimitiveMeshes::kCardMinSegments);
    _cardColumns = createSegmentsKnob(this, page, tr("Card Columns"), kGeoPrimitiveParamCardColumns,
                                      tr("The number of faces from the left of the card to its right."), 1, PrimitiveMeshes::kCardMinSegments);

    _gridSize = createSizeKnob(this, page, tr("Grid Size"), kGeoPrimitiveParamGridSize, tr("The grid's size along the X and Z axes."), { 1., 1. });
    _gridRows = createSegmentsKnob(this, page, tr("Grid Rows"), kGeoPrimitiveParamGridRows,
                                   tr("The number of faces along the Z axis."), 10, PrimitiveMeshes::kCardMinSegments);
    _gridColumns = createSegmentsKnob(this, page, tr("Grid Columns"), kGeoPrimitiveParamGridColumns,
                                      tr("The number of faces along the X axis."), 10, PrimitiveMeshes::kCardMinSegments);

    _cubeSize = createSizeKnob(this, page, tr("Cube Size"), kGeoPrimitiveParamCubeSize,
                               tr("The cube's size along the X, Y and Z axes."), { 1., 1., 1. });
    _cubeSubdivisions = createSegmentsKnob(this, page, tr("Cube Subdivisions"), kGeoPrimitiveParamCubeSubdivisions,
                                           tr("The number of faces along the X, Y and Z axes, on every side the axis crosses."),
                                           2, PrimitiveMeshes::kCubeMinSegments, 3);

    _sphereRadius = createSizeKnob(this, page, tr("Sphere Radius"), kGeoPrimitiveParamSphereRadius, tr("The sphere's radius."), { 1. });
    _sphereRows = createSegmentsKnob(this, page, tr("Sphere Rows"), kGeoPrimitiveParamSphereRows,
                                     tr("The number of bands from pole to pole."), 30, PrimitiveMeshes::kSphereMinRows);
    _sphereColumns = createSegmentsKnob(this, page, tr("Sphere Columns"), kGeoPrimitiveParamSphereColumns,
                                        tr("The number of faces around each band."), 30, PrimitiveMeshes::kSphereMinColumns);

    _cylinderRadius = createSizeKnob(this, page, tr("Cylinder Radius"), kGeoPrimitiveParamCylinderRadius, tr("The cylinder's radius."), { 1. });
    _cylinderHeight = createSizeKnob(this, page, tr("Cylinder Height"), kGeoPrimitiveParamCylinderHeight,
                                     tr("The cylinder's length along the Y axis."), { 2. });
    _cylinderRows = createSegmentsKnob(this, page, tr("Cylinder Rows"), kGeoPrimitiveParamCylinderRows,
                                       tr("The number of bands from the bottom to the top."), 30, PrimitiveMeshes::kCylinderMinRows);
    _cylinderColumns = createSegmentsKnob(this, page, tr("Cylinder Columns"), kGeoPrimitiveParamCylinderColumns,
                                          tr("The number of faces around each band."), 30, PrimitiveMeshes::kCylinderMinColumns);
    _cylinderTopCap = createCapKnob(this, page, tr("Cylinder Top Cap"), kGeoPrimitiveParamCylinderTopCap, tr("Closes the top end with a disc."));
    _cylinderBottomCap = createCapKnob(this, page, tr("Cylinder Bottom Cap"), kGeoPrimitiveParamCylinderBottomCap, tr("Closes the bottom end with a disc."));

    updateVisibility();
}

void
GeoPrimitive::updateVisibility()
{
    const KnobChoicePtr type = _type.lock();
    if (!type) {
        return;
    }
    const int shape = type->getValue();
    const bool card = (shape == eGeoShapeCard);
    const bool grid = (shape == eGeoShapeGrid);
    const bool cube = (shape == eGeoShapeCube);
    const bool sphere = (shape == eGeoShapeSphere);
    const bool cylinder = (shape == eGeoShapeCylinder);

    setShapeVisible<KnobDoublePtr>(_cardSize, card);
    setShapeVisible<KnobIntPtr>(_cardRows, card);
    setShapeVisible<KnobIntPtr>(_cardColumns, card);
    setShapeVisible<KnobDoublePtr>(_gridSize, grid);
    setShapeVisible<KnobIntPtr>(_gridRows, grid);
    setShapeVisible<KnobIntPtr>(_gridColumns, grid);
    setShapeVisible<KnobDoublePtr>(_cubeSize, cube);
    setShapeVisible<KnobIntPtr>(_cubeSubdivisions, cube);
    setShapeVisible<KnobDoublePtr>(_sphereRadius, sphere);
    setShapeVisible<KnobIntPtr>(_sphereRows, sphere);
    setShapeVisible<KnobIntPtr>(_sphereColumns, sphere);
    setShapeVisible<KnobDoublePtr>(_cylinderRadius, cylinder);
    setShapeVisible<KnobDoublePtr>(_cylinderHeight, cylinder);
    setShapeVisible<KnobIntPtr>(_cylinderRows, cylinder);
    setShapeVisible<KnobIntPtr>(_cylinderColumns, cylinder);
    setShapeVisible<KnobBoolPtr>(_cylinderTopCap, cylinder);
    setShapeVisible<KnobBoolPtr>(_cylinderBottomCap, cylinder);
}

void
GeoPrimitive::onKnobsLoaded()
{
    updateVisibility();
}

bool
GeoPrimitive::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    const KnobChoicePtr type = _type.lock();
    if (type && (k == type.get())) {
        updateVisibility();

        return true;
    }

    return false;
}

GeoShapeParams
GeoPrimitive::readParams(double time,
                         ViewSpec view) const
{
    GeoShapeParams p;
    const int shape = valueAt<int>(_type, (int)p.shape, time, view);
    if ((shape >= eGeoShapeCard) && (shape <= eGeoShapeCylinder)) {
        p.shape = (GeoShapeEnum)shape;
    }
    for (int d = 0; d < 2; ++d) {
        p.cardSize[d] = valueAt<double>(_cardSize, p.cardSize[d], time, view, d);
        p.gridSize[d] = valueAt<double>(_gridSize, p.gridSize[d], time, view, d);
    }
    p.cardRows = valueAt<int>(_cardRows, p.cardRows, time, view);
    p.cardColumns = valueAt<int>(_cardColumns, p.cardColumns, time, view);
    p.gridRows = valueAt<int>(_gridRows, p.gridRows, time, view);
    p.gridColumns = valueAt<int>(_gridColumns, p.gridColumns, time, view);
    for (int d = 0; d < 3; ++d) {
        p.cubeSize[d] = valueAt<double>(_cubeSize, p.cubeSize[d], time, view, d);
        p.cubeSubdivisions[d] = valueAt<int>(_cubeSubdivisions, p.cubeSubdivisions[d], time, view, d);
    }
    p.sphereRadius = valueAt<double>(_sphereRadius, p.sphereRadius, time, view);
    p.sphereRows = valueAt<int>(_sphereRows, p.sphereRows, time, view);
    p.sphereColumns = valueAt<int>(_sphereColumns, p.sphereColumns, time, view);
    p.cylinderRadius = valueAt<double>(_cylinderRadius, p.cylinderRadius, time, view);
    p.cylinderHeight = valueAt<double>(_cylinderHeight, p.cylinderHeight, time, view);
    p.cylinderRows = valueAt<int>(_cylinderRows, p.cylinderRows, time, view);
    p.cylinderColumns = valueAt<int>(_cylinderColumns, p.cylinderColumns, time, view);
    p.cylinderTopCap = valueAt<bool>(_cylinderTopCap, p.cylinderTopCap, time, view);
    p.cylinderBottomCap = valueAt<bool>(_cylinderBottomCap, p.cylinderBottomCap, time, view);

    return p;
}

StatusEnum
GeoPrimitive::renderGeometry(const GeoRenderActionArgs& args,
                             GeoDetailConstPtr* outputDetail,
                             std::string* errorMessage)
{
    const GeoShapeParams params = readParams(args.time, args.view);

    return renderGeometryGenerator([&params](GeoDetailBuilder& detail, std::string* /*error*/) {
        buildGeoShape(params, detail);

        return eStatusOK;
    },
                                   outputDetail, errorMessage);
}

NATRON_NAMESPACE_EXIT
