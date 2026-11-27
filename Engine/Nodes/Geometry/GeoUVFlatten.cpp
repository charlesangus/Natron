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

#include "GeoUVFlatten.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Geo/GeoPage.h"
#include "Engine/Geo/GeoUVIslands.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Geometry/GeoInterpolate.h"
#include "Engine/ParallelRenderArgs.h"

NATRON_NAMESPACE_ENTER

namespace {
std::string
trimmed(const std::string& s)
{
    std::size_t begin = 0, end = s.size();
    while ((begin < end) && std::isspace((unsigned char)s[begin])) {
        ++begin;
    }
    while ((end > begin) && std::isspace((unsigned char)s[end - 1])) {
        --end;
    }

    return s.substr(begin, end - begin);
}

bool
isAborted(const GeoUVFlattenParams& params)
{
    return params.aborted && params.aborted();
}

// The selection patterns treat a blank pattern as everything; a blank seam or constraint field
// means none.
bool
selectEdges(const GeoDetail& d,
            const std::string& pattern,
            std::set<GeoEdge>* edges,
            std::string* errorMessage)
{
    if (trimmed(pattern).empty()) {
        return true;
    }
    std::set<GeoEdge> selected;
    if (!geoSelectEdges(d, pattern, &selected, errorMessage)) {
        return false;
    }
    edges->insert(selected.begin(), selected.end());

    return true;
}

bool
selectVertices(const GeoDetail& d,
               const std::string& pattern,
               GeoElementMask* mask,
               std::string* errorMessage)
{
    if (trimmed(pattern).empty()) {
        mask->assign((std::size_t)d.vertexMap().offsetSize(), 0);

        return true;
    }

    return geoSelectElements(d, eGeoAttribClassVertex, pattern, mask, errorMessage);
}

bool
inMask(const GeoElementMask& mask,
       GeoOffset offset)
{
    return (offset >= 0) && ((std::size_t)offset < mask.size()) && mask[(std::size_t)offset];
}

// By vertex offset, the uv the vertex has now; false when there is no usable attribute.
bool
readExistingUV(const GeoDetail& d,
               const std::string& name,
               std::vector<GeoUV>* uv)
{
    const GeoAttribute* attribute = d.findAttribute(eGeoAttribClassVertex, name);
    bool onVertices = true;
    if (!attribute || !geoIsNumeric(*attribute) || (attribute->desc().tupleSize < 2)) {
        attribute = d.findAttribute(eGeoAttribClassPoint, name);
        onVertices = false;
    }
    if (!attribute || !geoIsNumeric(*attribute) || (attribute->desc().tupleSize < 2)) {
        return false;
    }
    uv->assign((std::size_t)d.vertexMap().offsetSize(), GeoUV { 0., 0. });
    std::vector<double> value((std::size_t)attribute->desc().tupleSize);
    d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        geoReadNumeric(*attribute, (std::size_t)(onVertices ? vertex : d.vertexPoint(vertex)), std::span<double>(value));
        (*uv)[(std::size_t)vertex] = { value[0], value[1] };
    });

    return true;
}

GeoAttribute*
uvAttributeForWriting(GeoDetailBuilder& detail,
                      const std::string& name)
{
    if (const GeoAttribute* existing = detail.detail().findAttribute(eGeoAttribClassVertex, name)) {
        if (geoIsNumeric(*existing) && (existing->desc().tupleSize >= 2)) {
            return detail.attributeForWriting(eGeoAttribClassVertex, name);
        }
    }
    const GeoAttribute* pointUV = detail.detail().findAttribute(eGeoAttribClassPoint, name);
    std::vector<std::pair<std::size_t, std::array<double, 3>>> seeded;
    if (pointUV && geoIsNumeric(*pointUV)) {
        const GeoDetail& d = detail.detail();
        const int components = std::min(3, (int)pointUV->desc().tupleSize);
        std::vector<double> value((std::size_t)pointUV->desc().tupleSize);
        d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
            geoReadNumeric(*pointUV, (std::size_t)d.vertexPoint(vertex), std::span<double>(value));
            std::array<double, 3> uvw = { 0., 0., 0. };
            for (int k = 0; k < components; ++k) {
                uvw[(std::size_t)k] = value[(std::size_t)k];
            }
            seeded.emplace_back((std::size_t)vertex, uvw);
        });
    }
    if (pointUV) {
        detail.removeAttribute(eGeoAttribClassPoint, name);
    }
    GeoAttribute& uv = detail.addAttribute(eGeoAttribClassVertex, GeoAttributeDesc { name, eGeoStorageFloat32, 3, eGeoTypeInfoNone, false });
    for (const std::pair<std::size_t, std::array<double, 3>>& s : seeded) {
        geoWriteNumeric(uv, s.first, std::span<const double>(s.second));
    }

    return &uv;
}

double
polygonArea(const std::vector<GeoUV>& ring)
{
    double area = 0.;
    for (std::size_t k = 0; k < ring.size(); ++k) {
        const GeoUV& a = ring[k];
        const GeoUV& b = ring[(k + 1) % ring.size()];
        area += 0.5 * (a[0] * b[1] - a[1] * b[0]);
    }

    return area;
}

struct FlattenStage {
    GeoUVIslands islands;
    std::vector<GeoUVFlattenChart> charts;
    std::vector<int> chartVertexOfVertex;
    std::vector<GeoUVFlattenChartConstraints> constraints;
    std::vector<GeoUVFlattenChartResult> results;
    // By chart: left with the uv it had, under Preserve Layout.
    std::vector<char> kept;
    // By chart: pinned in place, so the packing goes round it.
    std::vector<char> anchored;
    // By chart: oriented by its constraints, so the packing must not turn it.
    std::vector<char> oriented;
    std::vector<char> constrained;
};

