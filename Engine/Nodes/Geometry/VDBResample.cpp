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

#include "VDBResample.h"

#include <cmath>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Geo/GeoOpenVDB.h"

#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Geo/GeoVDB.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoNodeKnobUtils.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kTransformOrders[] = { "srt", "str", "rst", "rts", "tsr", "trs" };
const char* const kRotateOrders[] = { "xyz", "xzy", "yxz", "yzx", "zxy", "zyx" };

bool
fail(std::string* error,
     const std::string& message)
{
    if (error) {
        *error = message;
    }

    return false;
}

} // anonymous namespace

bool
VDBResample::apply(GeoDetailBuilder& builder,
                   const VDBResampleParams& params,
                   const GeoDetail* reference,
                   std::vector<std::string>* notes,
                   std::string* error)
{
    GeoVDBGridBaseConstPtr target;
    switch (params.mode) {
    case eVDBResampleModeVoxelSize:
        if (!std::isfinite(params.voxelSize) || !(params.voxelSize > 0.)) {
            return fail(error, "The voxel size must be above 0.");
        }
        break;
    case eVDBResampleModeVoxelScale:
        if (!std::isfinite(params.voxelScale) || !(params.voxelScale > 0.)) {
            return fail(error, "The voxel scale must be above 0.");
        }
        break;
    case eVDBResampleModeReference:
        if (reference) {
            target = geoFindVDB(*reference, params.referenceVDB);
        }
        if (!target) {
            return fail(error, "Resampling to a reference needs a VDB primitive on the Reference input that "
                               "Reference VDB matches.");
        }
        break;
    case eVDBResampleModeExplicit:
        for (const double s : params.transform.scale) {
            if (!std::isfinite(s) || (s == 0.)) {
                return fail(error, "The scale cannot be 0 on any axis.");
            }
        }
        break;
    }

    std::vector<std::pair<GeoOffset, GeoVDBGridBaseConstPtr>> selected;
    {
        const GeoDetail& d = builder.detail();
        d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
            if ((d.primitiveType(prim) == eGeoPrimitiveVDB) && geoNameMatchesPattern(params.group, geoVDBName(d, prim))) {
                selected.emplace_back(prim, geoVDBGrid(d, prim));
            }
        });
    }
    for (const auto& [prim, grid] : selected) {
        std::string why;
        GeoVDBGridBaseConstPtr resampled;
        if (params.mode == eVDBResampleModeExplicit) {
            resampled = geoTransformVDB(*grid, params.transform, params.resample, &why);
        } else {
            std::shared_ptr<openvdb::math::Transform> xform;
            switch (params.mode) {
            case eVDBResampleModeVoxelSize:
                xform = geoVoxelSizedVDBTransform(*grid, params.voxelSize, params.forceAxisAligned);
                break;
            case eVDBResampleModeVoxelScale:
                xform = geoScaledVDBTransform(*grid, params.voxelScale, params.forceAxisAligned);
                break;
            case eVDBResampleModeReference:
            case eVDBResampleModeExplicit:
                xform = target->transform().copy();
                break;
            }
            resampled = geoResampleVDB(*grid, *xform, params.resample, &why);
        }
        if (!resampled) {
            if (notes) {
                notes->push_back(why + " It was left as it is.");
            }
            continue;
        }
        geoReplaceVDBGrid(builder, prim, resampled);
    }

    return true;
} // VDBResample::apply

VDBResample::VDBResample(NodePtr node)
    : NativeEffectBase(node)
{
}

VDBResample::~VDBResample()
{
}

