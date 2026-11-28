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

#include "VDBFromPolygons.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Geo/GeoOpenVDB.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoNodeKnobUtils.h"

NATRON_NAMESPACE_ENTER

namespace {

/** @p params' conversion with the voxel size from the voxel count or the reference; false when
 * no size can be found, with the reason in @p notes. */
bool
resolveConvert(const GeoDetail& source,
               const GeoDetail* reference,
               const VDBFromPolygonsParams& params,
               GeoPolygonsToSdfParams* convert,
               std::vector<std::string>* notes)
{
    *convert = params.convert;
    if (reference) {
        if (const GeoVDBGridBaseConstPtr grid = geoFindVDB(*reference, params.referenceVDB)) {
            convert->transform = grid->transformPtr();

            return true;
        }
        notes->push_back(std::string("No VDB on the Reference input matches Reference VDB, so ")
                         + (params.useVoxelCount ? "Voxel Count" : "Voxel Size") + " is used.");
    }
    if (params.useVoxelCount) {
        const double counted = geoVoxelSizeForCount(source, params.voxelCount, params.voxelCountAxis);
        if (counted > 0.) {
            convert->voxelSize = counted;
        } else if (source.pointCount() > 0) {
            notes->push_back("The source has no extent along the voxel count's axis to divide, so no volume was made.");

            return false;
        }
    }

    return true;
}

} // anonymous namespace

bool
VDBFromPolygons::build(const GeoDetail& source,
                       const GeoDetail* reference,
                       const VDBFromPolygonsParams& params,
                       GeoDetailBuilder& out,
                       std::vector<std::string>* notes,
                       std::string* error,
                       const GeoVDBEstimateCallback& estimated)
{
    std::vector<std::string> resolveNotes;
    GeoPolygonsToSdfParams convert;
    if (!resolveConvert(source, reference, params, &convert, &resolveNotes)) {
        if (notes) {
            notes->insert(notes->end(), resolveNotes.begin(), resolveNotes.end());
        }

        return true;
    }
    GeoVDBConvertResult result;
    if (!geoPolygonsToSdf(source, convert, &result, error, estimated)) {
        return false;
    }
    geoAppendVDBGrids(out, result);
    if (notes) {
        // The estimate stays first, where the info showed it during the build.
        const auto rest = result.notes.begin() + std::min<std::ptrdiff_t>(1, std::ptrdiff_t(result.notes.size()));
        notes->insert(notes->end(), result.notes.begin(), rest);
        notes->insert(notes->end(), resolveNotes.begin(), resolveNotes.end());
        notes->insert(notes->end(), rest, result.notes.end());
    }

    return true;
}

VDBFromPolygons::VDBFromPolygons(NodePtr node)
    : NativeEffectBase(node)
{
}

VDBFromPolygons::~VDBFromPolygons()
{
}

