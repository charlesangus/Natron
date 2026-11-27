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

#include "ConvertVDB.h"

#include <exception>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Engine/Geo/GeoOpenVDB.h"

#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Geo/GeoVDB.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoNodeKnobUtils.h"

NATRON_NAMESPACE_ENTER

namespace {

/** Whether one of the space-separated names of @p group is @p name itself, not a glob or an exclusion. */
bool
namedExactly(const std::string& group,
             const std::string& name)
{
    std::istringstream tokens(group);
    std::string token;
    while (tokens >> token) {
        if (token == name) {
            return true;
        }
    }

    return false;
}

/** The VDB of @p mask matching @p pattern, or null with a note naming @p what. */
GeoVDBGridBaseConstPtr
maskGrid(const GeoDetail* mask,
         const std::string& pattern,
         const char* what,
         std::vector<std::string>* notes)
{
    GeoVDBGridBaseConstPtr grid = mask ? geoFindVDB(*mask, pattern) : GeoVDBGridBaseConstPtr();
    if (!grid && notes) {
        notes->push_back(std::string("No VDB on the Mask input matches ") + what + ", so it was not used.");
    }

    return grid;
}

} // anonymous namespace

bool
ConvertVDB::apply(GeoDetailBuilder& builder,
                  const ConvertVDBParams& params,
                  const GeoDetail* reference,
                  const GeoDetail* mask,
                  std::vector<std::string>* notes,
                  std::string* error)
{
    struct Selected {
        GeoOffset primitive;
        std::string name;
        GeoVDBGridBaseConstPtr grid;
    };
    std::vector<Selected> selected;
    {
        const GeoDetail& d = builder.detail();
        d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
            if (d.primitiveType(prim) != eGeoPrimitiveVDB) {
                return;
            }
            const std::string name(geoVDBName(d, prim));
            if (geoNameMatchesPattern(params.group, name)) {
                selected.push_back(Selected { prim, name, geoVDBGrid(d, prim) });
            }
        });
    }
    const auto note = [notes](const std::string& text) {
        if (notes) {
            notes->push_back(text);
        }
    };

    GeoSdfToPolygonsParams mesh = params.mesh;
    if (params.target == eConvertVDBTargetPolygons) {
        if (params.useSurfaceMask) {
            mesh.surfaceMask = maskGrid(mask, params.surfaceMask, "Surface Mask", notes);
        }
        if (params.useAdaptivityField) {
            mesh.adaptivityField = maskGrid(mask, params.adaptivityField, "Adaptivity Field", notes);
        }
    }

    std::vector<GeoOffset> meshed;
    for (const Selected& s : selected) {
        const bool scalar = s.grid->isType<openvdb::FloatGrid>() || s.grid->isType<openvdb::DoubleGrid>();
        const openvdb::GridClass gridClass = s.grid->getGridClass();
        const bool fog = gridClass == openvdb::GRID_FOG_VOLUME;
        const bool levelSet = gridClass == openvdb::GRID_LEVEL_SET;
        if (params.target == eConvertVDBTargetPolygons) {
            if (!scalar) {
                note(s.name + " holds " + s.grid->valueType() + " values, not float or double ones, so it was left as it is.");
                continue;
            }
            // Attribute and unsigned distance grids sit beside SDFs under the same group, and
            // their isosurface is rarely wanted.
            if (!fog && !levelSet && !namedExactly(params.group, s.name)) {
                note(s.name + " is neither an SDF nor a fog volume, so it was left as it is; name it in Group to "
                              "make polygons of it.");
                continue;
            }
            mesh.isovalue = fog ? params.fogIsovalue : params.isovalue;
            if (!geoSdfToPolygons(*s.grid, mesh, reference, builder, error)) {
                return false;
            }
            meshed.push_back(s.primitive);
            continue;
        }

        GeoVDBGridBaseConstPtr grid = s.grid;
        switch (params.vdbClass) {
        case eConvertVDBClassNoChange:
            grid = geoConvertVDBValues(grid, params.vdbType, params.vdbPrecision, error);
            if (!grid) {
                return false;
            }
            break;
        case eConvertVDBClassFogToSDF:
            if (fog && scalar) {
                grid = geoFogToSdf(*grid, params.fogIsovalue, error);
                if (!grid) {
                    return false;
                }
            } else if (!levelSet) {
                note(s.name + " is not a float or double fog volume, so it was not converted to an SDF.");
            }
            break;
        case eConvertVDBClassSDFToFog:
            if (levelSet && scalar) {
                grid = geoSdfToFog(*grid, error);
                if (!grid) {
                    return false;
                }
            } else if (!fog) {
                note(s.name + " is not a float or double SDF, so it was not converted to fog.");
            }
            break;
        }
        grid = geoCleanupVDB(*grid, params.cleanup);
        geoReplaceVDBGrid(builder, s.primitive, grid);
    }
    if (!meshed.empty()) {
        builder.deletePrimitives(meshed, true);
    }

    return true;
} // ConvertVDB::apply

