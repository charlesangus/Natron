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

#include "Engine/Nodes/Geometry/GeoGroup.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <QString>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Geo/GeoGroupOps.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {

GeoAttribClassEnum
elementClass(GeoGroupElementEnum elements)
{
    switch (elements) {
    case eGeoGroupElementPoints:
        return eGeoAttribClassPoint;
    case eGeoGroupElementVertices:
        return eGeoAttribClassVertex;
    case eGeoGroupElementPrimitives:
    case eGeoGroupElementEdges:
        break;
    }

    return eGeoAttribClassPrimitive;
}

bool
insideBounding(const GeoGroupParams& params,
               const std::array<double, 3>& p)
{
    // Relative slack, so points exactly on a face count as inside despite rounding.
    const double eps = 1e-9;
    if (params.boundingType == eGeoBoundingTypeBox) {
        for (int d = 0; d < 3; ++d) {
            const double half = std::abs(params.size[d]) * 0.5;
            if (std::abs(p[d] - params.centre[d]) > half + eps * std::max(1., half)) {
                return false;
            }
        }

        return true;
    }
    double sum = 0.;
    for (int d = 0; d < 3; ++d) {
        const double half = std::abs(params.size[d]) * 0.5;
        const double delta = p[d] - params.centre[d];
        if (half <= 0.) {
            if (std::abs(delta) > eps) {
                return false;
            }
            continue;
        }
        sum += (delta / half) * (delta / half);
    }

    return sum <= 1. + eps;
}

GeoElementMask
pointsInside(const GeoDetail& detail,
             const GeoGroupParams& params)
{
    const std::vector<std::array<double, 3>> positions = geoPointPositions(detail);
    GeoElementMask inside(positions.size(), 0);
    detail.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset p) {
        inside[std::size_t(p)] = insideBounding(params, positions[std::size_t(p)]);
    });

    return inside;
}

GeoElementMask
boundingMask(const GeoDetail& detail,
             GeoAttribClassEnum cls,
             const GeoGroupParams& params,
             const GeoElementMask& inside)
{
    if (cls == eGeoAttribClassPoint) {
        return inside;
    }
    GeoElementMask mask(detail.elementOffsetSize(cls), 0);
    if (cls == eGeoAttribClassVertex) {
        detail.vertexMap().forEachLive([&](GeoIndex /*index*/, GeoOffset v) {
            mask[std::size_t(v)] = inside[std::size_t(detail.vertexPoint(v))];
        });

        return mask;
    }
    detail.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        const GeoVertexRange range = detail.primitiveVertices(prim);
        std::size_t in = 0;
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            in += inside[std::size_t(detail.vertexPoint(v))] ? 1 : 0;
        }
        mask[std::size_t(prim)] = range.count > 0 && (params.includePartlyInside ? in > 0 : in == std::size_t(range.count));
    });

    return mask;
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

KnobChoicePtr
makeChoiceKnob(GeoGroup* holder,
               const KnobPagePtr& page,
               const QString& label,
               const char* name,
               const QString& toolTip,
               const std::vector<ChoiceOption>& options,
               int defaultValue)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, label.toStdString());
    knob->setName(name);
    knob->setHintToolTip(toolTip.toStdString());
    knob->populateChoices(options);
    knob->setDefaultValue(defaultValue);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

KnobBoolPtr
makeBoolKnob(GeoGroup* holder,
             const KnobPagePtr& page,
             const QString& label,
             const char* name,
             const QString& toolTip,
             bool defaultValue)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label.toStdString());
    knob->setName(name);
    knob->setHintToolTip(toolTip.toStdString());
    knob->setDefaultValue(defaultValue);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

KnobDoublePtr
makeVectorKnob(GeoGroup* holder,
               const KnobPagePtr& page,
               const QString& label,
               const char* name,
               const QString& toolTip,
               double defaultValue)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label.toStdString(), 3);
    knob->setName(name);
    knob->setHintToolTip(toolTip.toStdString());
    for (int d = 0; d < 3; ++d) {
        knob->setDefaultValue(defaultValue, d);
    }
    page->addKnob(knob);

    return knob;
}