bool
buildStage(const GeoDetail& d,
           const GeoElementMask& primitives,
           const std::set<GeoEdge>& seams,
           FlattenStage* stage,
           std::string* errorMessage)
{
    GeoUVIslandOptions options;
    options.connectivity = eGeoUVIslandConnectivityTopology;
    options.seams = seams;
    options.primitives = primitives;
    if (!geoComputeUVIslands(d, options, &stage->islands, errorMessage)) {
        return false;
    }
    geoBuildUVFlattenCharts(d, stage->islands, &stage->charts, &stage->chartVertexOfVertex);
    const std::size_t count = stage->charts.size();
    stage->constraints.assign(count, GeoUVFlattenChartConstraints());
    stage->results.assign(count, GeoUVFlattenChartResult());
    stage->kept.assign(count, 0);
    stage->anchored.assign(count, 0);
    stage->oriented.assign(count, 0);
    stage->constrained.assign(count, 0);

    return true;
}

int
chartOfVertex(const GeoDetail& d,
              const FlattenStage& stage,
              GeoOffset vertex)
{
    const GeoOffset primitive = d.vertexPrimitive(vertex);
    if ((primitive < 0) || ((std::size_t)primitive >= stage.islands.islandOfPrimitive.size())) {
        return -1;
    }

    return stage.islands.islandOfPrimitive[(std::size_t)primitive];
}

int
chartVertex(const FlattenStage& stage,
            GeoOffset vertex)
{
    return ((vertex >= 0) && ((std::size_t)vertex < stage.chartVertexOfVertex.size())) ? stage.chartVertexOfVertex[(std::size_t)vertex] : -1;
}

// By chart, the vertex edges of the mask as chart edges.
std::vector<std::vector<std::array<int, 2>>>
chartEdgesOf(const GeoDetail& d,
             const FlattenStage& stage,
             const GeoElementMask& mask)
{
    std::vector<std::vector<std::array<int, 2>>> edges(stage.charts.size());
    d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        if (!inMask(mask, vertex)) {
            return;
        }
        const int chart = chartOfVertex(d, stage, vertex);
        const int from = chartVertex(stage, vertex);
        const int to = chartVertex(stage, geoVertexEdgeEnd(d, vertex));
        if ((chart >= 0) && (from >= 0) && (to >= 0) && (from != to)) {
            edges[(std::size_t)chart].push_back({ from, to });
        }
    });

    return edges;
}

// The island already had uv worth keeping: some area in texture space.
bool
hasExistingLayout(const GeoDetail& d,
                  const std::vector<GeoOffset>& polygons,
                  const std::vector<GeoUV>& existing)
{
    double area = 0., extent = 0.;
    for (GeoOffset primitive : polygons) {
        const GeoVertexRange range = d.primitiveVertices(primitive);
        std::vector<GeoUV> ring;
        for (GeoOffset k = 0; k < range.count; ++k) {
            const GeoUV& uv = existing[(std::size_t)(range.start + k)];
            ring.push_back(uv);
            extent = std::max({ extent, std::fabs(uv[0]), std::fabs(uv[1]) });
        }
        area += std::fabs(polygonArea(ring));
    }

    return area > 1e-12 * std::max(1., extent * extent);
}

void
markExisting(const GeoDetail& d,
             const GeoUVFlattenParams& params,
             const std::vector<GeoUV>* existing,
             FlattenStage* stage)
{
    if (!existing) {
        return;
    }
    const bool boundaries = params.preserveIslandBoundaryShapes && (params.method == eGeoUVFlattenMethodSCP);
    for (std::size_t c = 0; c < stage->charts.size(); ++c) {
        if (!hasExistingLayout(d, stage->islands.islands[c], *existing)) {
            continue;
        }
        if (boundaries) {
            const GeoUVFlattenChart& chart = stage->charts[c];
            std::vector<GeoUV>& preserved = stage->constraints[c].preservedBoundary;
            preserved.resize(chart.vertexOffsets.size());
            for (std::size_t v = 0; v < chart.vertexOffsets.size(); ++v) {
                preserved[v] = (*existing)[(std::size_t)chart.vertexOffsets[v]];
            }
            stage->anchored[c] = params.preserveLayout;
        } else if (params.preserveLayout) {
            stage->kept[c] = 1;
        }
    }
}