ConvertVDB::ConvertVDB(NodePtr node)
    : NativeEffectBase(node)
{
}

ConvertVDB::~ConvertVDB()
{
}

const NodeSpec*
ConvertVDB::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
ConvertVDB::initializeKnobs()
{
    const ConvertVDBParams defaults;
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    _group = geoAddStringKnob(this, page, tr("Group").toStdString(), kConvertVDBParamGroup,
                              tr("The VDB primitives to convert, by name: space-separated globs, ^ in front of one to "
                                 "leave out what it matches. * converts every one. Polygons are made from grids "
                                 "that are neither SDFs nor fog volumes only when their exact name is listed.")
                                  .toStdString(),
                              defaults.group);
    {
        std::vector<ChoiceOption> targets;
        targets.push_back(ChoiceOption("polygons", tr("Polygons").toStdString(),
                                       tr("Quads and triangles along the isosurface, facing out of the inside: "
                                          "below the isovalue for an SDF, above it for any other grid.")
                                           .toStdString()));
        targets.push_back(ChoiceOption("vdb", tr("VDB").toStdString(),
                                       tr("VDBs of the class, value type and precision asked for, pruned and flood "
                                          "filled.")
                                           .toStdString()));
        _convertTo = geoAddChoiceKnob(this, page, tr("Convert To").toStdString(), kConvertVDBParamConvertTo,
                                      tr("What the selected grids become.").toStdString(), targets, int(defaults.target));
    }
    {
        std::vector<ChoiceOption> classes;
        classes.push_back(ChoiceOption("noChange", tr("No Change").toStdString(),
                                       tr("Keep each grid's class; VDB Type and VDB Precision apply.").toStdString()));
        classes.push_back(ChoiceOption("fogToSdf", tr("Convert Fog To SDF").toStdString(),
                                       tr("Fog volumes become narrow-band SDFs, 3 voxels each side, whose surface is "
                                          "where the density crosses Fog Isovalue.")
                                           .toStdString()));
        classes.push_back(ChoiceOption("sdfToFog", tr("Convert SDF To Fog").toStdString(),
                                       tr("SDFs become fog volumes: 1 inside, 0 outside, ramping across the "
                                          "interior band.")
                                           .toStdString()));
        _vdbClass = geoAddChoiceKnob(this, page, tr("VDB Class").toStdString(), kConvertVDBParamVDBClass,
                                     tr("The class the VDBs are converted to. Grids already of it, and grids of "
                                        "neither class, are left as they are.")
                                         .toStdString(),
                                     classes, int(defaults.vdbClass));
    }
    {
        std::vector<ChoiceOption> types;
        types.push_back(ChoiceOption("same", tr("Same").toStdString(), tr("Keep the value type.").toStdString()));
        types.push_back(ChoiceOption("float", tr("Float").toStdString(),
                                     tr("Scalars; a vector grid keeps only its topology, with zero values.").toStdString()));
        types.push_back(ChoiceOption("integer", tr("Integer").toStdString(),
                                     tr("Whole numbers, floats rounded to the nearest.").toStdString()));
        types.push_back(ChoiceOption("vector", tr("Vector").toStdString(),
                                     tr("Three components; a scalar is copied into each.").toStdString()));
        types.push_back(ChoiceOption("bool", tr("Bool").toStdString(),
                                     tr("On where the value is not zero.").toStdString()));
        _vdbType = geoAddChoiceKnob(this, page, tr("VDB Type").toStdString(), kConvertVDBParamVDBType,
                                    tr("The type of value stored in each voxel. Only with VDB Class at No Change: "
                                       "SDFs and fog volumes are always floats.")
                                        .toStdString(),
                                    types, int(defaults.vdbType));
    }
    {
        std::vector<ChoiceOption> precisions;
        precisions.push_back(ChoiceOption("same", tr("Same").toStdString(), tr("Keep the precision.").toStdString()));
        precisions.push_back(ChoiceOption("32", tr("32-bit").toStdString(), ""));
        precisions.push_back(ChoiceOption("64", tr("64-bit").toStdString(), ""));
        _vdbPrecision = geoAddChoiceKnob(this, page, tr("VDB Precision").toStdString(), kConvertVDBParamVDBPrecision,
                                         tr("The precision of each value. Only with VDB Class at No Change; bool "
                                            "grids have none.")
                                             .toStdString(),
                                         precisions, int(defaults.vdbPrecision));
    }

    KnobDoublePtr isovalue = geoAddDoubleKnob(this, page, tr("Isovalue").toStdString(), kConvertVDBParamIsovalue,
                                              tr("The level of the surface in grids that are not fog volumes, in world "
                                                 "units for an SDF: above 0 the surface grows outwards.")
                                                  .toStdString(),
                                              defaults.isovalue, -1., 1.);
    isovalue->setMinimum(-std::numeric_limits<double>::max());
    _isovalue = isovalue;
    _fogIsovalue = geoAddDoubleKnob(this, page, tr("Fog Isovalue").toStdString(), kConvertVDBParamFogIsovalue,
                                    tr("The density at the surface of fog volumes, for polygons and for an SDF.")
                                        .toStdString(),
                                    defaults.fogIsovalue, 0., 1.);
    _fogIsovalue.lock()->setMinimum(-std::numeric_limits<double>::max());
    _pruneTolerance = geoAddDoubleKnob(this, page, tr("Prune Tolerance").toStdString(), kConvertVDBParamPruneTolerance,
                                       tr("Voxels within this of the background are made inactive, then regions of "
                                          "values within it of each other collapse into tiles.")
                                           .toStdString(),
                                       defaults.cleanup.pruneTolerance, 0., 1.);
    _signedFloodFill = geoAddBoolKnob(this, page, tr("Signed Flood Fill").toStdString(), kConvertVDBParamSignedFloodFill,
                                      tr("Give the inactive values of an SDF the sign of the side they are on, "
                                         "negative inside. Other grids are left as they are.")
                                          .toStdString(),
                                      defaults.cleanup.signedFloodFill);
    _activateInsideVoxels = geoAddBoolKnob(this, page, tr("Activate Inside Voxels").toStdString(),
                                           kConvertVDBParamActivateInsideVoxels,
                                           tr("Make every voxel inside an SDF active, not only the interior band, "
                                              "for operations that visit only active voxels.")
                                               .toStdString(),
                                           defaults.cleanup.activateInsideVoxels);

    KnobDoublePtr adaptivity = geoAddDoubleKnob(this, page, tr("Adaptivity").toStdString(), kConvertVDBParamAdaptivity,
                                                tr("0 makes every polygon about a voxel in size; towards 1, flatter "
                                                   "regions are merged into fewer, larger polygons.")
                                                    .toStdString(),
                                                defaults.mesh.adaptivity, 0., 1.);
    adaptivity->setMaximum(1.);
    _adaptivity = adaptivity;
    _computeVertexNormals = geoAddBoolKnob(this, page, tr("Compute Vertex Normals").toStdString(),
                                           kConvertVDBParamComputeVertexNormals,
                                           tr("Give the polygons vertex normals N, smoothed between polygons that "
                                              "meet within 60 degrees. With a reference, the polygons on its surface "
                                              "and those inside it do not share normals, so their seams stay sharp.")
                                               .toStdString(),
                                           defaults.mesh.computeVertexNormals);
    KnobDoublePtr internalAdaptivity = geoAddDoubleKnob(this, page, tr("Internal Adaptivity").toStdString(),
                                                        kConvertVDBParamInternalAdaptivity,
                                                        tr("With a reference, the adaptivity of the regions inside its "
                                                           "surface, such as the faces of a fracture.")
                                                            .toStdString(),
                                                        defaults.mesh.internalAdaptivity, 0., 1.);
    internalAdaptivity->setMaximum(1.);
    _internalAdaptivity = internalAdaptivity;
    _transferSurfaceAttributes = geoAddBoolKnob(this, page, tr("Transfer Surface Attributes").toStdString(),
                                                kConvertVDBParamTransferSurfaceAttributes,
                                                tr("With a reference, every point takes the point attributes of the "
                                                   "closest reference point, and the polygons on the reference "
                                                   "surface its primitive and vertex attributes, overriding computed "
                                                   "normals there.")
                                                    .toStdString(),
                                                defaults.mesh.transferSurfaceAttributes);
    _referenceAttributes = geoAddStringKnob(this, page, tr("Reference Attributes").toStdString(),
                                            kConvertVDBParamReferenceAttributes,
                                            tr("The reference attributes transferred: space-separated globs, ^ in "
                                               "front of one to leave out what it matches.")
                                                .toStdString(),
                                            defaults.mesh.referenceAttributes);
    _sharpenFeatures = geoAddBoolKnob(this, page, tr("Sharpen Features").toStdString(), kConvertVDBParamSharpenFeatures,
                                      tr("With a reference, move the points along the seams onto its edges and "
                                         "corners.")
                                          .toStdString(),
                                      defaults.mesh.sharpenFeatures);
    KnobDoublePtr edgeTolerance = geoAddDoubleKnob(this, page, tr("Edge Tolerance").toStdString(), kConvertVDBParamEdgeTolerance,
                                                   tr("With a reference, voxels where its surface turns sharper than "
                                                      "this, as a dot product of normals, keep full resolution "
                                                      "whatever the adaptivity.")
                                                       .toStdString(),
                                                   defaults.mesh.edgeTolerance, 0., 1.);
    edgeTolerance->setMaximum(1.);
    _edgeTolerance = edgeTolerance;
    _surfaceGroup = geoAddStringKnob(this, page, tr("Surface Group").toStdString(), kConvertVDBParamSurfaceGroup,
                                     tr("With a reference, the primitive group of the polygons on its surface. Empty "
                                        "for none.")
                                         .toStdString(),
                                     defaults.mesh.surfaceGroup);
    _interiorGroup = geoAddStringKnob(this, page, tr("Interior Group").toStdString(), kConvertVDBParamInteriorGroup,
                                      tr("With a reference, the primitive group of the polygons inside its surface. "
                                         "Empty for none.")
                                          .toStdString(),
                                      defaults.mesh.interiorGroup);
    _seamLineGroup = geoAddStringKnob(this, page, tr("Seam Line Group").toStdString(), kConvertVDBParamSeamLineGroup,
                                      tr("With a reference, the primitive group of the polygons along the seams "
                                         "between its surface and the inside. Empty for none.")
                                          .toStdString(),
                                      defaults.mesh.seamLineGroup);
    _seamPoints = geoAddStringKnob(this, page, tr("Seam Points").toStdString(), kConvertVDBParamSeamPoints,
                                   tr("With a reference, the point group of the seams' points. Empty for none.")
                                       .toStdString(),
                                   defaults.mesh.seamPoints);

    _useSurfaceMask = geoAddBoolKnob(this, page, tr("Use Surface Mask").toStdString(), kConvertVDBParamUseSurfaceMask,
                                     tr("Make polygons only inside a VDB of the Mask input.").toStdString(),
                                     defaults.useSurfaceMask);
    _surfaceMask = geoAddStringKnob(this, page, tr("Surface Mask").toStdString(), kConvertVDBParamSurfaceMask,
                                    tr("The mask VDB, by name pattern; the first that matches is used. Its inside is "
                                       "below Mask Offset for an SDF, above it for any other float grid, and its "
                                       "active voxels for other types.")
                                        .toStdString(),
                                    defaults.surfaceMask);
    _maskOffset = geoAddDoubleKnob(this, page, tr("Mask Offset").toStdString(), kConvertVDBParamMaskOffset,
                                   tr("The level that bounds the mask's inside.").toStdString(), defaults.mesh.maskOffset,
                                   -1., 1.);
    _maskOffset.lock()->setMinimum(-std::numeric_limits<double>::max());
    _invertSurfaceMask = geoAddBoolKnob(this, page, tr("Invert Surface Mask").toStdString(), kConvertVDBParamInvertSurfaceMask,
                                        tr("Make polygons only outside the mask instead.").toStdString(),
                                        defaults.mesh.invertSurfaceMask);
    _useAdaptivityField = geoAddBoolKnob(this, page, tr("Use Adaptivity Field").toStdString(),
                                         kConvertVDBParamUseAdaptivityField,
                                         tr("Scale the adaptivity by a VDB of the Mask input.").toStdString(),
                                         defaults.useAdaptivityField);
    _adaptivityField = geoAddStringKnob(this, page, tr("Adaptivity Field").toStdString(), kConvertVDBParamAdaptivityField,
                                        tr("The float or double VDB, by name pattern, whose values multiply Adaptivity "
                                           "where they are sampled; the first that matches is used.")
                                            .toStdString(),
                                        defaults.adaptivityField);

    updateVisibility();
} // ConvertVDB::initializeKnobs