const NodeSpec*
VDBResample::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
VDBResample::initializeKnobs()
{
    const VDBResampleParams defaults;
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    _group = geoAddStringKnob(this, page, tr("Group").toStdString(), kVDBResampleParamGroup,
                              tr("The VDB primitives to resample, by name: space-separated globs, ^ in front of one "
                                 "to leave out what it matches. * resamples every one.")
                                  .toStdString(),
                              defaults.group);
    _referenceVDB = geoAddStringKnob(this, page, tr("Reference VDB").toStdString(), kVDBResampleParamReferenceVDB,
                                     tr("In Reference mode, the VDB of the Reference input, by name pattern, whose "
                                        "voxels the grids take; the first that matches is used.")
                                         .toStdString(),
                                     defaults.referenceVDB);
    {
        std::vector<ChoiceOption> filters;
        filters.push_back(ChoiceOption("point", tr("Point").toStdString(),
                                       tr("The closest voxel's value: fastest, blocky.").toStdString()));
        filters.push_back(ChoiceOption("linear", tr("Linear").toStdString(),
                                       tr("Trilinear between the eight closest voxels.").toStdString()));
        filters.push_back(ChoiceOption("quadratic", tr("Quadratic").toStdString(),
                                       tr("Triquadratic over the 27 closest voxels: smoother, slower.").toStdString()));
        _interpolation = geoAddChoiceKnob(this, page, tr("Interpolation").toStdString(), kVDBResampleParamInterpolation,
                                          tr("How the grids are sampled, unless an SDF is rebuilt. Integer grids "
                                             "always take the closest voxel's value.")
                                              .toStdString(),
                                          filters, int(defaults.resample.interpolation));
    }
    {
        std::vector<ChoiceOption> modes;
        modes.push_back(ChoiceOption("voxelSize", tr("Voxel Size").toStdString(),
                                     tr("Voxels of Voxel Size in world units, keeping each grid's orientation and "
                                        "origin unless Force Axis Aligned is on.")
                                         .toStdString()));
        modes.push_back(ChoiceOption("reference", tr("Reference").toStdString(),
                                     tr("The voxels of the Reference VDB, so the grids line up voxel for voxel.")
                                         .toStdString()));
        modes.push_back(ChoiceOption("voxelScale", tr("Voxel Scale").toStdString(),
                                     tr("Each grid's own voxels scaled by Voxel Scale: 2 makes them twice as large "
                                        "and the grid eight times smaller.")
                                         .toStdString()));
        modes.push_back(ChoiceOption("explicit", tr("Explicit").toStdString(),
                                     tr("The grids' values moved by the transform below, in index space: the "
                                        "translation is in voxels and the grids keep their voxels.")
                                         .toStdString()));
        _mode = geoAddChoiceKnob(this, page, tr("Mode").toStdString(), kVDBResampleParamMode,
                                 tr("Where the new voxels come from.").toStdString(), modes, int(defaults.mode));
    }
    {
        std::vector<ChoiceOption> orders;
        for (const char* order : kTransformOrders) {
            std::string label;
            for (const char* c = order; *c; ++c) {
                label += std::string(label.empty() ? "" : " ") + (*c == 's' ? "Scale" : (*c == 'r' ? "Rotate" : "Translate"));
            }
            orders.push_back(ChoiceOption(order, label, ""));
        }
        _transformOrder = geoAddChoiceKnob(this, page, tr("Transform Order").toStdString(), kVDBResampleParamTransformOrder,
                                           tr("The order the scale, rotation and translation apply in, first to "
                                              "last.")
                                               .toStdString(),
                                           orders, 0);
    }
    {
        std::vector<ChoiceOption> orders;
        for (const char* order : kRotateOrders) {
            std::string label(order);
            for (char& c : label) {
                c = char(c - 'a' + 'A');
            }
            orders.push_back(ChoiceOption(order, label, ""));
        }
        _rotateOrder = geoAddChoiceKnob(this, page, tr("Rotate Order").toStdString(), kVDBResampleParamRotateOrder,
                                        tr("The order the axis rotations apply in, first to last.").toStdString(),
                                        orders, 0);
    }
    const auto addVector = [&](const QString& label, const char* name, const QString& toolTip, double value,
                               double displayMinimum, double displayMaximum) {
        KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(this, label.toStdString(), 3);
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        for (int d = 0; d < 3; ++d) {
            knob->setDefaultValue(value, d);
            knob->setDisplayMinimum(displayMinimum, d);
            knob->setDisplayMaximum(displayMaximum, d);
        }
        page->addKnob(knob);

        return knob;
    };
    _translateVoxels = addVector(tr("Translate Voxels"), kVDBResampleParamTranslateVoxels,
                                 tr("The translation, in voxels."), 0., -10., 10.);
    _rotate = addVector(tr("Rotate"), kVDBResampleParamRotate, tr("The rotation about each axis, in degrees."), 0.,
                        -180., 180.);
    _scale = addVector(tr("Scale"), kVDBResampleParamScale, tr("The scale along each axis. Not 0."), 1., 0.1, 4.);
    _pivot = addVector(tr("Pivot"), kVDBResampleParamPivot,
                       tr("The point, in index space, that the rotation and scale are about."), 0., -100., 100.);

    KnobDoublePtr voxelSize = geoAddDoubleKnob(this, page, tr("Voxel Size").toStdString(), kVDBResampleParamVoxelSize,
                                               tr("The edge length of the new voxels in world units.").toStdString(),
                                               defaults.voxelSize, 0., 1.);
    voxelSize->setDisplayMinimum(0.001);
    _voxelSize = voxelSize;
    KnobDoublePtr voxelScale = geoAddDoubleKnob(this, page, tr("Voxel Scale").toStdString(), kVDBResampleParamVoxelScale,
                                                tr("How many times larger the new voxels are than each grid's own.")
                                                    .toStdString(),
                                                defaults.voxelScale, 0., 4.);
    voxelScale->setDisplayMinimum(0.1);
    _voxelScale = voxelScale;
    _forceAxisAligned = geoAddBoolKnob(this, page, tr("Force Axis Aligned").toStdString(), kVDBResampleParamForceAxisAligned,
                                       tr("Put the new voxels on a fresh transform aligned with the world axes, at the "
                                          "grid's origin, instead of the grid's own scaled, which may be rotated or "
                                          "tapered.")
                                           .toStdString(),
                                       defaults.forceAxisAligned);
    _transformVectors = geoAddBoolKnob(this, page, tr("Transform Vectors").toStdString(), kVDBResampleParamTransformVectors,
                                       tr("In Explicit mode, transform the values of vector grids too, as their vector "
                                          "type says: directions rotate, positions move as well.")
                                           .toStdString(),
                                       defaults.transform.transformVectors);
    _rebuildSDF = geoAddBoolKnob(this, page, tr("Rebuild SDF").toStdString(), kVDBResampleParamRebuildSDF,
                                 tr("Rebuild SDFs from their surface, with the same band width in voxels, so that "
                                    "their values stay distances after scaling. Other grids are sampled.")
                                     .toStdString(),
                                 defaults.resample.rebuildSdf);
    _pruneTolerance = geoAddDoubleKnob(this, page, tr("Prune Tolerance").toStdString(), kVDBResampleParamPruneTolerance,
                                       tr("Voxels within this of the background, which resampling leaves active, are "
                                          "made inactive, then regions of values within it of each other collapse "
                                          "into tiles.")
                                           .toStdString(),
                                       defaults.resample.pruneTolerance, 0., 1.);

    updateVisibility();
} // VDBResample::initializeKnobs