void
addConstraints(const GeoDetail& d,
               const GeoUVFlattenParams& params,
               const std::vector<GeoElementMask>& alignmentMasks,
               const std::vector<GeoElementMask>& straighteningMasks,
               const GeoElementMask& rectifyMask,
               FlattenStage* stage,
               GeoUVFlattenReport* report)
{
    const std::size_t count = stage->charts.size();
    if (params.enablePins) {
        for (const GeoUVFlattenPinSpec& spec : params.pins) {
            const GeoIndexMap& primitives = d.primitiveMap();
            if ((spec.primitive < 0) || ((std::size_t)spec.primitive >= (std::size_t)primitives.size())) {
                ++report->ignoredPins;
                continue;
            }
            const GeoOffset primitive = primitives.offsetFromIndex(spec.primitive);
            const int chart = ((std::size_t)primitive < stage->islands.islandOfPrimitive.size()) ? stage->islands.islandOfPrimitive[(std::size_t)primitive] : -1;
            const GeoVertexRange range = d.primitiveVertices(primitive);
            if ((chart < 0) || (spec.vertex < 0) || (spec.vertex >= (int)range.count)) {
                ++report->ignoredPins;
                continue;
            }
            if (stage->kept[(std::size_t)chart]) {
                ++report->ignoredPins;
                continue;
            }
            const GeoOffset vertex = range.start + spec.vertex;
            GeoUVFlattenPin pin;
            pin.vertex = chartVertex(*stage, vertex);
            pin.uv = spec.uv;
            pin.referenceVertex = chartVertex(*stage, geoVertexEdgeEnd(d, vertex));
            pin.referenceUV = spec.referenceUV;
            if (pin.vertex < 0) {
                ++report->ignoredPins;
                continue;
            }
            stage->constraints[(std::size_t)chart].pins.push_back(pin);
            stage->constrained[(std::size_t)chart] = 1;
        }
    }
    if (params.enableAxisAlignment) {
        for (std::size_t a = 0; a < alignmentMasks.size(); ++a) {
            if (params.alignments[a].malformed) {
                ++report->ignoredAlignments;
                continue;
            }
            std::vector<std::set<int>> members(count);
            d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
                const int chart = inMask(alignmentMasks[a], vertex) ? chartOfVertex(d, *stage, vertex) : -1;
                const int v = chartVertex(*stage, vertex);
                if ((chart >= 0) && (v >= 0)) {
                    members[(std::size_t)chart].insert(v);
                }
            });
            for (std::size_t c = 0; c < count; ++c) {
                if (members[c].size() >= 2) {
                    GeoUVFlattenAlignment alignment;
                    alignment.vertices.assign(members[c].begin(), members[c].end());
                    alignment.axis = params.alignments[a].axis;
                    stage->constraints[c].alignments.push_back(alignment);
                    stage->constrained[c] = 1;
                    stage->oriented[c] = 1;
                }
            }
        }
    }
    if (params.enableLoopStraightening && (params.method == eGeoUVFlattenMethodABF)) {
        for (const GeoElementMask& mask : straighteningMasks) {
            const std::vector<std::vector<std::array<int, 2>>> edges = chartEdgesOf(d, *stage, mask);
            for (std::size_t c = 0; c < count; ++c) {
                if (!edges[c].empty()) {
                    stage->constraints[c].straightenings.push_back(edges[c]);
                    stage->constrained[c] = 1;
                }
            }
        }
    }
    const std::vector<std::vector<std::array<int, 2>>> rectify = chartEdgesOf(d, *stage, rectifyMask);
    for (std::size_t c = 0; c < count; ++c) {
        if (!rectify[c].empty()) {
            stage->constraints[c].rectify = rectify[c];
            stage->oriented[c] = 1;
        }
    }
} // addConstraints

bool
solveStage(const GeoUVFlattenParams& params,
           FlattenStage* stage,
           GeoUVFlattenReport* report)
{
    std::atomic<bool> stopped { false };
    std::vector<char> failed(stage->charts.size(), 0);
    geoParallelFor(stage->charts.size(), [&](std::size_t c) {
        if (stage->kept[c] || stopped.load(std::memory_order_relaxed)) {
            return;
        }
        if (isAborted(params)) {
            stopped.store(true, std::memory_order_relaxed);

            return;
        }
        std::string error;
        if (!geoFlattenUVChart(stage->charts[c], stage->constraints[c], params.method, &stage->results[c], &error, params.aborted)) {
            failed[c] = 1;
            if (stage->results[c].aborted) {
                stopped.store(true, std::memory_order_relaxed);
            }
        } }, 1);
    if (stopped.load() || isAborted(params)) {
        report->aborted = true;

        return false;
    }
    for (std::size_t c = 0; c < stage->charts.size(); ++c) {
        if (stage->kept[c]) {
            continue;
        }
        if (failed[c] || !stage->results[c].solved) {
            stage->results[c].solved = false;
            ++report->unsolved;
            continue;
        }
        report->degenerateFaces += stage->results[c].degenerateFaces;
        report->fellBack += stage->results[c].fellBack ? 1 : 0;
        stage->anchored[c] = stage->results[c].placed && (stage->anchored[c] || !stage->constraints[c].pins.empty());
    }

    return true;
}

// Packs the stage's free islands into the area round its kept and anchored ones.
bool
packStage(const GeoDetail& d,
          const GeoUVFlattenParams& params,
          const std::vector<GeoUV>* existing,
          FlattenStage* stage,
          GeoUVFlattenReport* report,
          std::string* errorMessage)
{
    std::vector<GeoUVPackIsland> islands;
    std::vector<int> chartOfIsland;
    bool anyFree = false;
    for (std::size_t c = 0; c < stage->charts.size(); ++c) {
        GeoUVPackIsland island;
        if (stage->kept[c]) {
            for (GeoOffset primitive : stage->islands.islands[c]) {
                const GeoVertexRange range = d.primitiveVertices(primitive);
                std::vector<GeoUV> ring;
                for (GeoOffset k = 0; k < range.count; ++k) {
                    ring.push_back((*existing)[(std::size_t)(range.start + k)]);
                }
                island.polygons.push_back(ring);
            }
            island.obstacle = true;
        } else if (stage->results[c].solved) {
            GeoUVFlattenChartResult& result = stage->results[c];
            island.obstacle = stage->anchored[c];
            if (!island.obstacle && !stage->oriented[c]) {
                const double angle = geoUVMinimumAreaRotation(result.uv);
                const double cs = std::cos(angle), sn = std::sin(angle);
                for (GeoUV& uv : result.uv) {
                    uv = { cs * uv[0] - sn * uv[1], sn * uv[0] + cs * uv[1] };
                }
            }
            for (const std::vector<int>& face : stage->charts[c].faces) {
                std::vector<GeoUV> ring;
                for (int v : face) {
                    ring.push_back(result.uv[(std::size_t)v]);
                }
                island.polygons.push_back(ring);
            }
        } else {
            continue;
        }
        anyFree = anyFree || !island.obstacle;
        islands.push_back(std::move(island));
        chartOfIsland.push_back((int)c);
    }
    if (!anyFree) {
        return true;
    }
    GeoUVPackParams pack;
    pack.rotationDivisions = 1;
    pack.targetType = eGeoUVPackTargetRectangles;
    GeoUVPackRect area;
    area.x0 = params.center[0] - 0.5 * params.size[0];
    area.y0 = params.center[1] - 0.5 * params.size[1];
    area.x1 = params.center[0] + 0.5 * params.size[0];
    area.y1 = params.center[1] + 0.5 * params.size[1];
    pack.rectangles = { area };
    pack.avoidObstacles = true;
    pack.aborted = params.aborted;
    GeoUVPackResult packed;
    if (!geoPackUVIslands(islands, pack, &packed, errorMessage)) {
        return false;
    }
    if (packed.aborted) {
        report->aborted = true;

        return false;
    }
    report->nonpacked += packed.nonpacked;
    for (std::size_t i = 0; i < islands.size(); ++i) {
        if (islands[i].obstacle) {
            continue;
        }
        for (GeoUV& uv : stage->results[(std::size_t)chartOfIsland[i]].uv) {
            uv = geoUVPackApply(packed.placements[i], uv);
        }
    }

    return true;
} // packStage