const NodeSpec*
VDBFromPolygons::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
VDBFromPolygons::initializeKnobs()
{
    const VDBFromPolygonsParams defaults;
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    _referenceVDB = geoAddStringKnob(this, page, tr("Reference VDB").toStdString(), kVDBFromPolygonsParamReferenceVDB,
                                     tr("The VDB of the Reference input, by name pattern, whose voxel size and "
                                        "orientation the grids take; the first that matches is used. The voxel "
                                        "size or count applies when the input is not connected or nothing "
                                        "matches.")
                                         .toStdString(),
                                     defaults.referenceVDB);

    _distanceVDB = geoAddBoolKnob(this, page, tr("Distance VDB").toStdString(), kVDBFromPolygonsParamDistanceVDB,
                                  tr("Output a narrow-band distance field: the distance to the surface in world "
                                     "units, negative inside unless Unsigned Distance Field is on.")
                                      .toStdString(),
                                  defaults.convert.buildDistance);
    _distanceName = geoAddStringKnob(this, page, tr("Distance Name").toStdString(), kVDBFromPolygonsParamDistanceName,
                                     tr("The name of the distance grid.").toStdString(), defaults.convert.distanceName);
    _fogVDB = geoAddBoolKnob(this, page, tr("Fog VDB").toStdString(), kVDBFromPolygonsParamFogVDB,
                             tr("Output a density of 0 outside rising to 1 inside: across the interior band, "
                                "or with Fill Interior to the deepest point inside.")
                                 .toStdString(),
                             defaults.convert.buildFog);
    _fogName = geoAddStringKnob(this, page, tr("Fog Name").toStdString(), kVDBFromPolygonsParamFogName,
                                tr("The name of the fog grid.").toStdString(), defaults.convert.fogName);

    _useVoxelCount = geoAddBoolKnob(this, page, tr("Use Voxel Count").toStdString(), kVDBFromPolygonsParamUseVoxelCount,
                                    tr("Size the voxels so that Voxel Count of them span the source along Voxel "
                                       "Count Axis, instead of giving the voxel size.")
                                        .toStdString(),
                                    defaults.useVoxelCount);
    KnobDoublePtr voxelSize = geoAddDoubleKnob(this, page, tr("Voxel Size").toStdString(), kVDBFromPolygonsParamVoxelSize,
                                               tr("The edge length of a voxel in world units. Memory grows with the "
                                                  "cube of its inverse; the node's info shows an estimate before "
                                                  "building and what the grids use after.")
                                                   .toStdString(),
                                               defaults.convert.voxelSize, 0., 1.);
    voxelSize->setDisplayMinimum(0.001);
    _voxelSize = voxelSize;

    KnobIntPtr voxelCount = geoAddKnob<KnobInt>(this, page, tr("Voxel Count").toStdString(), kVDBFromPolygonsParamVoxelCount,
                                                tr("How many voxels span the source's bounds along Voxel Count Axis.")
                                                    .toStdString());
    voxelCount->setAnimationEnabled(true);
    voxelCount->setDefaultValue(defaults.voxelCount);
    voxelCount->setMinimum(1);
    voxelCount->setDisplayMinimum(1);
    voxelCount->setDisplayMaximum(500);
    _voxelCount = voxelCount;
    {
        std::vector<ChoiceOption> axes;
        axes.push_back(ChoiceOption("x", tr("X").toStdString(), ""));
        axes.push_back(ChoiceOption("y", tr("Y").toStdString(), ""));
        axes.push_back(ChoiceOption("z", tr("Z").toStdString(), ""));
        axes.push_back(ChoiceOption("longest", tr("Longest").toStdString(),
                                    tr("Whichever of the three is longest.").toStdString()));
        _voxelCountAxis = geoAddChoiceKnob(this, page, tr("Voxel Count Axis").toStdString(), kVDBFromPolygonsParamVoxelCountAxis,
                                           tr("The axis of the source's bounds that Voxel Count divides.").toStdString(),
                                           axes, int(defaults.voxelCountAxis));
    }

    _useWorldSpaceForBand = geoAddBoolKnob(this, page, tr("Use World Space For Band").toStdString(),
                                           kVDBFromPolygonsParamUseWorldSpaceForBand,
                                           tr("Give the band widths in world units instead of voxels.").toStdString(),
                                           defaults.convert.useWorldSpaceForBand);
    _exteriorBand = geoAddDoubleKnob(this, page, tr("Exterior Band").toStdString(), kVDBFromPolygonsParamExteriorBand,
                                     tr("How far outside the surface distances are stored, in voxels. At least 1.")
                                         .toStdString(),
                                     defaults.convert.exteriorBand, 1., 10.);
    _interiorBand = geoAddDoubleKnob(this, page, tr("Interior Band").toStdString(), kVDBFromPolygonsParamInteriorBand,
                                     tr("How far inside the surface distances are stored, in voxels. At least 1. "
                                        "Without Fill Interior, a fog volume's density ramps across it.")
                                         .toStdString(),
                                     defaults.convert.interiorBand, 1., 10.);
    _exteriorBandWorld = geoAddDoubleKnob(this, page, tr("Exterior Band World").toStdString(),
                                          kVDBFromPolygonsParamExteriorBandWorld,
                                          tr("How far outside the surface distances are stored, in world units. "
                                             "At least one voxel is kept.")
                                              .toStdString(),
                                          defaults.convert.exteriorBandWorld, 0., 1.);
    _interiorBandWorld = geoAddDoubleKnob(this, page, tr("Interior Band World").toStdString(),
                                          kVDBFromPolygonsParamInteriorBandWorld,
                                          tr("How far inside the surface distances are stored, in world units. At "
                                             "least one voxel is kept. Without Fill Interior, a fog volume's "
                                             "density ramps across it.")
                                              .toStdString(),
                                          defaults.convert.interiorBandWorld, 0., 1.);
    _fillInterior = geoAddBoolKnob(this, page, tr("Fill Interior").toStdString(), kVDBFromPolygonsParamFillInterior,
                                   tr("Store distances through the whole inside instead of stopping at the interior "
                                      "band, and ramp the fog volume's density to 1 at the deepest point inside. "
                                      "Needs a closed mesh.")
                                       .toStdString(),
                                   defaults.convert.fillInterior);
    _unsignedDistanceField = geoAddBoolKnob(this, page, tr("Unsigned Distance Field").toStdString(),
                                            kVDBFromPolygonsParamUnsignedDistanceField,
                                            tr("Store positive distances on both sides of the surface, within the "
                                               "exterior band. It needs no inside, so it suits open meshes, but it "
                                               "is not an SDF. The fog still guesses an inside.")
                                                .toStdString(),
                                            defaults.convert.unsignedDistanceField);

    _attributes = geoAddStringKnob(this, page, tr("Attributes").toStdString(), kVDBFromPolygonsParamAttributes,
                                   tr("Point, vertex and primitive attributes to carry into grids of their own, "
                                      "each named after its attribute: space-separated globs, ^ in front of one to "
                                      "leave out what it matches. Each voxel of the band holds the value at the "
                                      "closest point of the surface. Float and integer attributes of one or three "
                                      "components convert.")
                                       .toStdString(),
                                   defaults.convert.attributes);

    updateVisibility();
}

