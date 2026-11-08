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

#include "GeoDelete.h"

#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {

bool
isBlank(const std::string& s)
{
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

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
} // anonymous namespace

bool
GeoDelete::deleteElements(GeoDetailBuilder& builder,
                          const GeoDeleteParams& params,
                          std::string* error)
{
    const GeoDetail& detail = builder.detail();
    const GeoAttribClassEnum cls = params.groupType == eGeoGroupTypePoints ? eGeoAttribClassPoint : eGeoAttribClassPrimitive;
    GeoElementMask selected(detail.elementOffsetSize(cls), 0);
    if (!isBlank(params.group) && !geoSelectElements(detail, cls, params.group, &selected, error)) {
        return false;
    }
    if (params.deleteNonSelected) {
        detail.indexMap(cls).forEachLive([&](GeoIndex /*index*/, GeoOffset offset) {
            selected[std::size_t(offset)] = !selected[std::size_t(offset)];
        });
    }
    const std::vector<GeoOffset> doomed = geoMaskedOffsets(selected);
    if (doomed.empty()) {
        return true;
    }

    if (cls == eGeoAttribClassPrimitive) {
        builder.deletePrimitives(doomed, params.deleteUnusedPoints);

        return true;
    }

    std::vector<GeoOffset> primitives;
    detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        const GeoVertexRange range = detail.primitiveVertices(prim);
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            if (selected[std::size_t(detail.vertexPoint(v))]) {
                primitives.push_back(prim);
                break;
            }
        }
    });
    if (!primitives.empty()) {
        builder.deletePrimitives(primitives, false);
    }
    builder.deletePoints(doomed, eGeoPointDeleteDegenerate);

    return true;
}

GeoDelete::GeoDelete(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoDelete::~GeoDelete()
{
}

const NodeSpec*
GeoDelete::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoDelete::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobStringPtr group = createKnob<KnobString>(tr("Group").toStdString());
    group->setName(kGeoDeleteParamGroup);
    group->setHintToolTip(tr("The elements to delete: element numbers and ranges (0-10, 0-20:2), group names or globs, "
                             "* for everything, @attribute comparisons (@P.x<0), and the prefixes ! (everything the "
                             "term does not select) and ^ (remove the term's elements). Blank deletes nothing.")
                              .toStdString());
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    KnobChoicePtr groupType = AppManager::createKnob<KnobChoice>(this, tr("Group Type").toStdString());
    groupType->setName(kGeoDeleteParamGroupType);
    groupType->setHintToolTip(tr("Whether the group selects points or primitives.").toStdString());
    std::vector<ChoiceOption> types;
    types.push_back(ChoiceOption("points", tr("Points").toStdString(),
                                 tr("Deleting a point also deletes the primitives that use it.").toStdString()));
    types.push_back(ChoiceOption("primitives", tr("Primitives").toStdString(), ""));
    groupType->populateChoices(types);
    groupType->setDefaultValue((int)eGeoGroupTypePrimitives);
    groupType->setAnimationEnabled(false);
    page->addKnob(groupType);
    _groupType = groupType;

    KnobBoolPtr nonSelected = AppManager::createKnob<KnobBool>(this, tr("Delete Non Selected").toStdString());
    nonSelected->setName(kGeoDeleteParamDeleteNonSelected);
    nonSelected->setHintToolTip(tr("Deletes every element the group does not select, keeping only the selection.").toStdString());
    nonSelected->setDefaultValue(false);
    nonSelected->setAnimationEnabled(false);
    page->addKnob(nonSelected);
    _deleteNonSelected = nonSelected;

    KnobBoolPtr unused = AppManager::createKnob<KnobBool>(this, tr("Delete Unused Points").toStdString());
    unused->setName(kGeoDeleteParamDeleteUnusedPoints);
    unused->setHintToolTip(tr("Also deletes the points no remaining primitive uses.").toStdString());
    unused->setDefaultValue(true);
    unused->setAnimationEnabled(false);
    page->addKnob(unused);
    _deleteUnusedPoints = unused;

    updateVisibility();
}

void
GeoDelete::updateVisibility()
{
    const KnobChoicePtr groupType = _groupType.lock();
    const KnobBoolPtr unused = _deleteUnusedPoints.lock();
    if (groupType && unused) {
        unused->setSecret(groupType->getValue() != eGeoGroupTypePrimitives);
    }
}

void
GeoDelete::onKnobsLoaded()
{
    updateVisibility();
}

bool
GeoDelete::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    const KnobChoicePtr groupType = _groupType.lock();
    if (groupType && k == groupType.get()) {
        updateVisibility();

        return true;
    }

    return false;
}

StatusEnum
GeoDelete::renderGeometry(const GeoRenderActionArgs& args,
                          GeoDetailConstPtr* outputDetail,
                          std::string* errorMessage)
{
    GeoDeleteParams p;
    p.group = valueAt<std::string>(_group, p.group, args.time, args.view);
    p.groupType = valueAt<int>(_groupType, (int)p.groupType, args.time, args.view) == eGeoGroupTypePoints ? eGeoGroupTypePoints
                                                                                                          : eGeoGroupTypePrimitives;
    p.deleteNonSelected = valueAt<bool>(_deleteNonSelected, p.deleteNonSelected, args.time, args.view);
    p.deleteUnusedPoints = valueAt<bool>(_deleteUnusedPoints, p.deleteUnusedPoints, args.time, args.view);

    return renderGeometryFromInput(args, 0, [&p](GeoDetailBuilder& detail, std::string* error) {
        try {
            return deleteElements(detail, p, error) ? eStatusOK : eStatusFailed;
        } catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }

            return eStatusFailed;
        } }, outputDetail, errorMessage);
}

NATRON_NAMESPACE_EXIT