// The least-squares fit of from onto to by a uniform scale, a translation and a turn of a whole
// number of steps of a quarter turn, so the axes the island was aligned to stay axes.
GeoUVPackPlacement
fitInQuarterTurns(const std::vector<GeoUV>& from,
                  const std::vector<GeoUV>& to,
                  int step)
{
    static const std::array<double, 4> kTurns[4] = { { 1., 0., 0., 1. }, { 0., -1., 1., 0. }, { -1., 0., 0., -1. }, { 0., 1., -1., 0. } };
    const std::size_t n = std::min(from.size(), to.size());
    GeoUV cf = { 0., 0. }, ct = { 0., 0. };
    for (std::size_t i = 0; i < n; ++i) {
        for (int a = 0; a < 2; ++a) {
            cf[(std::size_t)a] += from[i][(std::size_t)a] / (double)n;
            ct[(std::size_t)a] += to[i][(std::size_t)a] / (double)n;
        }
    }
    double norm = 0.;
    for (std::size_t i = 0; i < n; ++i) {
        const double fx = from[i][0] - cf[0], fy = from[i][1] - cf[1];
        norm += fx * fx + fy * fy;
    }
    int best = 0;
    double bestDot = -std::numeric_limits<double>::infinity();
    for (int k = 0; k < 4; k += step) {
        const std::array<double, 4>& m = kTurns[k];
        double dot = 0.;
        for (std::size_t i = 0; i < n; ++i) {
            const double fx = from[i][0] - cf[0], fy = from[i][1] - cf[1];
            dot += (m[0] * fx + m[1] * fy) * (to[i][0] - ct[0]) + (m[2] * fx + m[3] * fy) * (to[i][1] - ct[1]);
        }
        if (dot > bestDot) {
            best = k;
            bestDot = dot;
        }
    }
    const double scale = ((norm > 0.) && (bestDot > 0.)) ? bestDot / norm : 1.;
    const std::array<double, 4>& m = kTurns[best];
    GeoUVPackPlacement placement;
    placement.m = { scale * m[0], scale * m[1], scale * m[2], scale * m[3] };
    placement.t = { ct[0] - (placement.m[0] * cf[0] + placement.m[1] * cf[1]), ct[1] - (placement.m[2] * cf[0] + placement.m[3] * cf[1]) };
    placement.packed = true;

    return placement;
}

void
fitOntoLayout(const GeoDetail& d,
              const FlattenStage& layout,
              FlattenStage* stage)
{
    for (std::size_t c = 0; c < stage->charts.size(); ++c) {
        if (stage->kept[c] || stage->anchored[c] || !stage->results[c].solved) {
            continue;
        }
        std::vector<GeoUV> from, to;
        for (GeoOffset primitive : stage->islands.islands[c]) {
            const GeoVertexRange range = d.primitiveVertices(primitive);
            for (GeoOffset k = 0; k < range.count; ++k) {
                const GeoOffset vertex = range.start + k;
                const int chart = chartOfVertex(d, layout, vertex);
                const int v = chartVertex(layout, vertex);
                const int own = chartVertex(*stage, vertex);
                if ((chart < 0) || (v < 0) || (own < 0) || layout.kept[(std::size_t)chart] || !layout.results[(std::size_t)chart].solved) {
                    continue;
                }
                from.push_back(stage->results[c].uv[(std::size_t)own]);
                to.push_back(layout.results[(std::size_t)chart].uv[(std::size_t)v]);
            }
        }
        if (from.size() < 2) {
            continue;
        }
        const GeoUVFlattenChartConstraints& constraints = stage->constraints[c];
        GeoUVPackPlacement placement;
        if (!constraints.alignments.empty()) {
            placement = fitInQuarterTurns(from, to, 2);
        } else if (!constraints.rectify.empty()) {
            placement = fitInQuarterTurns(from, to, 1);
        } else {
            placement = geoUVFitSimilarity(from, to);
        }
        for (GeoUV& uv : stage->results[c].uv) {
            uv = geoUVPackApply(placement, uv);
        }
    }
}
} // anonymous namespace

bool
geoUVFlattenParseUV(const std::string& text,
                    GeoUV* uv)
{
    std::string s = text;
    std::replace(s.begin(), s.end(), ',', ' ');
    std::istringstream in(s);
    double u = 0., v = 0.;
    if (!(in >> u >> v)) {
        return false;
    }
    std::string rest;
    if (in >> rest) {
        return false;
    }
    *uv = { u, v };

    return true;
}

