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

#include "Serialization/Merge/MergeHandler.h"

#include <array>
#include <map>
#include <set>
#include <tuple>

#include "Serialization/Merge/MergeRules.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

using MergeDetail::mergedOrder;
using MergeDetail::namesBefore;
using MergeDetail::Pick;
using MergeDetail::pick;
using MergeDetail::takeChanged;

namespace {

    typedef std::tuple<std::list<BezierCPSerialization>, std::list<BezierCPSerialization>, bool, bool> BezierGeometry;
    typedef std::tuple<int, std::list<RotoStrokeCurvesSerialization>> StrokeGeometry;
    typedef std::tuple<bool, bool, int, double, double, bool> ContextSettings;

    const RotoItemSerialization&
    itemOf(const RotoLayerChildSerialization& child)
    {
        switch (child.type) {
        case eRotoLayerChildTypeBezier:
            return child.bezier;
        case eRotoLayerChildTypeStroke:
            return child.stroke;
        case eRotoLayerChildTypeLayer:
            break;
        }

        return child.layer;
    }

    const RotoDrawableItemSerialization*
    drawableOf(const RotoLayerChildSerialization& child)
    {
        switch (child.type) {
        case eRotoLayerChildTypeBezier:
            return &child.bezier;
        case eRotoLayerChildTypeStroke:
            return &child.stroke;
        case eRotoLayerChildTypeLayer:
            break;
        }

        return nullptr;
    }

    RotoDrawableItemSerialization*
    drawableOf(RotoLayerChildSerialization* child)
    {
        return const_cast<RotoDrawableItemSerialization*>(drawableOf(*child));
    }

    BezierGeometry
    geometryOf(const BezierSerialization& bezier)
    {
        return BezierGeometry(bezier.controlPoints, bezier.featherPoints, bezier.closed, bezier.isOpenBezier);
    }

    StrokeGeometry
    geometryOf(const RotoStrokeItemSerialization& stroke)
    {
        return StrokeGeometry(stroke.brushType, stroke.strokes);
    }

    std::array<double, 4>
    overlayOf(const RotoDrawableItemSerialization& item)
    {
        return { item.overlayColor[0], item.overlayColor[1], item.overlayColor[2], item.overlayColor[3] };
    }

    ContextSettings
    settingsOf(const RotoContextSerialization& context)
    {
        return ContextSettings(context.autoKeying, context.rippleEdit, context.rippleEditMode, context.rippleEditRangeFirst, context.rippleEditRangeLast, context.featherLink);
    }

    std::tuple<bool, SplineWarpSerialization>
    splineWarpOf(const RotoContextSerialization& context)
    {
        return std::make_tuple(context.hasSplineWarp, context.hasSplineWarp ? context.splineWarp : SplineWarpSerialization());
    }

    std::map<std::string, const RotoLayerChildSerialization*>
    childrenByName(const RotoLayerSerialization* layer)
    {
        std::map<std::string, const RotoLayerChildSerialization*> result;
        if (layer) {
            for (const RotoLayerChildSerialization& child : layer->children) {
                result.emplace(itemOf(child).name, &child);
            }
        }

        return result;
    }

    const RotoLayerChildSerialization*
    childNamed(const std::map<std::string, const RotoLayerChildSerialization*>& children,
               const std::string& name)
    {
        std::map<std::string, const RotoLayerChildSerialization*>::const_iterator found = children.find(name);

        return (found == children.end()) ? nullptr : found->second;
    }

    std::vector<std::string>
    childNames(const RotoLayerSerialization& layer)
    {
        std::vector<std::string> result;
        for (const RotoLayerChildSerialization& child : layer.children) {
            result.push_back(itemOf(child).name);
        }

        return result;
    }

    bool sameLayer(const RotoLayerSerialization& a,
                   const RotoLayerSerialization& b);