void
ConvertVDB::updateVisibility()
{
    const KnobChoicePtr convertTo = _convertTo.lock();
    const bool polygons = !convertTo || (convertTo->getValue() == int(eConvertVDBTargetPolygons));
    const KnobChoicePtr vdbClass = _vdbClass.lock();
    const int cls = vdbClass ? vdbClass->getValue() : int(eConvertVDBClassNoChange);
    geoSetKnobVisible(_vdbClass, !polygons);
    geoSetKnobVisible(_vdbType, !polygons);
    geoSetKnobVisible(_vdbPrecision, !polygons);
    if (const KnobChoicePtr type = _vdbType.lock()) {
        type->setAllDimensionsEnabled(cls == int(eConvertVDBClassNoChange));
    }
    if (const KnobChoicePtr precision = _vdbPrecision.lock()) {
        precision->setAllDimensionsEnabled(cls == int(eConvertVDBClassNoChange));
    }
    geoSetKnobVisible(_isovalue, polygons);
    geoSetKnobVisible(_fogIsovalue, polygons || (cls == int(eConvertVDBClassFogToSDF)));
    geoSetKnobVisible(_pruneTolerance, !polygons);
    geoSetKnobVisible(_signedFloodFill, !polygons);
    geoSetKnobVisible(_activateInsideVoxels, !polygons);
    geoSetKnobVisible(_adaptivity, polygons);
    geoSetKnobVisible(_computeVertexNormals, polygons);
    geoSetKnobVisible(_internalAdaptivity, polygons);
    geoSetKnobVisible(_transferSurfaceAttributes, polygons);
    geoSetKnobVisible(_referenceAttributes, polygons);
    geoSetKnobVisible(_sharpenFeatures, polygons);
    geoSetKnobVisible(_edgeTolerance, polygons);
    geoSetKnobVisible(_surfaceGroup, polygons);
    geoSetKnobVisible(_interiorGroup, polygons);
    geoSetKnobVisible(_seamLineGroup, polygons);
    geoSetKnobVisible(_seamPoints, polygons);
    geoSetKnobVisible(_useSurfaceMask, polygons);
    const KnobBoolPtr useSurfaceMask = _useSurfaceMask.lock();
    const bool masked = polygons && useSurfaceMask && useSurfaceMask->getValue();
    geoSetKnobVisible(_surfaceMask, masked);
    geoSetKnobVisible(_maskOffset, masked);
    geoSetKnobVisible(_invertSurfaceMask, masked);
    geoSetKnobVisible(_useAdaptivityField, polygons);
    const KnobBoolPtr useAdaptivityField = _useAdaptivityField.lock();
    geoSetKnobVisible(_adaptivityField, polygons && useAdaptivityField && useAdaptivityField->getValue());
} // ConvertVDB::updateVisibility

