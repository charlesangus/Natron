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
#include <memory>
#include <string>
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
ConvertVDB::apply(GeoDetailBuilder& builder,
                  const ConvertVDBParams& params,
                  const GeoDetail* reference,
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

    std::vector<GeoOffset> meshed;
    for (const Selected& s : selected) {
        const bool scalar = s.grid->isType<openvdb::FloatGrid>() || s.grid->isType<openvdb::DoubleGrid>();
        const bool fog = s.grid->getGridClass() == openvdb::GRID_FOG_VOLUME;
        if (!scalar) {
            if (notes) {
                notes->push_back(s.name + " holds " + s.grid->valueType() + " values, not float or double ones, so it was left as it is.");
            }
            continue;
        }
        switch (params.target) {
        case eConvertVDBTargetPolygons: {
            GeoSdfToPolygonsParams mesh;
            mesh.isovalue = fog ? params.fogIsovalue : params.isovalue;
            mesh.adaptivity = params.adaptivity;
            mesh.referenceAttributes = params.referenceAttributes;
            if (!geoSdfToPolygons(*s.grid, mesh, reference, builder, error)) {
                return false;
            }
            meshed.push_back(s.primitive);
            break;
        }
        case eConvertVDBTargetFog: {
            if (fog) {
                break;
            }
            const GeoVDBGridBaseConstPtr converted = geoSdfToFog(*s.grid, error);
            if (!converted) {
                return false;
            }
            geoReplaceVDBGrid(builder, s.primitive, converted);
            break;
        }
        case eConvertVDBTargetSDF: {
            if (!fog) {
                break;
            }
            const GeoVDBGridBaseConstPtr converted = geoFogToSdf(*s.grid, params.fogIsovalue, error);
            if (!converted) {
                return false;
            }
            geoReplaceVDBGrid(builder, s.primitive, converted);
            break;
        }
        }
    }
    if (!meshed.empty()) {
        builder.deletePrimitives(meshed, true);
    }

    return true;
}

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

    KnobStringPtr group = createKnob<KnobString>(tr("Group").toStdString());
    group->setName(kConvertVDBParamGroup);
    group->setHintToolTip(tr("The VDB primitives to convert, by name: space-separated globs, ^ in front of one "
                             "to leave out what it matches. * converts every one.")
                              .toStdString());
    group->setDefaultValue(defaults.group);
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    KnobChoicePtr convertTo = createKnob<KnobChoice>(tr("Convert To").toStdString());
    convertTo->setName(kConvertVDBParamConvertTo);
    convertTo->setHintToolTip(tr("What the selected grids become. Grids already of that kind are left as they "
                                 "are.")
                                  .toStdString());
    {
        std::vector<ChoiceOption> targets;
        targets.push_back(ChoiceOption("polygons", tr("Polygons").toStdString(),
                                       tr("Quads and triangles along the isosurface, facing out of the inside: "
                                          "below the isovalue for a distance field, above it for a fog volume.")
                                           .toStdString()));
        targets.push_back(ChoiceOption("fog", tr("Fog").toStdString(),
                                       tr("A fog volume from a signed distance field: 1 inside, 0 outside, "
                                          "ramping across the interior band.")
                                           .toStdString()));
        targets.push_back(ChoiceOption("sdf", tr("SDF").toStdString(),
                                       tr("A narrow-band signed distance field from a fog volume, whose surface is "
                                          "where the density crosses Fog Isovalue.")
                                           .toStdString()));
        convertTo->populateChoices(targets);
    }
    convertTo->setAnimationEnabled(false);
    page->addKnob(convertTo);
    _convertTo = convertTo;

    KnobDoublePtr isovalue = createKnob<KnobDouble>(tr("Isovalue").toStdString());
    isovalue->setName(kConvertVDBParamIsovalue);
    isovalue->setHintToolTip(tr("The level of the surface in grids that are not fog volumes, in world units for "
                                "a distance field: above 0 the surface grows outwards.")
                                 .toStdString());
    isovalue->setDefaultValue(defaults.isovalue);
    isovalue->setDisplayMinimum(-1.);
    isovalue->setDisplayMaximum(1.);
    page->addKnob(isovalue);
    _isovalue = isovalue;

    KnobDoublePtr fogIsovalue = createKnob<KnobDouble>(tr("Fog Isovalue").toStdString());
    fogIsovalue->setName(kConvertVDBParamFogIsovalue);
    fogIsovalue->setHintToolTip(tr("The density at the surface of fog volumes.").toStdString());
    fogIsovalue->setDefaultValue(defaults.fogIsovalue);
    fogIsovalue->setDisplayMinimum(0.);
    fogIsovalue->setDisplayMaximum(1.);
    page->addKnob(fogIsovalue);
    _fogIsovalue = fogIsovalue;

    KnobDoublePtr adaptivity = createKnob<KnobDouble>(tr("Adaptivity").toStdString());
    adaptivity->setName(kConvertVDBParamAdaptivity);
    adaptivity->setHintToolTip(tr("0 makes every polygon about a voxel in size; towards 1, flatter regions are "
                                  "merged into fewer, larger polygons.")
                                   .toStdString());
    adaptivity->setDefaultValue(defaults.adaptivity);
    adaptivity->setMinimum(0.);
    adaptivity->setMaximum(1.);
    adaptivity->setDisplayMinimum(0.);
    adaptivity->setDisplayMaximum(1.);
    page->addKnob(adaptivity);
    _adaptivity = adaptivity;

    KnobStringPtr referenceAttributes = createKnob<KnobString>(tr("Reference Attributes").toStdString());
    referenceAttributes->setName(kConvertVDBParamReferenceAttributes);
    referenceAttributes->setHintToolTip(tr("With geometry on the Reference input, the points of the polygons copy "
                                           "these point attributes from the closest reference point: "
                                           "space-separated globs, ^ in front of one to leave out what it "
                                           "matches.")
                                            .toStdString());
    referenceAttributes->setDefaultValue(defaults.referenceAttributes);
    referenceAttributes->setAnimationEnabled(false);
    page->addKnob(referenceAttributes);
    _referenceAttributes = referenceAttributes;

    updateVisibility();
}