    bool
    sameItem(const RotoItemSerialization& a,
             const RotoItemSerialization& b)
    {
        return (a.name == b.name) && (a.label == b.label) && (a.activated == b.activated) && (a.locked == b.locked);
    }

    bool
    sameChild(const RotoLayerChildSerialization& a,
              const RotoLayerChildSerialization& b)
    {
        if ((a.type != b.type) || !sameItem(itemOf(a), itemOf(b))) {
            return false;
        }
        if (a.type == eRotoLayerChildTypeLayer) {
            return sameLayer(a.layer, b.layer);
        }
        const RotoDrawableItemSerialization& left = *drawableOf(a);
        const RotoDrawableItemSerialization& right = *drawableOf(b);
        if ((overlayOf(left) != overlayOf(right)) || !sameKnobLists(left.knobs, right.knobs)) {
            return false;
        }

        return (a.type == eRotoLayerChildTypeBezier) ? (geometryOf(a.bezier) == geometryOf(b.bezier)) : (geometryOf(a.stroke) == geometryOf(b.stroke));
    }

    bool
    sameLayer(const RotoLayerSerialization& a,
              const RotoLayerSerialization& b)
    {
        if (!sameItem(a, b) || (a.children.size() != b.children.size())) {
            return false;
        }
        std::list<RotoLayerChildSerialization>::const_iterator right = b.children.begin();
        for (const RotoLayerChildSerialization& child : a.children) {
            if (!sameChild(child, *right)) {
                return false;
            }
            ++right;
        }

        return true;
    }

    std::optional<RotoContextSerialization>
    itemAlone(const RotoLayerChildSerialization* child)
    {
        if (!child) {
            return std::nullopt;
        }
        RotoContextSerialization context;
        context.baseLayer.children.push_back(*child);

        return context;
    }

    std::optional<RotoContextSerialization>
    splineWarpAlone(const RotoContextSerialization* roto)
    {
        if (!roto) {
            return std::nullopt;
        }
        RotoContextSerialization context;
        context.hasSplineWarp = roto->hasSplineWarp;
        if (roto->hasSplineWarp) {
            context.splineWarp = roto->splineWarp;
        }

        return context;
    }

    void
    addItemConflict(MergeConflictKind kind,
                    const std::string& item,
                    MergeSide side,
                    std::optional<RotoContextSerialization> base,
                    std::optional<RotoContextSerialization> mine,
                    std::optional<RotoContextSerialization> theirs,
                    std::vector<MergeConflict>* conflicts)
    {
        MergeConflict conflict;
        conflict.kind = kind;
        conflict.item = item;
        conflict.side = side;
        conflict.base.roto = std::move(base);
        conflict.mine.roto = std::move(mine);
        conflict.theirs.roto = std::move(theirs);
        conflicts->push_back(conflict);
    }

    void
    mergeItemAppearance(const RotoItemSerialization* base,
                        RotoItemSerialization* merged,
                        const RotoItemSerialization& theirs)
    {
        if (!base) {
            return;
        }
        takeChanged(base->label, &merged->label, theirs.label);
        takeChanged(base->activated, &merged->activated, theirs.activated);
        takeChanged(base->locked, &merged->locked, theirs.locked);
    }

    RotoLayerSerialization mergeLayer(const RotoLayerSerialization* base,
                                      const RotoLayerSerialization& mine,
                                      const RotoLayerSerialization& theirs,
                                      const MergeHandlerRegistry& handlers,
                                      std::vector<MergeConflict>* conflicts);

    struct ItemSides {
        const RotoLayerChildSerialization* base;
        const RotoLayerChildSerialization& mine;
        const RotoLayerChildSerialization& theirs;
    };

    void
    addShapeConflict(const ItemSides& sides,
                     std::vector<MergeConflict>* conflicts)
    {
        addItemConflict(MergeConflictKind::eRotoShape, itemOf(sides.mine).name, MergeSide::eMine, itemAlone(sides.base), itemAlone(&sides.mine), itemAlone(&sides.theirs), conflicts);
    }