template <typename PtrType>
void
setKnobSecret(const std::weak_ptr<PtrType>& knob,
              bool secret)
{
    if (const std::shared_ptr<PtrType> locked = knob.lock()) {
        locked->setSecret(secret);
    }
}
} // anonymous namespace

bool
GeoGroup::selectEdges(const GeoDetail& detail,
                      const std::string& pattern,
                      std::set<GeoEdge>* edges,
                      std::string* error)
{
    return geoSelectEdges(detail, pattern, edges, error);
}

static bool
buildSelection(GeoDetailBuilder& builder,
               const GeoGroupParams& params,
               std::string* error)
{
    if (params.name.empty()) {
        if (error) {
            *error = "the group needs a name";
        }

        return false;
    }
    const GeoDetail& detail = builder.detail();
    const bool any = params.usePattern || params.useBounding;
    const GeoElementMask inside = params.useBounding ? pointsInside(detail, params) : GeoElementMask();

    if (params.elements == eGeoGroupElementEdges) {
        std::set<GeoEdge> selected;
        if (params.usePattern && !geoSelectEdges(detail, params.pattern, &selected, error)) {
            return false;
        }
        if (params.useBounding) {
            std::set<GeoEdge> bounded;
            for (const GeoEdge& e : params.usePattern ? selected : geoAllEdges(detail)) {
                const bool a = inside[std::size_t(e.p0)];
                const bool b = inside[std::size_t(e.p1)];
                if (params.includePartlyInside ? (a || b) : (a && b)) {
                    bounded.insert(e);
                }
            }
            selected.swap(bounded);
        }
        std::vector<GeoEdge> previous;
        if (const GeoEdgeGroup* existing = detail.findEdgeGroup(params.name)) {
            previous.assign(existing->edges().begin(), existing->edges().end());
        }
        GeoEdgeGroup& group = builder.addEdgeGroup(params.name);
        switch (params.merge) {
        case eGeoGroupMergeReplace:
            group.clear();
            group.add(std::vector<GeoEdge>(selected.begin(), selected.end()));
            break;
        case eGeoGroupMergeUnion:
            group.add(std::vector<GeoEdge>(selected.begin(), selected.end()));
            break;
        case eGeoGroupMergeIntersect:
            for (const GeoEdge& e : previous) {
                if (!selected.count(e)) {
                    group.remove(e.p0, e.p1);
                }
            }
            break;
        case eGeoGroupMergeSubtract:
            for (const GeoEdge& e : selected) {
                group.remove(e.p0, e.p1);
            }
            break;
        }

        return true;
    }

    const GeoAttribClassEnum cls = elementClass(params.elements);
    GeoElementMask selected(detail.elementOffsetSize(cls), 0);
    if (params.usePattern && !geoSelectElements(detail, cls, params.pattern, &selected, error)) {
        return false;
    }
    if (params.useBounding) {
        const GeoElementMask bounded = boundingMask(detail, cls, params, inside);
        for (std::size_t i = 0; i < selected.size(); ++i) {
            selected[i] = (params.usePattern ? selected[i] : char(1)) && bounded[i];
        }
    }
    if (!any) {
        std::fill(selected.begin(), selected.end(), char(0));
    }
    const GeoIndexMap& map = detail.indexMap(cls);
    std::vector<GeoOffset> toAdd;
    std::vector<GeoOffset> toRemove;
    const GeoElementGroup* existing = detail.findGroup(cls, params.name);
    map.forEachLive([&](GeoIndex /*index*/, GeoOffset offset) {
        const bool was = existing && existing->contains(offset);
        const bool sel = selected[std::size_t(offset)];
        bool now = was;
        switch (params.merge) {
        case eGeoGroupMergeReplace:
            now = sel;
            break;
        case eGeoGroupMergeUnion:
            now = was || sel;
            break;
        case eGeoGroupMergeIntersect:
            now = was && sel;
            break;
        case eGeoGroupMergeSubtract:
            now = was && !sel;
            break;
        }
        if (now && !was) {
            toAdd.push_back(offset);
        } else if (was && !now) {
            toRemove.push_back(offset);
        }
    });
    GeoElementGroup& group = builder.addGroup(cls, params.name);
    group.add(toAdd);
    for (GeoOffset offset : toRemove) {
        group.remove(offset);
    }

    return true;
}