void
ConvertVDB::updateVisibility()
{
    const KnobChoicePtr convertTo = _convertTo.lock();
    const int target = convertTo ? convertTo->getValue() : (int)eConvertVDBTargetPolygons;
    const bool polygons = target == eConvertVDBTargetPolygons;
    setVisible<KnobDoublePtr>(_isovalue, polygons);
    setVisible<KnobDoublePtr>(_fogIsovalue, target != eConvertVDBTargetFog);
    setVisible<KnobDoublePtr>(_adaptivity, polygons);
    setVisible<KnobStringPtr>(_referenceAttributes, polygons);
}

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
    const KnobChoicePtr convertTo = _convertTo.lock();
    if (convertTo && (k == convertTo.get())) {
        updateVisibility();

        return true;
    }

    return false;
}

ConvertVDBParams
ConvertVDB::readParams(double time,
                       ViewSpec view) const
{
    ConvertVDBParams p;
    p.group = valueAt<std::string>(_group, p.group, time, view);
    const int target = valueAt<int>(_convertTo, (int)p.target, time, view);
    if ((target >= eConvertVDBTargetPolygons) && (target <= eConvertVDBTargetSDF)) {
        p.target = (ConvertVDBTargetEnum)target;
    }
    p.isovalue = valueAt<double>(_isovalue, p.isovalue, time, view);
    p.fogIsovalue = valueAt<double>(_fogIsovalue, p.fogIsovalue, time, view);
    p.adaptivity = valueAt<double>(_adaptivity, p.adaptivity, time, view);
    p.referenceAttributes = valueAt<std::string>(_referenceAttributes, p.referenceAttributes, time, view);

    return p;
}

StatusEnum
ConvertVDB::renderGeometry(const GeoRenderActionArgs& args,
                           GeoDetailConstPtr* outputDetail,
                           std::string* errorMessage)
{
    const ConvertVDBParams params = readParams(args.time, args.view);
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