bool
flattenGeoUVs(GeoDetailBuilder& detail,
              const GeoUVFlattenParams& params,
              GeoUVFlattenReport* report,
              std::string* errorMessage)
{
    GeoUVFlattenReport local;
    GeoUVFlattenReport* r = report ? report : &local;
    *r = GeoUVFlattenReport();
    std::string error;
    auto fail = [&](const std::string& message) {
        if (errorMessage) {
            *errorMessage = message;
        }

        return false;
    };
    if (trimmed(params.uvAttribute).empty()) {
        return fail("The UV Attribute is empty.");
    }
    const GeoDetail& d = detail.detail();
    GeoElementMask primitives;
    if (!geoSelectElements(d, eGeoAttribClassPrimitive, params.group, &primitives, &error)) {
        return fail(error);
    }
    std::vector<GeoUV> existingUV;
    const bool hasExisting = readExistingUV(d, params.uvAttribute, &existingUV);
    const std::vector<GeoUV>* existing = hasExisting ? &existingUV : nullptr;

    std::set<GeoEdge> seams, layoutSeams;
    if (!selectEdges(d, params.seams, &seams, &error)) {
        return fail(error);
    }
    if (!params.enableManualLayout && !selectEdges(d, params.layoutSeams, &layoutSeams, &error)) {
        return fail(error);
    }
    if (params.preserveSeams && hasExisting) {
        GeoUVIslandOptions options;
        options.connectivity = eGeoUVIslandConnectivityUV;
        options.uvAttribute = params.uvAttribute;
        options.primitives = primitives;
        GeoUVIslands current;
        if (!geoComputeUVIslands(d, options, &current, &error)) {
            return fail(error);
        }
        d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
            if (((std::size_t)vertex < current.vertexEdgeCut.size()) && current.vertexEdgeCut[(std::size_t)vertex]
                && (current.vertexEdgeTwin[(std::size_t)vertex] != GeoIndexMap::kInvalidOffset)) {
                const GeoEdge edge = geoVertexEdge(d, vertex);
                seams.insert(edge);
                layoutSeams.insert(edge);
            }
        });
    }
    GeoElementMask rectifyMask;
    if (!selectVertices(d, params.rectifyGroup, &rectifyMask, &error)) {
        return fail(error);
    }
    std::vector<GeoElementMask> alignmentMasks(params.alignments.size());
    for (std::size_t a = 0; a < params.alignments.size(); ++a) {
        if (!selectVertices(d, params.alignments[a].group, &alignmentMasks[a], &error)) {
            return fail(error);
        }
    }
    std::vector<GeoElementMask> straighteningMasks(params.straightenings.size());
    for (std::size_t s = 0; s < params.straightenings.size(); ++s) {
        if (!selectVertices(d, params.straightenings[s], &straighteningMasks[s], &error)) {
            return fail(error);
        }
    }

    FlattenStage stage;
    if (!buildStage(d, primitives, seams, &stage, &error)) {
        return fail(error);
    }
    r->degenerateFaces += (int)stage.islands.degenerate.size();
    markExisting(d, params, existing, &stage);
    addConstraints(d, params, alignmentMasks, straighteningMasks, rectifyMask, &stage, r);
    if (!solveStage(params, &stage, r)) {
        return fail(std::string());
    }
    if (params.enableManualLayout) {
        if (!packStage(d, params, existing, &stage, r, &error)) {
            return fail(error);
        }
    } else {
        FlattenStage layout;
        if (!buildStage(d, primitives, layoutSeams, &layout, &error)) {
            return fail(error);
        }
        markExisting(d, params, existing, &layout);
        GeoUVFlattenReport layoutReport;
        if (!solveStage(params, &layout, &layoutReport)) {
            r->aborted = true;

            return fail(std::string());
        }
        if (!packStage(d, params, existing, &layout, &layoutReport, &error)) {
            r->aborted = layoutReport.aborted;

            return fail(error);
        }
        r->nonpacked += layoutReport.nonpacked;
        fitOntoLayout(d, layout, &stage);
    }
    if (isAborted(params)) {
        r->aborted = true;

        return fail(std::string());
    }

    std::vector<std::pair<GeoOffset, GeoUV>> written;
    d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        const int chart = chartOfVertex(d, stage, vertex);
        const int v = chartVertex(stage, vertex);
        if ((chart < 0) || (v < 0) || stage.kept[(std::size_t)chart] || !stage.results[(std::size_t)chart].solved) {
            return;
        }
        written.emplace_back(vertex, stage.results[(std::size_t)chart].uv[(std::size_t)v]);
    });
    std::vector<GeoEdge> seamEdges;
    if (params.outputSeams) {
        d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
            if ((chartOfVertex(d, stage, vertex) >= 0) && stage.islands.vertexEdgeCut[(std::size_t)vertex]
                && (stage.islands.vertexEdgeTwin[(std::size_t)vertex] != GeoIndexMap::kInvalidOffset)) {
                seamEdges.push_back(geoVertexEdge(d, vertex));
            }
        });
    }
    std::vector<GeoOffset> constrainedPolygons;
    if (params.outputConstrainedIslands) {
        for (std::size_t c = 0; c < stage.charts.size(); ++c) {
            if (stage.constrained[c]) {
                constrainedPolygons.insert(constrainedPolygons.end(), stage.islands.islands[c].begin(), stage.islands.islands[c].end());
            }
        }
    }

    GeoAttribute* uv = uvAttributeForWriting(detail, params.uvAttribute);
    std::vector<double> value((std::size_t)uv->desc().tupleSize);
    for (const std::pair<GeoOffset, GeoUV>& w : written) {
        geoReadNumeric(*uv, (std::size_t)w.first, std::span<double>(value));
        value[0] = w.second[0];
        value[1] = w.second[1];
        geoWriteNumeric(*uv, (std::size_t)w.first, std::span<const double>(value));
    }
    if (params.outputSeams && !trimmed(params.outputSeamsGroup).empty()) {
        GeoEdgeGroup& group = detail.addEdgeGroup(trimmed(params.outputSeamsGroup));
        group.clear();
        group.add(std::span<const GeoEdge>(seamEdges));
    }
    if (params.outputConstrainedIslands && !trimmed(params.outputConstrainedIslandsGroup).empty()) {
        GeoElementGroup& group = detail.addGroup(eGeoAttribClassPrimitive, trimmed(params.outputConstrainedIslandsGroup));
        group.clear();
        group.add(std::span<const GeoOffset>(constrainedPolygons));
    }

    return true;
} // flattenGeoUVs

