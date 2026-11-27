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

#include "VDBFromParticles.h"

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

bool
VDBFromParticles::build(const GeoDetail& source,
                        const GeoDetail* reference,
                        const std::string& referenceVDB,
                        bool mergeWithReferenceVDB,
                        GeoPointsToSdfParams params,
                        GeoDetailBuilder& out,
                        std::vector<std::string>* notes,
                        std::string* error,
                        const GeoVDBEstimateCallback& estimated)
{
    std::vector<std::string> referenceNotes;
    if (reference) {
        const GeoVDBGridBaseConstPtr grid = geoFindVDB(*reference, referenceVDB);
        if (grid) {
            params.transform = grid->transformPtr();
            if (mergeWithReferenceVDB) {
                params.mergeInto = grid;
            }
        } else {
            referenceNotes.push_back("No VDB on the Reference input matches Reference VDB, so Voxel Size is used.");
        }
    }
    GeoVDBConvertResult result;
    if (!geoPointsToSdf(source, params, &result, error, estimated)) {
        return false;
    }
    geoAppendVDBGrids(out, result);
    if (notes) {
        // The estimate stays first, where the info showed it during the build.
        const auto rest = result.notes.begin() + std::min<std::ptrdiff_t>(1, std::ptrdiff_t(result.notes.size()));
        notes->insert(notes->end(), result.notes.begin(), rest);
        notes->insert(notes->end(), referenceNotes.begin(), referenceNotes.end());
        notes->insert(notes->end(), rest, result.notes.end());
    }

    return true;
}

VDBFromParticles::VDBFromParticles(NodePtr node)
    : NativeEffectBase(node)
{
}

VDBFromParticles::~VDBFromParticles()
{
}