void
VDBResample::updateVisibility()
{
    const KnobChoicePtr mode = _mode.lock();
    const int m = mode ? mode->getValue() : int(eVDBResampleModeVoxelSize);
    const bool explicitMode = m == eVDBResampleModeExplicit;
    geoSetKnobVisible(_referenceVDB, m == eVDBResampleModeReference);
    geoSetKnobVisible(_voxelSize, m == eVDBResampleModeVoxelSize);
    geoSetKnobVisible(_voxelScale, m == eVDBResampleModeVoxelScale);
    geoSetKnobVisible(_forceAxisAligned, (m == eVDBResampleModeVoxelSize) || (m == eVDBResampleModeVoxelScale));
    geoSetKnobVisible(_transformOrder, explicitMode);
    geoSetKnobVisible(_rotateOrder, explicitMode);
    geoSetKnobVisible(_translateVoxels, explicitMode);
    geoSetKnobVisible(_rotate, explicitMode);
    geoSetKnobVisible(_scale, explicitMode);
    geoSetKnobVisible(_pivot, explicitMode);
    geoSetKnobVisible(_transformVectors, explicitMode);
}

void
VDBResample::onKnobsLoaded()
{
    updateVisibility();
}

bool
VDBResample::knobChanged(KnobI* k,
                         ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    const KnobChoicePtr mode = _mode.lock();
    if (mode && (k == mode.get())) {
        updateVisibility();

        return true;
    }

    return false;
}

