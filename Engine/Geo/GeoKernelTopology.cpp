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

#include "GeoKernelTopology.h"

#include <algorithm>
#include <climits>
#include <unordered_map>

#include "Engine/Kernel/KernelRunner.h"

NATRON_NAMESPACE_ENTER

GeoKernelTopology::GeoKernelTopology(const GeoDetail& input, std::size_t itemCount)
    : _input(input)
    , _points(numbering(input.pointCount(), itemCount))
    , _primitives(numbering(input.primitiveCount(), itemCount))
    , _vertices(numbering(input.vertexCount(), itemCount))
{
}

GeoKernelTopology::Numbering
GeoKernelTopology::numbering(GeoIndex inputCount, std::size_t itemCount)
{
    Numbering n;
    n.inputCount = inputCount;
    const std::int64_t room = std::int64_t(INT32_MAX) - inputCount;
    n.stride = room > 0 ? room / std::int64_t(std::max<std::size_t>(itemCount, 1)) : 0;
    return n;
}

std::int32_t
GeoKernelTopology::provisional(const Numbering& n, std::size_t item, std::int32_t& counter, const char* what)
{
    if (counter >= n.stride) {
        throw KernelRunError(std::string("too many new ") + what + " for 32-bit element numbers");
    }
    const std::int64_t number = n.inputCount + std::int64_t(item) * n.stride + counter;
    ++counter;
    return std::int32_t(number);
}

std::int32_t
GeoKernelTopology::newPoint(std::size_t item, GeoKernelEditQueue& queue) const
{
    return provisional(_points, item, queue.newPoints, "points");
}

std::int32_t
GeoKernelTopology::newPrimitive(std::size_t item, GeoKernelEditQueue& queue) const
{
    return provisional(_primitives, item, queue.newPrimitives, "primitives");
}

std::int32_t
GeoKernelTopology::newVertex(std::size_t item, GeoKernelEditQueue& queue) const
{
    return provisional(_vertices, item, queue.newVertices, "vertices");
}

std::int32_t
GeoKernelTopology::group(GeoAttribClassEnum cls, const std::string& name)
{
    for (std::size_t k = 0; k < _groups.size(); ++k) {
        if (_groups[k].first == cls && _groups[k].second == name) {
            return std::int32_t(k);
        }
    }
    _groups.emplace_back(cls, name);
    return std::int32_t(_groups.size() - 1);
}