const NodeSpec*
VDBFromParticles::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
VDBFromParticles::initializeKnobs()
{
    const GeoPointsToSdfParams defaults;
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    _referenceVDB = geoAddStringKnob(this, page, tr("Reference VDB").toStdString(), kVDBFromParticlesParamReferenceVDB,
                                     tr("The VDB of the Reference input, by name pattern, whose voxel size and "
                                        "orientation the grids take; the first that matches is used. Voxel Size "
                                        "applies when the input is not connected or nothing matches.")
                                         .toStdString(),
                                     "*");
    _mergeWithReferenceVDB = geoAddBoolKnob(this, page, tr("Merge With Reference VDB").toStdString(),
                                            kVDBFromParticlesParamMergeWithReferenceVDB,
                                            tr("Union the spheres into the reference VDB, when it is a float SDF, "
                                               "so that a coarse grid can carry the bulk and the points add the "
                                               "detail.")
                                                .toStdString(),
                                            false);

    _distanceVDB = geoAddBoolKnob(this, page, tr("Distance VDB").toStdString(), kVDBFromParticlesParamDistanceVDB,
                                  tr("Output a narrow-band signed distance field: the distance to the spheres' "
                                     "surface in world units, negative inside.")
                                      .toStdString(),
                                  defaults.buildDistance);
    _distanceName = geoAddStringKnob(this, page, tr("Distance Name").toStdString(), kVDBFromParticlesParamDistanceName,
                                     tr("The name of the distance grid.").toStdString(), defaults.distanceName);
    _fogVDB = geoAddBoolKnob(this, page, tr("Fog VDB").toStdString(), kVDBFromParticlesParamFogVDB,
                             tr("Output a density of 0 outside and 1 inside, ramping across the inner half of the "
                                "band.")
                                 .toStdString(),
                             defaults.buildFog);
    _fogName = geoAddStringKnob(this, page, tr("Fog Name").toStdString(), kVDBFromParticlesParamFogName,
                                tr("The name of the fog grid.").toStdString(), defaults.fogName);
    _maskVDB = geoAddBoolKnob(this, page, tr("Mask VDB").toStdString(), kVDBFromParticlesParamMaskVDB,
                              tr("Output a fog volume of the shell around the spheres' surface: the spheres grown "
                                 "by half of Mask Width Scale radii, less the spheres shrunk by as much. It limits "
                                 "later smoothing of the surface to the region the points cover.")
                                  .toStdString(),
                              defaults.buildMask);
    _maskName = geoAddStringKnob(this, page, tr("Mask Name").toStdString(), kVDBFromParticlesParamMaskName,
                                 tr("The name of the mask grid.").toStdString(), defaults.maskName);
    _maskWidthScale = geoAddDoubleKnob(this, page, tr("Mask Width Scale").toStdString(), kVDBFromParticlesParamMaskWidthScale,
                                       tr("The width of the mask's shell, in radii of each sphere.").toStdString(),
                                       defaults.maskWidth, 0., 4.);

    _voxelSize = geoAddDoubleKnob(this, page, tr("Voxel Size").toStdString(), kVDBFromParticlesParamVoxelSize,
                                  tr("The edge length of a voxel in world units. Memory grows with the cube of its "
                                     "inverse; the node's info shows an estimate before building and what the grids "
                                     "use after.")
                                      .toStdString(),
                                  defaults.voxelSize, 0., 1.);
    _radiusScale = geoAddDoubleKnob(this, page, tr("Radius Scale").toStdString(), kVDBFromParticlesParamRadiusScale,
                                    tr("Multiplies each point's pscale, or 1 for points without one, to give its "
                                       "sphere's radius in world units.")
                                        .toStdString(),
                                    defaults.radiusScale, 0., 10.);
    _minRadius = geoAddDoubleKnob(this, page, tr("Min Radius").toStdString(), kVDBFromParticlesParamMinRadius,
                                  tr("Spheres with a smaller radius, in voxels, are left out; the node's info counts "
                                     "them. Below 1.5 the spheres alias. Lower it, or the voxel size, to keep small "
                                     "points.")
                                      .toStdString(),
                                  defaults.minRadius, 0.001, 5.);
    _prune = geoAddBoolKnob(this, page, tr("Prune").toStdString(), kVDBFromParticlesParamPrune,
                            tr("Collapse the constant regions left inside spheres larger than a leaf node, 8 voxels "
                               "across, into tiles.")
                                .toStdString(),
                            defaults.prune);
    _useWorldSpaceForBand = geoAddBoolKnob(this, page, tr("Use World Space For Band").toStdString(),
                                           kVDBFromParticlesParamUseWorldSpaceForBand,
                                           tr("Give the band width in world units instead of voxels.").toStdString(),
                                           defaults.useWorldSpaceForBand);
    _bandWidth = geoAddDoubleKnob(this, page, tr("Band Width").toStdString(), kVDBFromParticlesParamBandWidth,
                                  tr("How far each side of the surface distances are stored, in voxels.").toStdString(),
                                  defaults.bandWidth, 1., 10.);
    _bandWidthWorld = geoAddDoubleKnob(this, page, tr("Band Width World").toStdString(), kVDBFromParticlesParamBandWidthWorld,
                                       tr("How far each side of the surface distances are stored, in world units.")
                                           .toStdString(),
                                       defaults.bandWidthWorld, 0., 1.);

    {
        std::vector<ChoiceOption> footprints;
        footprints.push_back(ChoiceOption("spherical", tr("Spherical").toStdString(),
                                          tr("A sphere on each point.").toStdString()));
        footprints.push_back(ChoiceOption("velocityTrail", tr("Velocity Trail").toStdString(),
                                          tr("Each sphere stretched back along -v times Velocity Scale by a run "
                                             "of spheres shrinking towards Min Radius. Points without v get no "
                                             "trail.")
                                              .toStdString()));
        _particleFootprint = geoAddChoiceKnob(this, page, tr("Particle Footprint").toStdString(),
                                              kVDBFromParticlesParamParticleFootprint,
                                              tr("The shape each point leaves.").toStdString(), footprints,
                                              int(defaults.footprint));
    }
    _velocityScale = geoAddDoubleKnob(this, page, tr("Velocity Scale").toStdString(), kVDBFromParticlesParamVelocityScale,
                                      tr("Multiplies v to give the length and direction of the trail in world units.")
                                          .toStdString(),
                                      defaults.velocityScale, 0., 10.);
    _trailSpacing = geoAddDoubleKnob(this, page, tr("Trail Spacing").toStdString(), kVDBFromParticlesParamTrailSpacing,
                                     tr("The gap between the spheres of a trail, in half-radii. Smaller is smoother "
                                        "and slower.")
                                         .toStdString(),
                                     defaults.trailSpacing, 0.01, 2.);

    _attributes = geoAddStringKnob(this, page, tr("Attributes").toStdString(), kVDBFromParticlesParamAttributes,
                                   tr("Point attributes to carry into grids of their own, each named after its "
                                      "attribute: space-separated globs, ^ in front of one to leave out what it "
                                      "matches. Each voxel holds the value of the sphere whose surface is closest. "
                                      "Float and integer attributes of one or three components convert.")
                                       .toStdString(),
                                   defaults.attributes);

    updateVisibility();
}