namespace {

GeoAttribClassEnum
promoteClass(GeoGroupPromoteEnum to)
{
    switch (to) {
    case eGeoGroupPromotePoints:
        return eGeoAttribClassPoint;
    case eGeoGroupPromoteEdges:
        break;
    case eGeoGroupPromotePrimitives:
        return eGeoAttribClassPrimitive;
    }

    return eGeoAttribClassVertex;
}

GeoElementMask
groupMask(const GeoDetail& detail,
          GeoAttribClassEnum cls,
          const std::string& name)
{
    GeoElementMask mask(detail.elementOffsetSize(cls), 0);
    if (const GeoElementGroup* group = detail.findGroup(cls, name)) {
        detail.indexMap(cls).forEachLive([&](GeoIndex /*index*/, GeoOffset offset) {
            mask[std::size_t(offset)] = group->contains(offset);
        });
    }

    return mask;
}

void
storeMask(GeoDetailBuilder& builder,
          GeoAttribClassEnum cls,
          const std::string& name,
          const GeoElementMask& mask)
{
    GeoElementGroup& group = builder.addGroup(cls, name);
    group.clear();
    group.add(geoMaskedOffsets(mask));
}

void
storeEdges(GeoDetailBuilder& builder,
           const std::string& name,
           const std::vector<GeoEdge>& edges)
{
    GeoEdgeGroup& group = builder.addEdgeGroup(name);
    group.clear();
    group.add(edges);
}

bool
editGroup(GeoDetailBuilder& builder,
          const GeoGroupParams& params,
          std::string* error)
{
    if (params.elements == eGeoGroupElementVertices) {
        if (error) {
            *error = "Expand and Promote need a group type of points, edges or primitives";
        }

        return false;
    }
    const bool fromEdges = params.elements == eGeoGroupElementEdges;
    const GeoAttribClassEnum fromClass = elementClass(params.elements);
    const GeoDetail& detail = builder.detail();

    GeoElementMask mask;
    std::vector<GeoEdge> edges;
    if (fromEdges) {
        if (const GeoEdgeGroup* group = detail.findEdgeGroup(params.name)) {
            edges.assign(group->edges().begin(), group->edges().end());
        }
        if (params.expand) {
            edges = geoExpandEdges(detail, edges, params.expandSteps);
        }
    } else {
        mask = groupMask(detail, fromClass, params.name);
        if (params.expand) {
            mask = fromClass == eGeoAttribClassPoint ? geoExpandPoints(detail, mask, params.expandSteps)
                                                     : geoExpandPrimitives(detail, mask, params.expandSteps);
        }
    }

    const bool toEdges = params.promote && params.promoteTo == eGeoGroupPromoteEdges;
    const bool toSame = !params.promote || (toEdges ? fromEdges : (!fromEdges && promoteClass(params.promoteTo) == fromClass));
    if (toSame) {
        if (params.expand) {
            if (fromEdges) {
                storeEdges(builder, params.name, edges);
            } else {
                storeMask(builder, fromClass, params.name, mask);
            }
        }

        return true;
    }

    if (toEdges) {
        const std::vector<GeoEdge> result = fromClass == eGeoAttribClassPoint ? geoPromotePointsToEdges(detail, mask, params.entirelyContained)
                                                                              : geoPromotePrimitivesToEdges(detail, mask);
        storeEdges(builder, params.name, result);
    } else {
        const GeoAttribClassEnum toClass = promoteClass(params.promoteTo);
        GeoElementMask result;
        if (fromEdges) {
            result = toClass == eGeoAttribClassPoint ? geoPromoteEdgesToPoints(detail, edges) : geoPromoteEdgesToPrimitives(detail, edges);
        } else if (toClass == eGeoAttribClassPoint) {
            result = geoPromotePrimitivesToPoints(detail, mask);
        } else {
            result = geoPromotePointsToPrimitives(detail, mask, params.entirelyContained);
        }
        storeMask(builder, toClass, params.name, result);
    }
    if (!params.keepOriginal) {
        if (fromEdges) {
            builder.removeEdgeGroup(params.name);
        } else {
            builder.removeGroup(fromClass, params.name);
        }
    }

    return true;
}
} // anonymous namespace