void
GeoKernelTopology::apply(GeoDetailBuilder& builder, std::vector<GeoKernelEditQueue>& queues) const
{
    for (const std::pair<GeoAttribClassEnum, std::string>& g : _groups) {
        builder.addGroup(g.first, g.second);
    }

    std::vector<const GeoKernelEdit*> order;
    for (GeoKernelEditQueue& q : queues) {
        std::stable_sort(q.edits.begin(), q.edits.end(),
                         [](const GeoKernelEdit& x, const GeoKernelEdit& y) { return x.element < y.element; });
        for (const GeoKernelEdit& e : q.edits) {
            order.push_back(&e);
        }
    }
    if (order.empty()) {
        return;
    }

    std::unordered_map<std::int32_t, GeoOffset> newPoints;
    std::unordered_map<std::int32_t, GeoOffset> newPrimitives;
    const auto resolvePoint = [&](std::int32_t number) -> GeoOffset {
        if (number >= 0 && number < _input.pointCount()) {
            return _input.pointMap().offsetFromIndex(number);
        }
        const auto it = newPoints.find(number);
        return it == newPoints.end() ? GeoIndexMap::kInvalidOffset : it->second;
    };
    const auto resolvePrimitive = [&](std::int32_t number) -> GeoOffset {
        if (number >= 0 && number < _input.primitiveCount()) {
            return _input.primitiveMap().offsetFromIndex(number);
        }
        const auto it = newPrimitives.find(number);
        return it == newPrimitives.end() ? GeoIndexMap::kInvalidOffset : it->second;
    };

    GeoIndex pointAdds = 0;
    for (const GeoKernelEdit* e : order) {
        if (e->kind == GeoKernelEditKind::eAddPoint || e->kind == GeoKernelEditKind::eAddPointFrom) {
            ++pointAdds;
        }
    }
    if (pointAdds > 0) {
        GeoOffset next = builder.appendPoints(pointAdds);
        for (const GeoKernelEdit* e : order) {
            if (e->kind == GeoKernelEditKind::eAddPoint || e->kind == GeoKernelEditKind::eAddPointFrom) {
                newPoints[e->handle] = next++;
            }
        }
        std::vector<GeoAttribute*> pointAttributes;
        for (const GeoKernelEdit* e : order) {
            if (e->kind == GeoKernelEditKind::eAddPoint) {
                builder.setPointPosition(newPoints[e->handle], e->position[0], e->position[1], e->position[2]);
            } else if (e->kind == GeoKernelEditKind::eAddPointFrom) {
                const GeoOffset source = resolvePoint(e->a);
                if (source < 0) {
                    continue;
                }
                if (pointAttributes.empty()) {
                    std::vector<std::string> names;
                    for (std::size_t k = 0; k < builder.detail().attributeCount(eGeoAttribClassPoint); ++k) {
                        names.push_back(builder.detail().attribute(eGeoAttribClassPoint, k).name());
                    }
                    for (const std::string& name : names) {
                        pointAttributes.push_back(builder.attributeForWriting(eGeoAttribClassPoint, name));
                    }
                }
                for (GeoAttribute* attribute : pointAttributes) {
                    attribute->copyElement(std::size_t(newPoints[e->handle]), std::size_t(source));
                }
            }
        }
    }

    struct NewPrimitive {
        std::int32_t handle;
        GeoPrimitiveTypeEnum type;
        std::vector<GeoOffset> points;
    };
    std::vector<NewPrimitive> primitives;
    std::unordered_map<std::int32_t, std::size_t> primitiveSlot;
    for (const GeoKernelEdit* e : order) {
        if (e->kind == GeoKernelEditKind::eAddPrimitive) {
            NewPrimitive p { e->handle, e->primitiveType, {} };
            for (std::int32_t number : e->points) {
                const GeoOffset point = resolvePoint(number);
                if (point >= 0) {
                    p.points.push_back(point);
                }
            }
            primitiveSlot[e->handle] = primitives.size();
            primitives.push_back(std::move(p));
        } else if (e->kind == GeoKernelEditKind::eAddVertex) {
            const auto it = primitiveSlot.find(e->a);
            if (it == primitiveSlot.end()) {
                if (e->a >= 0 && e->a < _input.primitiveCount()) {
                    throw KernelRunError("addvertex: primitive " + std::to_string(e->a)
                                         + " was not added by this run, and an existing primitive's vertices cannot grow");
                }
                continue;
            }
            const GeoOffset point = resolvePoint(e->b);
            if (point >= 0) {
                primitives[it->second].points.push_back(point);
            }
        }
    }
    for (std::size_t i = 0; i < primitives.size();) {
        std::size_t j = i;
        std::vector<std::int32_t> counts;
        std::vector<GeoOffset> points;
        while (j < primitives.size() && primitives[j].type == primitives[i].type) {
            counts.push_back(std::int32_t(primitives[j].points.size()));
            points.insert(points.end(), primitives[j].points.begin(), primitives[j].points.end());
            ++j;
        }
        const GeoOffset first = builder.appendPrimitives(primitives[i].type, counts, points);
        for (std::size_t k = i; k < j; ++k) {
            newPrimitives[primitives[k].handle] = first + GeoOffset(k - i);
        }
        i = j;
    }

    for (const GeoKernelEdit* e : order) {
        if (e->kind != GeoKernelEditKind::eSetPrimitiveVertex) {
            continue;
        }
        const GeoOffset primitive = resolvePrimitive(e->a);
        const GeoOffset point = resolvePoint(e->c);
        if (primitive < 0 || point < 0) {
            continue;
        }
        const GeoVertexRange range = builder.detail().primitiveVertices(primitive);
        if (e->b < 0 || e->b >= range.count) {
            continue;
        }
        builder.setVertexPoint(range.start + e->b, point);
    }

    for (const GeoKernelEdit* e : order) {
        if (e->kind != GeoKernelEditKind::eSetGroup || e->a < 0 || std::size_t(e->a) >= _groups.size()) {
            continue;
        }
        const GeoAttribClassEnum cls = _groups[std::size_t(e->a)].first;
        const GeoOffset offset = cls == eGeoAttribClassPrimitive ? resolvePrimitive(e->b) : resolvePoint(e->b);
        if (offset < 0) {
            continue;
        }
        GeoElementGroup* g = builder.groupForWriting(cls, _groups[std::size_t(e->a)].second);
        if (!g) {
            continue;
        }
        const bool value = e->c != 0;
        switch (GeoKernelGroupMode(e->d)) {
        case GeoKernelGroupMode::eSet:
            g->set(offset, value);
            break;
        case GeoKernelGroupMode::eAdd:
            if (value) {
                g->add(offset);
            }
            break;
        case GeoKernelGroupMode::eRemove:
            if (value) {
                g->remove(offset);
            }
            break;
        case GeoKernelGroupMode::eToggle:
            if (value) {
                g->set(offset, !g->contains(offset));
            }
            break;
        }
    }

    std::vector<GeoOffset> removedPrimitives;
    std::vector<GeoOffset> removedPrimitivesAndPoints;
    std::vector<GeoOffset> removedPoints;
    for (const GeoKernelEdit* e : order) {
        if (e->kind == GeoKernelEditKind::eRemovePrimitive) {
            const GeoOffset primitive = resolvePrimitive(e->a);
            if (primitive >= 0) {
                (e->b ? removedPrimitivesAndPoints : removedPrimitives).push_back(primitive);
            }
        } else if (e->kind == GeoKernelEditKind::eRemovePoint) {
            const GeoOffset point = resolvePoint(e->a);
            if (point >= 0) {
                removedPoints.push_back(point);
            }
        }
    }
    if (!removedPrimitives.empty()) {
        builder.deletePrimitives(removedPrimitives, false);
    }
    if (!removedPrimitivesAndPoints.empty()) {
        builder.deletePrimitives(removedPrimitivesAndPoints, true);
    }
    if (!removedPoints.empty()) {
        builder.deletePoints(removedPoints, eGeoPointDeleteDegenerate);
    }
}

NATRON_NAMESPACE_EXIT