    template <typename Geometry>
    void
    mergeGeometry(const std::optional<Geometry>& base,
                  const Geometry& mine,
                  const Geometry& theirs,
                  const ItemSides& sides,
                  Geometry* merged,
                  std::vector<MergeConflict>* conflicts)
    {
        const Pick choice = pick(base, std::optional<Geometry>(mine), std::optional<Geometry>(theirs));
        if (choice == Pick::eConflict) {
            addShapeConflict(sides, conflicts);
        }
        *merged = (choice == Pick::eTheirs) ? theirs : mine;
    }

    RotoLayerChildSerialization
    mergeChild(const RotoLayerChildSerialization* base,
               const RotoLayerChildSerialization& mine,
               const RotoLayerChildSerialization& theirs,
               const MergeHandlerRegistry& handlers,
               std::vector<MergeConflict>* conflicts)
    {
        const std::string& name = itemOf(mine).name;
        const ItemSides sides { base, mine, theirs };
        if (mine.type != theirs.type) {
            addShapeConflict(sides, conflicts);

            return mine;
        }
        if (base && (base->type != mine.type)) {
            base = nullptr;
        }
        RotoLayerChildSerialization result = mine;
        if (mine.type == eRotoLayerChildTypeLayer) {
            result.layer = mergeLayer(base ? &base->layer : nullptr, mine.layer, theirs.layer, handlers, conflicts);

            return result;
        }

        const RotoDrawableItemSerialization* baseItem = base ? drawableOf(*base) : nullptr;
        RotoDrawableItemSerialization* merged = drawableOf(&result);
        const RotoDrawableItemSerialization& theirsItem = *drawableOf(theirs);
        mergeItemAppearance(baseItem, merged, theirsItem);
        if (baseItem) {
            std::array<double, 4> overlay = overlayOf(*merged);
            takeChanged(overlayOf(*baseItem), &overlay, overlayOf(theirsItem));
            std::copy(overlay.begin(), overlay.end(), merged->overlayColor);
        }
        std::vector<MergeConflict> knobConflicts;
        merged->knobs = mergeKnobLists(handlers, std::string(), baseItem ? &baseItem->knobs : nullptr, merged->knobs, theirsItem.knobs, &knobConflicts);
        for (MergeConflict& conflict : knobConflicts) {
            conflict.item = name;
            conflicts->push_back(std::move(conflict));
        }

        if (mine.type == eRotoLayerChildTypeBezier) {
            BezierGeometry geometry;
            mergeGeometry(base ? std::optional<BezierGeometry>(geometryOf(base->bezier)) : std::nullopt, geometryOf(mine.bezier), geometryOf(theirs.bezier), sides, &geometry, conflicts);
            std::tie(result.bezier.controlPoints, result.bezier.featherPoints, result.bezier.closed, result.bezier.isOpenBezier) = geometry;
        } else {
            StrokeGeometry geometry;
            mergeGeometry(base ? std::optional<StrokeGeometry>(geometryOf(base->stroke)) : std::nullopt, geometryOf(mine.stroke), geometryOf(theirs.stroke), sides, &geometry, conflicts);
            std::tie(result.stroke.brushType, result.stroke.strokes) = geometry;
        }

        return result;
    }