GeoUVFlatten::GeoUVFlatten(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoUVFlatten::~GeoUVFlatten()
{
}

const NodeSpec*
GeoUVFlatten::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoUVFlatten::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    auto createString = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      const std::string& defaultValue) {
        KnobStringPtr knob = createKnob<KnobString>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    auto createToggle = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      bool defaultValue) {
        KnobBoolPtr knob = createKnob<KnobBool>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    auto createDouble = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      std::initializer_list<double> defaults) {
        KnobDoublePtr knob = createKnob<KnobDouble>(label.toStdString(), (int)defaults.size());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        int i = 0;
        for (double value : defaults) {
            knob->setDefaultValue(value, i++);
        }
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };
    auto createTable = [this, &page](const QString& label,
                                     const char* name,
                                     const QString& toolTip,
                                     const std::vector<KnobRowTable::Column>& columns) {
        KnobRowTablePtr knob = createKnob<KnobRowTable>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->setColumns(columns);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };

    _group = createString(tr("Group"), kGeoUVFlattenParamGroup,
                          tr("The polygons to flatten, as a primitive group pattern. Empty flattens every polygon. "
                             "Other primitives, and polygons outside the group, keep their uv."),
                          std::string());
    _uvAttribute = createString(tr("UV Attribute"), kGeoUVFlattenParamUVAttribute,
                                tr("The vertex attribute the texture coordinates are written to. An existing one keeps "
                                   "its other components; a point attribute of the name is replaced by a vertex one."),
                                "uv");
    {
        KnobChoicePtr knob = createKnob<KnobChoice>(tr("Flattening Method").toStdString());
        knob->setName(kGeoUVFlattenParamFlatteningMethod);
        knob->setHintToolTip(tr("How the islands are flattened. Both keep the angles of the surface as well as they can; "
                                "they differ in how evenly they keep its areas and in the constraints they take.")
                                 .toStdString());
        knob->populateChoices({ ChoiceOption("scp", tr("Spectral (SCP)").toStdString(),
                                             tr("Fast and robust, with slightly more area distortion. Takes Preserve "
                                                "Island Boundary Shapes but not straightened loops.")
                                                 .toStdString()),
                                ChoiceOption("abf", tr("Angle Based (ABF)").toStdString(),
                                             tr("Slower and more even in area. Takes straightened loops. An island it "
                                                "cannot solve is flattened with SCP instead, with a warning.")
                                                 .toStdString()) });
        knob->setDefaultValue((int)eGeoUVFlattenMethodSCP);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);
        _flatteningMethod = knob;
    }
    _preserveSeams = createToggle(tr("Preserve Seams"), kGeoUVFlattenParamPreserveSeams,
                                  tr("Cuts the islands along the breaks in the existing uv as well as along the seams."),
                                  false);
    _preserveLayout = createToggle(tr("Preserve Layout"), kGeoUVFlattenParamPreserveLayout,
                                   tr("Leaves the islands that already have uv where they are, and packs the new islands "
                                      "round them."),
                                   false);
    _preserveIslandBoundaryShapes = createToggle(tr("Preserve Island Boundary Shapes"), kGeoUVFlattenParamPreserveIslandBoundaryShapes,
                                                 tr("Keeps the boundary of each island that already has uv and flattens "
                                                    "its inside again. With Preserve Layout the island stays where it is; "
                                                    "without it, it is packed. Spectral (SCP) only."),
                                                 false);
    _seams = createString(tr("Seams"), kGeoUVFlattenParamSeams,
                          tr("The edges the islands are cut along, as an edge pattern: point paths such as p3-4-5, edge "
                             "group names and @attribute comparisons. Empty cuts no edge. With Enable Manual Layout off "
                             "these are the final seams, applied after packing on top of the Layout Seams."),
                          std::string());
    _rectifyGroup = createString(tr("Rectify Group"), kGeoUVFlattenParamRectifyGroup,
                                 tr("A region of quads made rectangles in uv, as a vertex group of vertex edges: each "
                                    "vertex names the edge to the next vertex of its polygon. The horizontal edges are "
                                    "enough. Empty rectifies nothing."),
                                 std::string());
    _enablePins = createToggle(tr("Enable Pins"), kGeoUVFlattenParamEnablePins,
                               tr("Turns every pin on or off."), true);
    _pins = createTable(tr("Pins"), kGeoUVFlattenParamPins,
                        tr("Vertices pinned to uv positions. Primitive is the polygon's number, Vertex the vertex's place "
                           "in it, UV where it goes, as \"u v\". Reference UV is the absolute uv the polygon's next vertex "
                           "goes to, turning and scaling the island; it is only used when the pin is the only one of its "
                           "island, and empty (or equal to UV) only moves the island, keeping its flattened size and "
                           "orientation. Two rows on one vertex count as one pin, the last winning. An island with two or "
                           "more pins, or one, is left where its pins put it."),
                        { KnobRowTable::Column { "Primitive", "primitive", "0" },
                          KnobRowTable::Column { "Vertex", "vertex", "0" },
                          KnobRowTable::Column { "UV", "uv", "0 0" },
                          KnobRowTable::Column { "Reference UV", "referenceUV", "" } });
    _enableAxisAlignment = createToggle(tr("Enable Axis Alignment"), kGeoUVFlattenParamEnableAxisAlignment,
                                        tr("Turns every alignment on or off."), true);
    _alignments = createTable(tr("Alignments"), kGeoUVFlattenParamAlignments,
                              tr("Vertex groups lined up in uv. Group is a vertex group pattern; one of the vertices "
                                 "wired to a point is enough. Align Parallel To is U, to give them one v, or V, to give "
                                 "them one u, in either case; a row with any other value is ignored, with a warning."),
                              { KnobRowTable::Column { "Group", "group", "" },
                                KnobRowTable::Column { "Align Parallel To", "alignParallelTo", "U" } });
    _enableLoopStraightening = createToggle(tr("Enable Loop Straightening"), kGeoUVFlattenParamEnableLoopStraightening,
                                            tr("Turns every straightened loop on or off. Angle Based (ABF) only."), true);
    _straightenings = createTable(tr("Straightenings"), kGeoUVFlattenParamStraightenings,
                                  tr("Sequences of edges put on one straight line, each a vertex group of vertex edges. "
                                     "Angle Based (ABF) only."),
                                  { KnobRowTable::Column { "Group", "group", "" } });
    _enableManualLayout = createToggle(tr("Enable Manual Layout"), kGeoUVFlattenParamEnableManualLayout,
                                       tr("On, the Seams and the flattening constraints are applied before packing and "
                                          "the Layout Seams are ignored. Off, the islands of the Layout Seams are flattened "
                                          "and packed first, and the islands of the Seams are then flattened with their "
                                          "constraints and fitted onto that layout, so cutting or joining an edge does not "
                                          "move the other islands."),
                                       true);
    _layoutSeams = createString(tr("Layout Seams"), kGeoUVFlattenParamLayoutSeams,
                                tr("The edges the islands are cut along before packing, as an edge pattern, when Enable "
                                   "Manual Layout is off. Empty cuts no edge."),
                                std::string());
    _outputSeams = createToggle(tr("Output Seams"), kGeoUVFlattenParamOutputSeams,
                                tr("Writes the edges the islands are cut along as an edge group."), false);
    _outputSeamsGroup = createString(tr("Output Seams Group"), kGeoUVFlattenParamOutputSeamsGroup,
                                     tr("The name of the seam edge group."), "seams");
    _outputConstrainedIslands = createToggle(tr("Output Constrained Islands"), kGeoUVFlattenParamOutputConstrainedIslands,
                                             tr("Writes the polygons of every island with a pin, alignment or straightened "
                                                "loop as a primitive group."),
                                             false);
    _outputConstrainedIslandsGroup = createString(tr("Output Constrained Islands Group"), kGeoUVFlattenParamOutputConstrainedIslandsGroup,
                                                  tr("The name of the constrained islands' primitive group."), "constrainedprims");
    _center = createDouble(tr("Center"), kGeoUVFlattenParamCenter,
                           tr("The centre of the packing area, in uv."), { 0.5, 0.5 });
    _size = createDouble(tr("Size"), kGeoUVFlattenParamSize,
                         tr("The width and height of the packing area, in uv."), { 1., 1. });
    updateEnabled();
} // GeoUVFlatten::initializeKnobs