VDBResampleParams
VDBResample::readParams(double time,
                        ViewSpec view) const
{
    VDBResampleParams p;
    p.group = geoKnobValueAt<std::string>(_group, p.group, time, view);
    p.referenceVDB = geoKnobValueAt<std::string>(_referenceVDB, p.referenceVDB, time, view);
    const int mode = geoKnobValueAt<int>(_mode, int(p.mode), time, view);
    if ((mode >= eVDBResampleModeVoxelSize) && (mode <= eVDBResampleModeExplicit)) {
        p.mode = VDBResampleModeEnum(mode);
    }
    p.voxelSize = geoKnobValueAt<double>(_voxelSize, p.voxelSize, time, view);
    p.voxelScale = geoKnobValueAt<double>(_voxelScale, p.voxelScale, time, view);
    p.forceAxisAligned = geoKnobValueAt<bool>(_forceAxisAligned, p.forceAxisAligned, time, view);
    const int interpolation = geoKnobValueAt<int>(_interpolation, int(p.resample.interpolation), time, view);
    if ((interpolation >= eGeoVDBInterpolationPoint) && (interpolation <= eGeoVDBInterpolationQuadratic)) {
        p.resample.interpolation = GeoVDBInterpolationEnum(interpolation);
    }
    p.resample.rebuildSdf = geoKnobValueAt<bool>(_rebuildSDF, p.resample.rebuildSdf, time, view);
    p.resample.pruneTolerance = geoKnobValueAt<double>(_pruneTolerance, p.resample.pruneTolerance, time, view);

    GeoVDBIndexTransform& t = p.transform;
    const int order = geoKnobValueAt<int>(_transformOrder, 0, time, view);
    if ((order >= 0) && (order < int(sizeof(kTransformOrders) / sizeof(kTransformOrders[0])))) {
        t.order = kTransformOrders[order];
    }
    const int rotateOrder = geoKnobValueAt<int>(_rotateOrder, 0, time, view);
    if ((rotateOrder >= 0) && (rotateOrder < int(sizeof(kRotateOrders) / sizeof(kRotateOrders[0])))) {
        t.rotateOrder = kRotateOrders[rotateOrder];
    }
    for (int d = 0; d < 3; ++d) {
        t.translate[d] = geoKnobValueAt<double>(_translateVoxels, t.translate[d], time, view, d);
        t.rotate[d] = geoKnobValueAt<double>(_rotate, t.rotate[d], time, view, d);
        t.scale[d] = geoKnobValueAt<double>(_scale, t.scale[d], time, view, d);
        t.pivot[d] = geoKnobValueAt<double>(_pivot, t.pivot[d], time, view, d);
    }
    t.transformVectors = geoKnobValueAt<bool>(_transformVectors, t.transformVectors, time, view);

    return p;
} // VDBResample::readParams

StatusEnum
VDBResample::renderGeometry(const GeoRenderActionArgs& args,
                            GeoDetailConstPtr* outputDetail,
                            std::string* errorMessage)
{
    const VDBResampleParams params = readParams(args.time, args.view);
    const GeoDetailConstPtr reference = args.getInputDetail(1);
    std::vector<std::string> notes;
    const StatusEnum status = renderGeometryFromInput(args, 0, [&](GeoDetailBuilder& detail, std::string* error) {
        try {
            return apply(detail, params, reference.get(), &notes, error) ? eStatusOK : eStatusFailed;
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