void
VDBFromParticles::updateVisibility()
{
    const KnobChoicePtr footprint = _particleFootprint.lock();
    const bool trails = footprint && (footprint->getValue() == int(eGeoParticleFootprintVelocityTrail));
    geoSetKnobVisible(_velocityScale, trails);
    geoSetKnobVisible(_trailSpacing, trails);
    const KnobBoolPtr world = _useWorldSpaceForBand.lock();
    const bool inWorld = world && world->getValue();
    geoSetKnobVisible(_bandWidth, !inWorld);
    geoSetKnobVisible(_bandWidthWorld, inWorld);
    const KnobBoolPtr distance = _distanceVDB.lock();
    geoSetKnobVisible(_distanceName, distance && distance->getValue());
    const KnobBoolPtr fog = _fogVDB.lock();
    geoSetKnobVisible(_fogName, fog && fog->getValue());
    const KnobBoolPtr mask = _maskVDB.lock();
    const bool masked = mask && mask->getValue();
    geoSetKnobVisible(_maskName, masked);
    geoSetKnobVisible(_maskWidthScale, masked);
}

void
VDBFromParticles::onKnobsLoaded()
{
    updateVisibility();
}

bool
VDBFromParticles::knobChanged(KnobI* k,
                              ValueChangedReasonEnum /*reason*/,
                              ViewSpec /*view*/,
                              double /*time*/,
                              bool /*originatedFromMainThread*/)
{
    const KnobI* toggles[] = {
        _particleFootprint.lock().get(),
        _useWorldSpaceForBand.lock().get(),
        _distanceVDB.lock().get(),
        _fogVDB.lock().get(),
        _maskVDB.lock().get(),
    };
    for (const KnobI* toggle : toggles) {
        if (toggle && (k == toggle)) {
            updateVisibility();

            return true;
        }
    }

    return false;
}

GeoPointsToSdfParams
VDBFromParticles::readParams(double time,
                             ViewSpec view) const
{
    GeoPointsToSdfParams p;
    p.buildDistance = geoKnobValueAt<bool>(_distanceVDB, p.buildDistance, time, view);
    p.distanceName = geoKnobValueAt<std::string>(_distanceName, p.distanceName, time, view);
    p.buildFog = geoKnobValueAt<bool>(_fogVDB, p.buildFog, time, view);
    p.fogName = geoKnobValueAt<std::string>(_fogName, p.fogName, time, view);
    p.buildMask = geoKnobValueAt<bool>(_maskVDB, p.buildMask, time, view);
    p.maskName = geoKnobValueAt<std::string>(_maskName, p.maskName, time, view);
    p.maskWidth = geoKnobValueAt<double>(_maskWidthScale, p.maskWidth, time, view);
    p.voxelSize = geoKnobValueAt<double>(_voxelSize, p.voxelSize, time, view);
    p.radiusScale = geoKnobValueAt<double>(_radiusScale, p.radiusScale, time, view);
    p.minRadius = geoKnobValueAt<double>(_minRadius, p.minRadius, time, view);
    p.prune = geoKnobValueAt<bool>(_prune, p.prune, time, view);
    p.useWorldSpaceForBand = geoKnobValueAt<bool>(_useWorldSpaceForBand, p.useWorldSpaceForBand, time, view);
    p.bandWidth = geoKnobValueAt<double>(_bandWidth, p.bandWidth, time, view);
    p.bandWidthWorld = geoKnobValueAt<double>(_bandWidthWorld, p.bandWidthWorld, time, view);
    p.footprint = geoKnobValueAt<int>(_particleFootprint, 0, time, view) == int(eGeoParticleFootprintVelocityTrail)
        ? eGeoParticleFootprintVelocityTrail
        : eGeoParticleFootprintSpherical;
    p.velocityScale = geoKnobValueAt<double>(_velocityScale, p.velocityScale, time, view);
    p.trailSpacing = geoKnobValueAt<double>(_trailSpacing, p.trailSpacing, time, view);
    p.attributes = geoKnobValueAt<std::string>(_attributes, p.attributes, time, view);

    return p;
}

StatusEnum
VDBFromParticles::renderGeometry(const GeoRenderActionArgs& args,
                                 GeoDetailConstPtr* outputDetail,
                                 std::string* errorMessage)
{
    const GeoPointsToSdfParams params = readParams(args.time, args.view);
    const std::string referenceVDB = geoKnobValueAt<std::string>(_referenceVDB, "*", args.time, args.view);
    const bool merge = geoKnobValueAt<bool>(_mergeWithReferenceVDB, false, args.time, args.view);
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
            return build(source, reference.get(), referenceVDB, merge, params, detail, &notes, error, estimated) ? eStatusOK : eStatusFailed;
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