bool
GeoGroup::createGroup(GeoDetailBuilder& builder,
                      const GeoGroupParams& params,
                      std::string* error)
{
    if (!buildSelection(builder, params, error)) {
        return false;
    }

    return !(params.expand || params.promote) || editGroup(builder, params, error);
}

GeoGroup::GeoGroup(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoGroup::~GeoGroup()
{
}

const NodeSpec*
GeoGroup::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoGroup::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobStringPtr name = createKnob<KnobString>(tr("Group Name").toStdString());
    name->setName(kGeoGroupParamGroupName);
    name->setHintToolTip(tr("The name of the group to create or edit.").toStdString());
    name->setDefaultValue("group1");
    name->setAnimationEnabled(false);
    page->addKnob(name);
    _groupName = name;

    std::vector<ChoiceOption> types;
    types.push_back(ChoiceOption("points", tr("Points").toStdString(), ""));
    types.push_back(ChoiceOption("primitives", tr("Primitives").toStdString(), ""));
    types.push_back(ChoiceOption("vertices", tr("Vertices").toStdString(), ""));
    types.push_back(ChoiceOption("edges", tr("Edges").toStdString(), tr("Pairs of points joined by a polygon or polyline side.").toStdString()));
    _groupType = makeChoiceKnob(this, page, tr("Group Type"), kGeoGroupParamGroupType, tr("The kind of element the group holds."),
                                types, (int)eGeoGroupElementPrimitives);

    std::vector<ChoiceOption> merges;
    merges.push_back(ChoiceOption("replace", tr("Replace").toStdString(), tr("The group holds exactly the selection.").toStdString()));
    merges.push_back(ChoiceOption("union", tr("Union").toStdString(), tr("The selection is added to the group's members.").toStdString()));
    merges.push_back(ChoiceOption("intersect", tr("Intersect").toStdString(),
                                  tr("Only the members that are also selected stay.").toStdString()));
    merges.push_back(ChoiceOption("subtract", tr("Subtract").toStdString(), tr("The selection is removed from the group.").toStdString()));
    _initialMerge = makeChoiceKnob(this, page, tr("Initial Merge"), kGeoGroupParamInitialMerge,
                                   tr("How the selection combines with the members a group of this name already has."),
                                   merges, (int)eGeoGroupMergeReplace);

    _usePattern = makeBoolKnob(this, page, tr("Use Pattern"), kGeoGroupParamUsePattern,
                               tr("Selects with the pattern."), true);

    KnobStringPtr pattern = createKnob<KnobString>(tr("Pattern").toStdString());
    pattern->setName(kGeoGroupParamPattern);
    pattern->setHintToolTip(tr("Space-separated terms applied left to right: element numbers and ranges (0-10, 0-20:2, "
                               "0-9:2,3 for two of every three), group names or globs, * for everything, @attribute "
                               "comparisons (@P.y>0, @name==piece*), and the prefixes ! (everything the term does not "
                               "select) and ^ (remove the term's elements). For edges, p0-1-2 names the edges along a "
                               "path of point numbers. Empty selects everything.")
                                .toStdString());
    pattern->setAnimationEnabled(false);
    page->addKnob(pattern);
    _pattern = pattern;

    _useBounding = makeBoolKnob(this, page, tr("Use Bounding"), kGeoGroupParamUseBounding,
                                tr("Keeps only what lies inside the bounding box or sphere."), false);

    std::vector<ChoiceOption> shapes;
    shapes.push_back(ChoiceOption("box", tr("Box").toStdString(), ""));
    shapes.push_back(ChoiceOption("sphere", tr("Sphere").toStdString(), tr("An ellipsoid filling the box.").toStdString()));
    _boundingType = makeChoiceKnob(this, page, tr("Bounding Type"), kGeoGroupParamBoundingType,
                                   tr("The shape of the bounding region."), shapes, (int)eGeoBoundingTypeBox);
    _centre = makeVectorKnob(this, page, tr("Centre"), kGeoGroupParamCentre, tr("The centre of the bounding region."), 0.);
    _size = makeVectorKnob(this, page, tr("Size"), kGeoGroupParamSize,
                           tr("The bounding region's size along the X, Y and Z axes."), 1.);
    _includePartlyInside = makeBoolKnob(this, page, tr("Include Partly Inside"), kGeoGroupParamIncludePartlyInside,
                                        tr("Primitives and edges with any of their points inside count as inside; "
                                           "otherwise all their points must be."),
                                        false);

    _expand = makeBoolKnob(this, page, tr("Expand"), kGeoGroupParamExpand,
                           tr("Grows the group by Expand Steps rings of neighbouring elements. Points neighbour each other "
                              "through a shared edge, primitives through a shared edge, and edges through a shared point. "
                              "Needs a group type of points, edges or primitives."),
                           false);
    KnobIntPtr steps = createKnob<KnobInt>(tr("Expand Steps").toStdString());
    steps->setName(kGeoGroupParamExpandSteps);
    steps->setHintToolTip(tr("How many rings of neighbours to add. A negative count shrinks the group instead, "
                             "removing the elements on its boundary.")
                              .toStdString());
    steps->setDefaultValue(1);
    steps->setAnimationEnabled(false);
    page->addKnob(steps);
    _expandSteps = steps;

    _promote = makeBoolKnob(this, page, tr("Promote"), kGeoGroupParamPromote,
                            tr("Converts the group into a group of the same name holding another kind of element. Runs "
                               "after Expand. Needs a group type of points, edges or primitives."),
                            false);
    std::vector<ChoiceOption> targets;
    targets.push_back(ChoiceOption("points", tr("Points").toStdString(), tr("Every point of the group's elements.").toStdString()));
    targets.push_back(ChoiceOption("edges", tr("Edges").toStdString(), tr("The edges of the group's elements.").toStdString()));
    targets.push_back(ChoiceOption("primitives", tr("Primitives").toStdString(), tr("The primitives that use the group's elements.").toStdString()));
    _promoteTo = makeChoiceKnob(this, page, tr("Promote To"), kGeoGroupParamPromoteTo,
                                tr("The kind of element the converted group holds."), targets, (int)eGeoGroupPromotePoints);
    _entirelyContained = makeBoolKnob(this, page, tr("Entirely Contained"), kGeoGroupParamEntirelyContained,
                                      tr("Promoting points to edges or primitives keeps only those whose points are all in the "
                                         "group; otherwise one point is enough."),
                                      false);
    _keepOriginal = makeBoolKnob(this, page, tr("Keep Original"), kGeoGroupParamKeepOriginal,
                                 tr("Keeps the group of the original kind next to the converted one; otherwise it is removed."),
                                 false);

    updateVisibility();
}