void
VDBFromPolygons::updateVisibility()
{
    const KnobBoolPtr useVoxelCount = _useVoxelCount.lock();
    const bool count = useVoxelCount && useVoxelCount->getValue();
    geoSetKnobVisible(_voxelSize, !count);
    geoSetKnobVisible(_voxelCount, count);
    geoSetKnobVisible(_voxelCountAxis, count);
    const KnobBoolPtr world = _useWorldSpaceForBand.lock();
    const bool inWorld = world && world->getValue();
    geoSetKnobVisible(_exteriorBand, !inWorld);
    geoSetKnobVisible(_interiorBand, !inWorld);
    geoSetKnobVisible(_exteriorBandWorld, inWorld);
    geoSetKnobVisible(_interiorBandWorld, inWorld);
    const KnobBoolPtr distance = _distanceVDB.lock();
    const bool distanceOn = distance && distance->getValue();
    geoSetKnobVisible(_distanceName, distanceOn);
    geoSetKnobVisible(_unsignedDistanceField, distanceOn);
    const KnobBoolPtr fog = _fogVDB.lock();
    geoSetKnobVisible(_fogName, fog && fog->getValue());
}

void
VDBFromPolygons::onKnobsLoaded()
{
    updateVisibility();
}

bool
VDBFromPolygons::knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    const KnobI* toggles[] = {
        _useVoxelCount.lock().get(),
        _useWorldSpaceForBand.lock().get(),
        _distanceVDB.lock().get(),
        _fogVDB.lock().get(),
    };
    for (const KnobI* toggle : toggles) {
        if (toggle && (k == toggle)) {
            updateVisibility();

            return true;
        }
    }

    return false;
}

VDBFromPolygonsParams
VDBFromPolygons::readParams(double time,
                            ViewSpec view) const
{
    VDBFromPolygonsParams p;
    GeoPolygonsToSdfParams& c = p.convert;
    p.referenceVDB = geoKnobValueAt<std::string>(_referenceVDB, p.referenceVDB, time, view);
    c.buildDistance = geoKnobValueAt<bool>(_distanceVDB, c.buildDistance, time, view);
    c.distanceName = geoKnobValueAt<std::string>(_distanceName, c.distanceName, time, view);
    c.buildFog = geoKnobValueAt<bool>(_fogVDB, c.buildFog, time, view);
    c.fogName = geoKnobValueAt<std::string>(_fogName, c.fogName, time, view);
    p.useVoxelCount = geoKnobValueAt<bool>(_useVoxelCount, p.useVoxelCount, time, view);
    c.voxelSize = geoKnobValueAt<double>(_voxelSize, c.voxelSize, time, view);
    p.voxelCount = geoKnobValueAt<int>(_voxelCount, p.voxelCount, time, view);
    const int axis = geoKnobValueAt<int>(_voxelCountAxis, int(p.voxelCountAxis), time, view);
    if ((axis >= eGeoVDBAxisX) && (axis <= eGeoVDBAxisLongest)) {
        p.voxelCountAxis = GeoVDBAxisEnum(axis);
    }
    c.useWorldSpaceForBand = geoKnobValueAt<bool>(_useWorldSpaceForBand, c.useWorldSpaceForBand, time, view);
    c.exteriorBand = geoKnobValueAt<double>(_exteriorBand, c.exteriorBand, time, view);
    c.interiorBand = geoKnobValueAt<double>(_interiorBand, c.interiorBand, time, view);
    c.exteriorBandWorld = geoKnobValueAt<double>(_exteriorBandWorld, c.exteriorBandWorld, time, view);
    c.interiorBandWorld = geoKnobValueAt<double>(_interiorBandWorld, c.interiorBandWorld, time, view);
    c.fillInterior = geoKnobValueAt<bool>(_fillInterior, c.fillInterior, time, view);
    c.unsignedDistanceField = geoKnobValueAt<bool>(_unsignedDistanceField, c.unsignedDistanceField, time, view);
    c.attributes = geoKnobValueAt<std::string>(_attributes, c.attributes, time, view);

    return p;
}

StatusEnum
VDBFromPolygons::renderGeometry(const GeoRenderActionArgs& args,
                                GeoDetailConstPtr* outputDetail,
                                std::string* errorMessage)
{
    const VDBFromPolygonsParams params = readParams(args.time, args.view);
    const GeoDetailConstPtr input = args.getInputDetail(0);
    const GeoDetailConstPtr reference = args.getInputDetail(1);
    const GeoDetail empty;
    const GeoDetail& source = input ? *input : empty;
    const NodePtr node = getNode();
    // A tiny voxel size can take long or run out of memory, so the estimate shows during the build.
    const GeoVDBEstimateCallback estimated = geoEstimatePoster(node);
    std::vector<std::string> notes;
    const StatusEnum status = renderGeometryGenerator([&](GeoDetailBuilder& detail, std::string* error) {
        try {
            return build(source, reference.get(), params, detail, &notes, error, estimated) ? eStatusOK : eStatusFailed;
        } catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }

            return eStatusFailed;
        }
    },
                                                      outputDetail, errorMessage);
    if ((status == eStatusOK) && node) {
        node->setQuietInfoMessage(geoJoinNotes(notes));
    }

    return status;
}

NATRON_NAMESPACE_EXIT
