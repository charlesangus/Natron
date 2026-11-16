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
#include <cstdint>
#include <exception>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
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
} // anonymous namespace

namespace {

bool
isBlank(const std::string& text)
{
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

struct EdgeRun {
    GeoOffset sourcePrimitive = 0;
    std::vector<GeoOffset> sourceVertices;
};

// Splits each polygon and polyline that uses a deleted edge into the open runs of edges that
// survive; a polygon is cut at its deleted edges, so a run starts right after one.
void
deleteEdges(GeoDetailBuilder& builder,
            const std::set<GeoEdge>& doomed)
{
    const GeoDetail& detail = builder.detail();
    std::vector<EdgeRun> runs;
    std::vector<GeoOffset> affected;
    detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        const GeoPrimitiveTypeEnum type = detail.primitiveType(prim);
        if (type != eGeoPrimitivePolygon && type != eGeoPrimitivePolyline) {
            return;
        }
        const GeoVertexRange range = detail.primitiveVertices(prim);
        if (range.count < 2) {
            return;
        }
        const bool closed = type == eGeoPrimitivePolygon && range.count > 2;
        const GeoOffset segments = closed ? range.count : range.count - 1;
        std::vector<char> cut(std::size_t(segments), 0);
        bool anyCut = false;
        for (GeoOffset i = 0; i < segments; ++i) {
            const GeoOffset a = detail.vertexPoint(range.start + i);
            const GeoOffset b = detail.vertexPoint(range.start + (i + 1) % range.count);
            if (a != b && doomed.count(GeoEdge(a, b))) {
                cut[std::size_t(i)] = 1;
                anyCut = true;
            }
        }
        if (!anyCut) {
            return;
        }
        affected.push_back(prim);

        // A closed polygon begins after a cut so no run wraps around its seam.
        GeoOffset first = 0;
        if (closed) {
            while (!cut[std::size_t(first)]) {
                ++first;
            }
            first = (first + 1) % segments;
        }
        EdgeRun run;
        run.sourcePrimitive = prim;
        const auto flush = [&] {
            if (!run.sourceVertices.empty()) {
                runs.push_back(std::move(run));
                run = EdgeRun();
                run.sourcePrimitive = prim;
            }
        };
        for (GeoOffset n = 0; n < segments; ++n) {
            const GeoOffset i = (first + n) % segments;
            if (cut[std::size_t(i)]) {
                flush();
                continue;
            }
            if (run.sourceVertices.empty()) {
                run.sourceVertices.push_back(range.start + i);
            }
            run.sourceVertices.push_back(range.start + (i + 1) % range.count);
        }
        flush();
    });
    if (affected.empty()) {
        return;
    }

    if (!runs.empty()) {
        std::vector<std::int32_t> counts;
        std::vector<GeoOffset> points;
        for (const EdgeRun& run : runs) {
            counts.push_back(std::int32_t(run.sourceVertices.size()));
            for (const GeoOffset v : run.sourceVertices) {
                points.push_back(detail.vertexPoint(v));
            }
        }
        const GeoOffset firstPrim = builder.appendPrimitives(eGeoPrimitivePolyline, counts, points);

        // Names are listed first: writing an attribute replaces the page it writes to.
        const auto carry = [&](GeoAttribClassEnum cls) {
            std::vector<std::string> names;
            for (std::size_t i = 0; i < detail.attributeCount(cls); ++i) {
                names.push_back(detail.attribute(cls, i).desc().name);
            }
            for (const std::string& name : names) {
                GeoAttribute* attribute = builder.attributeForWriting(cls, name);
                if (!attribute) {
                    continue;
                }
                for (std::size_t r = 0; r < runs.size(); ++r) {
                    const GeoOffset prim = firstPrim + GeoOffset(r);
                    if (cls == eGeoAttribClassPrimitive) {
                        attribute->copyElement(std::size_t(prim), std::size_t(runs[r].sourcePrimitive));
                        continue;
                    }
                    const GeoVertexRange range = builder.detail().primitiveVertices(prim);
                    for (GeoOffset k = 0; k < range.count; ++k) {
                        attribute->copyElement(std::size_t(range.start + k), std::size_t(runs[r].sourceVertices[std::size_t(k)]));
                    }
                }
            }
        };
        carry(eGeoAttribClassPrimitive);
        carry(eGeoAttribClassVertex);
    }
    builder.deletePrimitives(affected, false);
}
} // anonymous namespace

bool
GeoDelete::deleteElements(GeoDetailBuilder& builder,
                          const GeoDeleteParams& params,
                          std::string* error)
{
    const GeoDetail& detail = builder.detail();
    GeoGroupTypeEnum groupType = params.groupType;
    if (groupType == eGeoGroupTypeGuess) {
        groupType = eGeoGroupTypePrimitives;
        if (!isBlank(params.group)) {
            const GeoGroupTypeEnum allowed[] = { eGeoGroupTypePoints, eGeoGroupTypeEdges, eGeoGroupTypePrimitives };
            if (!geoGuessGroupType(detail, params.group, allowed, eGeoGroupTypePrimitives, &groupType, error)) {
                return false;
            }
        }
    }
    if (groupType == eGeoGroupTypeEdges) {
        std::set<GeoEdge> selected;
        if (!geoSelectEdges(detail, params.group, &selected, error)) {
            return false;
        }
        if (params.deleteNonSelected) {
            std::set<GeoEdge> others = geoAllEdges(detail);
            for (const GeoEdge& edge : selected) {
                others.erase(edge);
            }
            selected = std::move(others);
        }
        deleteEdges(builder, selected);

        return true;
    }
    const GeoAttribClassEnum cls = groupType == eGeoGroupTypePoints ? eGeoAttribClassPoint : eGeoAttribClassPrimitive;
    GeoElementMask selected(detail.elementOffsetSize(cls), 0);
    if (!geoSelectElements(detail, cls, params.group, &selected, error)) {
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
                             "term does not select) and ^ (remove the term's elements). Blank means every element, so Delete Non "
                             "Selected with a blank group keeps everything.")
                              .toStdString());
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    KnobChoicePtr groupType = AppManager::createKnob<KnobChoice>(this, tr("Group Type").toStdString());
    groupType->setName(kGeoDeleteParamGroupType);
    groupType->setHintToolTip(tr("Whether the group pattern selects points, edges or primitives.").toStdString());
    std::vector<ChoiceOption> types;
    types.push_back(ChoiceOption("points", tr("Points").toStdString(),
                                 tr("Deleting a point also deletes the primitives that use it.").toStdString()));
    types.push_back(ChoiceOption("primitives", tr("Primitives").toStdString(), ""));
    types.push_back(ChoiceOption("edges", tr("Edges").toStdString(),
                                 tr("Opens each polygon at the selected edges and removes the selected segments of polylines. "
                                    "Points are kept.")
                                     .toStdString()));
    types.push_back(ChoiceOption("guess", tr("Guess from Group").toStdString(),
                                 tr("The class the group pattern names: edge paths mean edges, a group name means the class that has it, "
                                    "and plain numbers mean primitives.")
                                     .toStdString()));
    groupType->populateChoices(types);
    groupType->setDefaultValue((int)eGeoGroupTypeGuess);
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
        const int type = groupType->getValue();
        unused->setSecret(type == eGeoGroupTypePoints || type == eGeoGroupTypeEdges);
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
    const int groupType = valueAt<int>(_groupType, (int)p.groupType, args.time, args.view);
    if (groupType >= eGeoGroupTypePoints && groupType <= eGeoGroupTypeGuess) {
        p.groupType = (GeoGroupTypeEnum)groupType;
    }
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