void
GeoUVFlatten::updateEnabled()
{
    const KnobChoicePtr method = _flatteningMethod.lock();
    const bool abf = method && (method->getValue() == (int)eGeoUVFlattenMethodABF);
    auto on = [](const KnobBoolWPtr& weak) {
        const KnobBoolPtr knob = weak.lock();

        return knob && knob->getValue();
    };
    auto enable = [](const KnobIPtr& knob,
                     bool enabled) {
        if (knob) {
            knob->setAllDimensionsEnabled(enabled);
        }
    };
    enable(_preserveIslandBoundaryShapes.lock(), !abf);
    enable(_pins.lock(), on(_enablePins));
    enable(_alignments.lock(), on(_enableAxisAlignment));
    enable(_enableLoopStraightening.lock(), abf);
    enable(_straightenings.lock(), abf && on(_enableLoopStraightening));
    enable(_layoutSeams.lock(), !on(_enableManualLayout));
    enable(_outputSeamsGroup.lock(), on(_outputSeams));
    enable(_outputConstrainedIslandsGroup.lock(), on(_outputConstrainedIslands));
} // GeoUVFlatten::updateEnabled

void
GeoUVFlatten::onKnobsLoaded()
{
    updateEnabled();
}

bool
GeoUVFlatten::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    for (const KnobIPtr& knob : { KnobIPtr(_flatteningMethod.lock()), KnobIPtr(_enablePins.lock()), KnobIPtr(_enableAxisAlignment.lock()),
                                  KnobIPtr(_enableLoopStraightening.lock()), KnobIPtr(_enableManualLayout.lock()),
                                  KnobIPtr(_outputSeams.lock()), KnobIPtr(_outputConstrainedIslands.lock()) }) {
        if (knob && (k == knob.get())) {
            updateEnabled();

            return true;
        }
    }

    return false;
}

StatusEnum
GeoUVFlatten::renderGeometry(const GeoRenderActionArgs& args,
                             GeoDetailConstPtr* outputDetail,
                             std::string* errorMessage)
{
    GeoUVFlattenParams params = readParams(args.time, args.view);
    const ParallelRenderArgsPtr frameArgs = getFrameArgs();
    const AbortableRenderInfoPtr render = frameArgs ? frameArgs->abortInfo.lock() : AbortableRenderInfoPtr();
    if (render) {
        params.aborted = [render]() {
            return render->isAborted();
        };
    }
    GeoUVFlattenReport report;
    const StatusEnum status = renderGeometryFromInput(args, 0, [&params, &report](GeoDetailBuilder& detail, std::string* error) {
        std::string message;
        if (!flattenGeoUVs(detail, params, &report, &message)) {
            if (error) {
                *error = message;
            }

            return eStatusFailed;
        }

        return eStatusOK; }, outputDetail, errorMessage);
    if (status != eStatusOK) {
        return status;
    }
    std::vector<std::string> warnings;
    if (report.degenerateFaces > 0) {
        warnings.push_back(tr("%1 polygon(s) with no area were skipped.").arg(report.degenerateFaces).toStdString());
    }
    if (report.fellBack > 0) {
        warnings.push_back(tr("%1 island(s) could not be solved Angle Based and were flattened with Spectral (SCP) instead.")
                               .arg(report.fellBack)
                               .toStdString());
    }
    if (report.unsolved > 0) {
        warnings.push_back(tr("%1 island(s) could not be flattened and kept their uv.").arg(report.unsolved).toStdString());
    }
    if (report.nonpacked > 0) {
        warnings.push_back(tr("%1 island(s) did not fit in the packing area.").arg(report.nonpacked).toStdString());
    }
    if (report.ignoredPins > 0) {
        warnings.push_back(tr("%1 pin(s) name no vertex of a polygon in the group, or one of an island Preserve Layout keeps, "
                              "and were ignored.")
                               .arg(report.ignoredPins)
                               .toStdString());
    }
    if (report.ignoredAlignments > 0) {
        warnings.push_back(tr("%1 alignment(s) have an Align Parallel To other than U or V and were ignored.")
                               .arg(report.ignoredAlignments)
                               .toStdString());
    }
    if (warnings.empty()) {
        clearPersistentMessage(false);
    } else {
        std::string text;
        for (const std::string& w : warnings) {
            text += (text.empty() ? "" : "\n") + w;
        }
        setPersistentMessage(eMessageTypeWarning, text);
    }

    return eStatusOK;
} // GeoUVFlatten::renderGeometry