void
GeoGroup::updateVisibility()
{
    const KnobBoolPtr usePattern = _usePattern.lock();
    const KnobBoolPtr useBounding = _useBounding.lock();
    const KnobChoicePtr groupType = _groupType.lock();
    if (!usePattern || !useBounding || !groupType) {
        return;
    }
    const bool bounding = useBounding->getValue();
    const int type = groupType->getValue();
    setKnobSecret(_pattern, !usePattern->getValue());
    setKnobSecret(_boundingType, !bounding);
    setKnobSecret(_centre, !bounding);
    setKnobSecret(_size, !bounding);
    const KnobBoolPtr expand = _expand.lock();
    const KnobBoolPtr promote = _promote.lock();
    setKnobSecret(_expandSteps, !expand || !expand->getValue());
    setKnobSecret(_promoteTo, !promote || !promote->getValue());
    setKnobSecret(_entirelyContained, !promote || !promote->getValue());
    setKnobSecret(_keepOriginal, !promote || !promote->getValue());
    setKnobSecret(_includePartlyInside, !bounding || (type != eGeoGroupElementPrimitives && type != eGeoGroupElementEdges));
}

void
GeoGroup::onKnobsLoaded()
{
    updateVisibility();
}

bool
GeoGroup::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec /*view*/,
                      double /*time*/,
                      bool /*originatedFromMainThread*/)
{
    const KnobBoolPtr usePattern = _usePattern.lock();
    const KnobBoolPtr useBounding = _useBounding.lock();
    const KnobChoicePtr groupType = _groupType.lock();
    const KnobBoolPtr expand = _expand.lock();
    const KnobBoolPtr promote = _promote.lock();
    if ((expand && k == expand.get()) || (promote && k == promote.get()) || (usePattern && k == usePattern.get()) || (useBounding && k == useBounding.get()) || (groupType && k == groupType.get())) {
        updateVisibility();

        return true;
    }

    return false;
}