    RotoLayerSerialization
    mergeLayer(const RotoLayerSerialization* base,
               const RotoLayerSerialization& mine,
               const RotoLayerSerialization& theirs,
               const MergeHandlerRegistry& handlers,
               std::vector<MergeConflict>* conflicts)
    {
        RotoLayerSerialization result = mine;
        mergeItemAppearance(base, &result, theirs);

        const std::map<std::string, const RotoLayerChildSerialization*> baseChildren = childrenByName(base);
        const std::map<std::string, const RotoLayerChildSerialization*> mineChildren = childrenByName(&mine);
        const std::map<std::string, const RotoLayerChildSerialization*> theirsChildren = childrenByName(&theirs);
        std::map<std::string, RotoLayerChildSerialization> kept;
        for (const std::map<std::string, const RotoLayerChildSerialization*>* side : { &mineChildren, &theirsChildren }) {
            for (const std::pair<const std::string, const RotoLayerChildSerialization*>& entry : *side) {
                const std::string& name = entry.first;
                if (kept.count(name)) {
                    continue;
                }
                const RotoLayerChildSerialization* b = childNamed(baseChildren, name);
                const RotoLayerChildSerialization* m = childNamed(mineChildren, name);
                const RotoLayerChildSerialization* t = childNamed(theirsChildren, name);
                if (m && t) {
                    kept.emplace(name, mergeChild(b, *m, *t, handlers, conflicts));
                    continue;
                }
                const RotoLayerChildSerialization* present = m ? m : t;
                if (!b) {
                    kept.emplace(name, *present);
                } else if (!sameChild(*b, *present)) {
                    addItemConflict(MergeConflictKind::eDeleteModify, name, m ? MergeSide::eTheirs : MergeSide::eMine, itemAlone(b), itemAlone(m), itemAlone(t), conflicts);
                    conflicts->back().base.itemsBefore = namesBefore(childNames(*base), name);
                    conflicts->back().mine.itemsBefore = namesBefore(childNames(mine), name);
                    conflicts->back().theirs.itemsBefore = namesBefore(childNames(theirs), name);
                    kept.emplace(name, *present);
                }
            }
        }

        std::set<std::string> keptNames;
        for (const std::pair<const std::string, RotoLayerChildSerialization>& entry : kept) {
            keptNames.insert(entry.first);
        }
        result.children.clear();
        for (const std::string& name : mergedOrder(childNames(mine), childNames(theirs), keptNames)) {
            result.children.push_back(std::move(kept.at(name)));
        }

        return result;
    }

} // namespace

namespace MergeDetail {

    bool
    sameRotoContexts(const RotoContextSerialization& a,
                     const RotoContextSerialization& b)
    {
        return sameLayer(a.baseLayer, b.baseLayer) && (settingsOf(a) == settingsOf(b)) && (splineWarpOf(a) == splineWarpOf(b));
    }

} // namespace MergeDetail

RotoContextSerialization
mergeRotoContexts(const RotoContextSerialization* base,
                  const RotoContextSerialization& mine,
                  const RotoContextSerialization& theirs,
                  const MergeHandlerRegistry& handlers,
                  std::vector<MergeConflict>* conflicts)
{
    RotoContextSerialization result = mine;
    result.baseLayer = mergeLayer(base ? &base->baseLayer : nullptr, mine.baseLayer, theirs.baseLayer, handlers, conflicts);
    if (base) {
        ContextSettings settings = settingsOf(mine);
        takeChanged(settingsOf(*base), &settings, settingsOf(theirs));
        std::tie(result.autoKeying, result.rippleEdit, result.rippleEditMode, result.rippleEditRangeFirst, result.rippleEditRangeLast, result.featherLink) = settings;
    }
    const std::optional<std::tuple<bool, SplineWarpSerialization>> baseWarp = base ? std::optional<std::tuple<bool, SplineWarpSerialization>>(splineWarpOf(*base)) : std::nullopt;
    const Pick warp = pick(baseWarp, std::optional<std::tuple<bool, SplineWarpSerialization>>(splineWarpOf(mine)), std::optional<std::tuple<bool, SplineWarpSerialization>>(splineWarpOf(theirs)));
    if (warp == Pick::eConflict) {
        addItemConflict(MergeConflictKind::eRotoShape, std::string(), MergeSide::eMine, splineWarpAlone(base), splineWarpAlone(&mine), splineWarpAlone(&theirs), conflicts);
    } else if (warp == Pick::eTheirs) {
        result.hasSplineWarp = theirs.hasSplineWarp;
        result.splineWarp = theirs.splineWarp;
    }

    return result;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