void
ConvertVDB::onKnobsLoaded()
{
    updateVisibility();
}

bool
ConvertVDB::knobChanged(KnobI* k,
                        ValueChangedReasonEnum /*reason*/,
                        ViewSpec /*view*/,
                        double /*time*/,
                        bool /*originatedFromMainThread*/)
{
    const KnobI* toggles[] = {
        _convertTo.lock().get(),
        _vdbClass.lock().get(),
        _useSurfaceMask.lock().get(),
        _useAdaptivityField.lock().get(),
    };
    for (const KnobI* toggle : toggles) {
        if (toggle && (k == toggle)) {
            updateVisibility();

            return true;
        }
    }

    return false;
}

ConvertVDBParams
ConvertVDB::readParams(double time,
                       ViewSpec view) const
{
    ConvertVDBParams p;
    p.group = geoKnobValueAt<std::string>(_group, p.group, time, view);
    const int target = geoKnobValueAt<int>(_convertTo, int(p.target), time, view);
    if ((target >= eConvertVDBTargetPolygons) && (target <= eConvertVDBTargetVDB)) {
        p.target = ConvertVDBTargetEnum(target);
    }
    const int cls = geoKnobValueAt<int>(_vdbClass, int(p.vdbClass), time, view);
    if ((cls >= eConvertVDBClassNoChange) && (cls <= eConvertVDBClassSDFToFog)) {
        p.vdbClass = ConvertVDBClassEnum(cls);
    }
    const int type = geoKnobValueAt<int>(_vdbType, int(p.vdbType), time, view);
    if ((type >= eGeoVDBValueTypeSame) && (type <= eGeoVDBValueTypeBool)) {
        p.vdbType = GeoVDBValueTypeEnum(type);
    }
    const int precision = geoKnobValueAt<int>(_vdbPrecision, int(p.vdbPrecision), time, view);
    if ((precision >= eGeoVDBPrecisionSame) && (precision <= eGeoVDBPrecision64)) {
        p.vdbPrecision = GeoVDBPrecisionEnum(precision);
    }
    p.isovalue = geoKnobValueAt<double>(_isovalue, p.isovalue, time, view);
    p.fogIsovalue = geoKnobValueAt<double>(_fogIsovalue, p.fogIsovalue, time, view);
    p.cleanup.pruneTolerance = geoKnobValueAt<double>(_pruneTolerance, p.cleanup.pruneTolerance, time, view);
    p.cleanup.signedFloodFill = geoKnobValueAt<bool>(_signedFloodFill, p.cleanup.signedFloodFill, time, view);
    p.cleanup.activateInsideVoxels = geoKnobValueAt<bool>(_activateInsideVoxels, p.cleanup.activateInsideVoxels, time, view);
    GeoSdfToPolygonsParams& m = p.mesh;
    m.adaptivity = geoKnobValueAt<double>(_adaptivity, m.adaptivity, time, view);
    m.computeVertexNormals = geoKnobValueAt<bool>(_computeVertexNormals, m.computeVertexNormals, time, view);
    m.internalAdaptivity = geoKnobValueAt<double>(_internalAdaptivity, m.internalAdaptivity, time, view);
    m.transferSurfaceAttributes = geoKnobValueAt<bool>(_transferSurfaceAttributes, m.transferSurfaceAttributes, time, view);
    m.referenceAttributes = geoKnobValueAt<std::string>(_referenceAttributes, m.referenceAttributes, time, view);
    m.sharpenFeatures = geoKnobValueAt<bool>(_sharpenFeatures, m.sharpenFeatures, time, view);
    m.edgeTolerance = geoKnobValueAt<double>(_edgeTolerance, m.edgeTolerance, time, view);
    m.surfaceGroup = geoKnobValueAt<std::string>(_surfaceGroup, m.surfaceGroup, time, view);
    m.interiorGroup = geoKnobValueAt<std::string>(_interiorGroup, m.interiorGroup, time, view);
    m.seamLineGroup = geoKnobValueAt<std::string>(_seamLineGroup, m.seamLineGroup, time, view);
    m.seamPoints = geoKnobValueAt<std::string>(_seamPoints, m.seamPoints, time, view);
    m.maskOffset = geoKnobValueAt<double>(_maskOffset, m.maskOffset, time, view);
    m.invertSurfaceMask = geoKnobValueAt<bool>(_invertSurfaceMask, m.invertSurfaceMask, time, view);
    p.useSurfaceMask = geoKnobValueAt<bool>(_useSurfaceMask, p.useSurfaceMask, time, view);
    p.surfaceMask = geoKnobValueAt<std::string>(_surfaceMask, p.surfaceMask, time, view);
    p.useAdaptivityField = geoKnobValueAt<bool>(_useAdaptivityField, p.useAdaptivityField, time, view);
    p.adaptivityField = geoKnobValueAt<std::string>(_adaptivityField, p.adaptivityField, time, view);

    return p;
} // ConvertVDB::readParams

StatusEnum
ConvertVDB::renderGeometry(const GeoRenderActionArgs& args,
                           GeoDetailConstPtr* outputDetail,
                           std::string* errorMessage)
{
    const ConvertVDBParams params = readParams(args.time, args.view);
    const GeoDetailConstPtr reference = args.getInputDetail(1);
    const GeoDetailConstPtr mask = args.getInputDetail(2);
    std::vector<std::string> notes;
    const StatusEnum status = renderGeometryFromInput(args, 0, [&](GeoDetailBuilder& detail, std::string* error) {
        try {
            return apply(detail, params, reference.get(), mask.get(), &notes, error) ? eStatusOK : eStatusFailed;
        } catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }

            return eStatusFailed;
        } }, outputDetail, errorMessage);
    if (status == eStatusOK) {
        if (notes.empty()) {
            clearPersistentMessage(false);
        } else {
            setPersistentMessage(eMessageTypeWarning, geoJoinNotes(notes));
        }
    }

    return status;
}

NATRON_NAMESPACE_EXIT