StatusEnum
GeoGroup::renderGeometry(const GeoRenderActionArgs& args,
                         GeoDetailConstPtr* outputDetail,
                         std::string* errorMessage)
{
    GeoGroupParams p;
    const double time = args.time;
    const ViewSpec view = args.view;
    p.name = valueAt<std::string>(_groupName, p.name, time, view);
    const int type = valueAt<int>(_groupType, (int)p.elements, time, view);
    if (type >= eGeoGroupElementPoints && type <= eGeoGroupElementEdges) {
        p.elements = (GeoGroupElementEnum)type;
    }
    const int merge = valueAt<int>(_initialMerge, (int)p.merge, time, view);
    if (merge >= eGeoGroupMergeReplace && merge <= eGeoGroupMergeSubtract) {
        p.merge = (GeoGroupMergeEnum)merge;
    }
    p.usePattern = valueAt<bool>(_usePattern, p.usePattern, time, view);
    p.pattern = valueAt<std::string>(_pattern, p.pattern, time, view);
    p.useBounding = valueAt<bool>(_useBounding, p.useBounding, time, view);
    p.boundingType = valueAt<int>(_boundingType, 0, time, view) == eGeoBoundingTypeSphere ? eGeoBoundingTypeSphere : eGeoBoundingTypeBox;
    for (int d = 0; d < 3; ++d) {
        p.centre[d] = valueAt<double>(_centre, p.centre[d], time, view, d);
        p.size[d] = valueAt<double>(_size, p.size[d], time, view, d);
    }
    p.includePartlyInside = valueAt<bool>(_includePartlyInside, p.includePartlyInside, time, view);

    p.expand = valueAt<bool>(_expand, p.expand, time, view);
    p.expandSteps = valueAt<int>(_expandSteps, p.expandSteps, time, view);
    p.promote = valueAt<bool>(_promote, p.promote, time, view);
    const int target = valueAt<int>(_promoteTo, (int)p.promoteTo, time, view);
    if (target >= eGeoGroupPromotePoints && target <= eGeoGroupPromotePrimitives) {
        p.promoteTo = (GeoGroupPromoteEnum)target;
    }
    p.entirelyContained = valueAt<bool>(_entirelyContained, p.entirelyContained, time, view);
    p.keepOriginal = valueAt<bool>(_keepOriginal, p.keepOriginal, time, view);

    return renderGeometryFromInput(args, 0, [&p](GeoDetailBuilder& detail, std::string* error) {
        try {
            return createGroup(detail, p, error) ? eStatusOK : eStatusFailed;
        } catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }

            return eStatusFailed;
        } }, outputDetail, errorMessage);
}

NATRON_NAMESPACE_EXIT