GeoUVFlattenParams
GeoUVFlatten::readParams(double time,
                         ViewSpec view) const
{
    GeoUVFlattenParams p;
    auto text = [time, view](const KnobStringWPtr& weak,
                             const std::string& fallback) {
        const KnobStringPtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    auto toggle = [time, view](const KnobBoolWPtr& weak,
                               bool fallback) {
        const KnobBoolPtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    auto rows = [](const KnobRowTableWPtr& weak) {
        std::list<std::vector<std::string>> table;
        if (const KnobRowTablePtr knob = weak.lock()) {
            try {
                knob->getTable(&table);
            } catch (const std::exception&) {
                table.clear();
            }
        }

        return table;
    };
    auto integer = [](const std::string& s,
                      long* out) {
        const std::string t = trimmed(s);
        char* end = nullptr;
        const long value = std::strtol(t.c_str(), &end, 10);
        if (t.empty() || !end || (*end != '\0')) {
            return false;
        }
        *out = value;

        return true;
    };
    p.group = text(_group, p.group);
    p.uvAttribute = trimmed(text(_uvAttribute, p.uvAttribute));
    if (const KnobChoicePtr method = _flatteningMethod.lock()) {
        p.method = (method->getValueAtTime(time, 0, view) == (int)eGeoUVFlattenMethodABF) ? eGeoUVFlattenMethodABF : eGeoUVFlattenMethodSCP;
    }
    p.preserveSeams = toggle(_preserveSeams, p.preserveSeams);
    p.preserveLayout = toggle(_preserveLayout, p.preserveLayout);
    p.preserveIslandBoundaryShapes = toggle(_preserveIslandBoundaryShapes, p.preserveIslandBoundaryShapes);
    p.seams = text(_seams, p.seams);
    p.rectifyGroup = text(_rectifyGroup, p.rectifyGroup);
    p.enablePins = toggle(_enablePins, p.enablePins);
    for (const std::vector<std::string>& row : rows(_pins)) {
        GeoUVFlattenPinSpec pin;
        long primitive = 0, vertex = 0;
        if ((row.size() < 3) || !integer(row[0], &primitive) || !integer(row[1], &vertex) || !geoUVFlattenParseUV(row[2], &pin.uv)) {
            pin.primitive = -1;
        } else {
            pin.primitive = (GeoIndex)primitive;
            pin.vertex = (int)vertex;
        }
        if ((row.size() < 4) || !geoUVFlattenParseUV(row[3], &pin.referenceUV)) {
            pin.referenceUV = pin.uv;
        }
        p.pins.push_back(pin);
    }
    p.enableAxisAlignment = toggle(_enableAxisAlignment, p.enableAxisAlignment);
    for (const std::vector<std::string>& row : rows(_alignments)) {
        if (row.empty() || trimmed(row[0]).empty()) {
            continue;
        }
        GeoUVFlattenAlignmentSpec alignment;
        alignment.group = row[0];
        const std::string axis = (row.size() > 1) ? trimmed(row[1]) : std::string();
        if ((axis == "V") || (axis == "v")) {
            alignment.axis = eGeoUVFlattenAxisV;
        } else if ((axis == "U") || (axis == "u")) {
            alignment.axis = eGeoUVFlattenAxisU;
        } else {
            alignment.malformed = true;
        }
        p.alignments.push_back(alignment);
    }
    p.enableLoopStraightening = toggle(_enableLoopStraightening, p.enableLoopStraightening);
    for (const std::vector<std::string>& row : rows(_straightenings)) {
        if (!row.empty() && !trimmed(row[0]).empty()) {
            p.straightenings.push_back(row[0]);
        }
    }
    p.enableManualLayout = toggle(_enableManualLayout, p.enableManualLayout);
    p.layoutSeams = text(_layoutSeams, p.layoutSeams);
    p.outputSeams = toggle(_outputSeams, p.outputSeams);
    p.outputSeamsGroup = text(_outputSeamsGroup, p.outputSeamsGroup);
    p.outputConstrainedIslands = toggle(_outputConstrainedIslands, p.outputConstrainedIslands);
    p.outputConstrainedIslandsGroup = text(_outputConstrainedIslandsGroup, p.outputConstrainedIslandsGroup);
    for (int i = 0; i < 2; ++i) {
        if (const KnobDoublePtr center = _center.lock()) {
            p.center[(std::size_t)i] = center->getValueAtTime(time, i, view);
        }
        if (const KnobDoublePtr size = _size.lock()) {
            p.size[(std::size_t)i] = size->getValueAtTime(time, i, view);
        }
    }

    return p;
} // GeoUVFlatten::readParams

NATRON_NAMESPACE_EXIT
