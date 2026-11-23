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

NATRON_NAMESPACE_ENTER

namespace {

template <typename T>
T
valueAt(const std::weak_ptr<Knob<T>>& knob,
        T fallback,
        double time,
        ViewSpec view)
{
    if (const std::shared_ptr<Knob<T>> locked = knob.lock()) {
        return locked->getValueAtTime(time, 0, view);
    }

    return fallback;
}

template <typename KnobPtrT>
void
setVisible(const std::weak_ptr<typename KnobPtrT::element_type>& knob,
           bool visible)
{
    if (const KnobPtrT locked = knob.lock()) {
        locked->setSecret(!visible);
    }
}

std::string
joinNotes(const std::vector<std::string>& notes)
{
    std::string text;
    for (const std::string& note : notes) {
        text += (text.empty() ? "" : "\n") + note;
    }

    return text;
}

} // namespace

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
            if (error) {
                *error = "The voxel size must be above 0.";
            }

            return false;
        }
        break;
    case eVDBResampleModeVoxelScale:
        if (!std::isfinite(params.voxelScale) || !(params.voxelScale > 0.)) {
            if (error) {
                *error = "The voxel scale must be above 0.";
            }

            return false;
        }
        break;
    case eVDBResampleModeReference:
        if (reference) {
            reference->primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
                if (!target && (reference->primitiveType(prim) == eGeoPrimitiveVDB)) {
                    target = geoVDBGrid(*reference, prim);
                }
            });
        }
        if (!target) {
            if (error) {
                *error = "Resampling to a reference needs a VDB primitive on the Reference input.";
            }

            return false;
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
        std::shared_ptr<openvdb::math::Transform> xform;
        switch (params.mode) {
        case eVDBResampleModeVoxelSize:
            xform = geoVoxelSizedVDBTransform(*grid, params.voxelSize);
            break;
        case eVDBResampleModeVoxelScale:
            xform = geoScaledVDBTransform(*grid, params.voxelScale);
            break;
        case eVDBResampleModeReference:
            xform = target->transform().copy();
            break;
        }
        std::string why;
        const GeoVDBGridBaseConstPtr resampled = geoResampleVDB(*grid, *xform, params.interpolation, &why);
        if (!resampled) {
            if (notes) {
                notes->push_back(why + " It was left as it is.");
            }
            continue;
        }
        geoReplaceVDBGrid(builder, prim, resampled);
    }

    return true;
}

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

    KnobStringPtr group = createKnob<KnobString>(tr("Group").toStdString());
    group->setName(kVDBResampleParamGroup);
    group->setHintToolTip(tr("The VDB primitives to resample, by name: space-separated globs, ^ in front of one "
                             "to leave out what it matches. * resamples every one.")
                              .toStdString());
    group->setDefaultValue(defaults.group);
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode").toStdString());
    mode->setName(kVDBResampleParamMode);
    mode->setHintToolTip(tr("Where the new voxels come from.").toStdString());
    {
        std::vector<ChoiceOption> modes;
        modes.push_back(ChoiceOption("voxelSize", tr("Voxel Size").toStdString(),
                                     tr("Voxels of Voxel Size in world units, keeping each grid's orientation "
                                        "and origin.")
                                         .toStdString()));
        modes.push_back(ChoiceOption("reference", tr("Reference").toStdString(),
                                     tr("The voxels of the first VDB primitive on the Reference input, so the "
                                        "grids line up voxel for voxel.")
                                         .toStdString()));
        modes.push_back(ChoiceOption("voxelScale", tr("Voxel Scale").toStdString(),
                                     tr("Each grid's own voxels scaled by Voxel Scale: 2 makes them twice as large "
                                        "and the grid eight times smaller.")
                                         .toStdString()));
        mode->populateChoices(modes);
    }
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    _mode = mode;

    KnobDoublePtr voxelSize = createKnob<KnobDouble>(tr("Voxel Size").toStdString());
    voxelSize->setName(kVDBResampleParamVoxelSize);
    voxelSize->setHintToolTip(tr("The edge length of the new voxels in world units.").toStdString());
    voxelSize->setDefaultValue(defaults.voxelSize);
    voxelSize->setMinimum(0.);
    voxelSize->setDisplayMinimum(0.001);
    voxelSize->setDisplayMaximum(1.);
    page->addKnob(voxelSize);
    _voxelSize = voxelSize;

    KnobDoublePtr voxelScale = createKnob<KnobDouble>(tr("Voxel Scale").toStdString());
    voxelScale->setName(kVDBResampleParamVoxelScale);
    voxelScale->setHintToolTip(tr("How many times larger the new voxels are than each grid's own.").toStdString());
    voxelScale->setDefaultValue(defaults.voxelScale);
    voxelScale->setMinimum(0.);
    voxelScale->setDisplayMinimum(0.1);
    voxelScale->setDisplayMaximum(4.);
    page->addKnob(voxelScale);
    _voxelScale = voxelScale;

    KnobChoicePtr interpolation = createKnob<KnobChoice>(tr("Interpolation").toStdString());
    interpolation->setName(kVDBResampleParamInterpolation);
    interpolation->setHintToolTip(tr("How grids that are not signed distance fields are sampled.").toStdString());
    {
        std::vector<ChoiceOption> filters;
        filters.push_back(ChoiceOption("point", tr("Point").toStdString(),
                                       tr("The closest voxel's value: fastest, blocky.").toStdString()));
        filters.push_back(ChoiceOption("linear", tr("Linear").toStdString(),
                                       tr("Trilinear between the eight closest voxels.").toStdString()));
        filters.push_back(ChoiceOption("quadratic", tr("Quadratic").toStdString(),
                                       tr("Triquadratic over the 27 closest voxels: smoother, slower.").toStdString()));
        interpolation->populateChoices(filters);
    }
    interpolation->setDefaultValue((int)defaults.interpolation);
    interpolation->setAnimationEnabled(false);
    page->addKnob(interpolation);
    _interpolation = interpolation;

    updateVisibility();
}

void
VDBResample::updateVisibility()
{
    const KnobChoicePtr mode = _mode.lock();
    const int m = mode ? mode->getValue() : (int)eVDBResampleModeVoxelSize;
    setVisible<KnobDoublePtr>(_voxelSize, m == eVDBResampleModeVoxelSize);
    setVisible<KnobDoublePtr>(_voxelScale, m == eVDBResampleModeVoxelScale);
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
    p.group = valueAt<std::string>(_group, p.group, time, view);
    const int mode = valueAt<int>(_mode, (int)p.mode, time, view);
    if ((mode >= eVDBResampleModeVoxelSize) && (mode <= eVDBResampleModeVoxelScale)) {
        p.mode = (VDBResampleModeEnum)mode;
    }
    p.voxelSize = valueAt<double>(_voxelSize, p.voxelSize, time, view);
    p.voxelScale = valueAt<double>(_voxelScale, p.voxelScale, time, view);
    const int interpolation = valueAt<int>(_interpolation, (int)p.interpolation, time, view);
    if ((interpolation >= eGeoVDBInterpolationPoint) && (interpolation <= eGeoVDBInterpolationQuadratic)) {
        p.interpolation = (GeoVDBInterpolationEnum)interpolation;
    }

    return p;
}

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
            setPersistentMessage(eMessageTypeWarning, joinNotes(notes));
        }
    }

    return status;
}

NATRON_NAMESPACE_EXIT
