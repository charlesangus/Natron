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

#include "GeoDisplay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPointF>
#include <QString>

#include "Engine/Geo/GeoPacked.h"
#include "Engine/Geo/GeoVDB.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoSdfFileFormat.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

const char* const kRootName = "NatronGeometryDisplay";
const char* const kPointMarkersName = "PointMarkers";
const char* const kNormalsName = "Normals";
const char* const kHighlightPointsName = "HighlightPoints";
const char* const kHighlightEdgesName = "HighlightEdges";
const char* const kPackedBoundsName = "PackedBounds";
const char* const kSelectedPointsName = "SelectedPoints";
const char* const kSelectedPrimitivesName = "SelectedPrimitives";
const GfVec3f kSelectionColour(1.f, 0.8f, 0.1f);

// The layer's content changes in place, so one hash stands for it whatever it holds.
const U64 kGuideLayerHash = 0x4e47656f44697370ULL;

const int kMaxLabelsPerPaint = 1000;
// The most anchors a paint projects, whatever is in view: the cells culled first leave only those
// near the view to scan, but a view inside a dense cloud still has its cost bounded.
const std::size_t kMaxLabelsScanned = 500000;
// Per axis, at most; the cells' own culling stays a few thousand corner projections a paint.
const int kMaxLabelGridDivisions = 16;
// About this many anchors a cell before the grid divides further.
const std::size_t kLabelsPerCell = 256;

struct LabelAnchor {
    float position[3];
    GeoIndex number;
};

// The anchors of a kind bucketed by a coarse grid over them, each cell's anchors contiguous, with
// the cell's tight bounds, so a paint skips the cells out of view instead of projecting them all.
struct LabelCell {
    float min[3];
    float max[3];
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct LabelSet {
    std::vector<LabelAnchor> anchors;
    std::vector<LabelCell> cells;

    bool empty() const
    {
        return anchors.empty();
    }
};

// Which of the guides drawn from the viewed details' own data a collect pass builds.
enum GuideKindFlags {
    eGuideMarkers = 1,
    eGuideNormals = 2,
    eGuideHighlight = 4,
    eGuidePacked = 8,
};

struct Guides {
    VtVec3fArray markers;
    VtVec3fArray normals;
    VtVec3fArray highlightPoints;
    VtVec3fArray highlightEdges;
    VtVec3fArray packedBounds;
    VtVec3fArray selectedPoints;
    VtVec3fArray selectedPrimitives;
};

GfVec3f
toVec(const std::array<double, 3>& p)
{
    return GfVec3f(float(p[0]), float(p[1]), float(p[2]));
}

void
addSegment(VtVec3fArray* lines,
           const GfVec3f& a,
           const GfVec3f& b)
{
    lines->push_back(a);
    lines->push_back(b);
}

void
addBox(VtVec3fArray* lines,
       const GeoBounds& b)
{
    const auto corner = [&b](int i) {
        return GfVec3f(float((i & 1) ? b.max[0] : b.min[0]), float((i & 2) ? b.max[1] : b.min[1]), float((i & 4) ? b.max[2] : b.min[2]));
    };
    for (int i = 0; i < 8; ++i) {
        for (int axis = 1; axis < 8; axis <<= 1) {
            if (!(i & axis)) {
                addSegment(lines, corner(i), corner(i | axis));
            }
        }
    }
}

double
readComponent(const GeoAttribute& attribute,
              GeoOffset offset,
              int component)
{
    return (attribute.storage() == eGeoStorageFloat64) ? attribute.get<double>(std::size_t(offset), component)
                                                       : double(attribute.get<float>(std::size_t(offset), component));
}

bool
hasPackedUSD(const GeoDetail& detail)
{
    return detail.findAttribute(eGeoAttribClassPrimitive, kGeoPackedUSDPathAttr) != nullptr;
}

void
collectGuides(const GeoDetail& detail,
              const GeoDisplayOptions& options,
              int kinds,
              Guides* guides)
{
    const GeoIndexMap& points = detail.pointMap();
    if (kinds & eGuideMarkers) {
        points.forEachLive([&](GeoIndex, GeoOffset offset) {
            guides->markers.push_back(toVec(detail.pointPosition(offset)));
        });
    }
    if ((kinds & eGuideNormals) && options.normals) {
        const auto usable = [](const GeoAttribute* n) {
            return n && (n->tupleSize() == 3) && ((n->storage() == eGeoStorageFloat32) || (n->storage() == eGeoStorageFloat64));
        };
        const double scale = options.normalScale;
        const auto addNormal = [&](const GeoAttribute& n,
                                   GeoOffset element,
                                   GeoOffset point) {
            const std::array<double, 3> p = detail.pointPosition(point);
            const std::array<double, 3> tip { p[0] + readComponent(n, element, 0) * scale,
                                              p[1] + readComponent(n, element, 1) * scale,
                                              p[2] + readComponent(n, element, 2) * scale };
            addSegment(&guides->normals, toVec(p), toVec(tip));
        };
        const GeoAttribute* pointN = detail.findAttribute(eGeoAttribClassPoint, "N");
        const GeoAttribute* vertexN = detail.findAttribute(eGeoAttribClassVertex, "N");
        if (usable(pointN)) {
            points.forEachLive([&](GeoIndex, GeoOffset offset) { addNormal(*pointN, offset, offset); });
        } else if (usable(vertexN)) {
            // Each corner's normal from its point, so a hard edge shows one line per face.
            detail.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) { addNormal(*vertexN, vertex, detail.vertexPoint(vertex)); });
        }
    }
    const GeoIndexMap& primitives = detail.primitiveMap();
    if ((kinds & eGuideHighlight) && !options.highlightGroup.empty()) {
        if (const GeoElementGroup* group = detail.findGroup(eGeoAttribClassPoint, options.highlightGroup)) {
            points.forEachLive([&](GeoIndex, GeoOffset offset) {
                if (group->contains(offset)) {
                    guides->highlightPoints.push_back(toVec(detail.pointPosition(offset)));
                }
            });
        } else if (const GeoElementGroup* group = detail.findGroup(eGeoAttribClassPrimitive, options.highlightGroup)) {
            primitives.forEachLive([&](GeoIndex, GeoOffset primitive) {
                if (!group->contains(primitive)) {
                    return;
                }
                const GeoPrimitiveTypeEnum type = detail.primitiveType(primitive);
                if (geoPrimitiveIsPacked(type)) {
                    addBox(&guides->highlightEdges, geoPackedWorldBounds(detail, primitive));

                    return;
                }
                if (type == eGeoPrimitiveVDB) {
                    const GeoBounds bounds = geoVDBBounds(detail, primitive);
                    if (!(bounds == GeoBounds())) {
                        addBox(&guides->highlightEdges, bounds);
                    }

                    return;
                }
                const GeoVertexRange vertices = detail.primitiveVertices(primitive);
                const GeoOffset edges = (type == eGeoPrimitivePolygon) ? vertices.count : vertices.count - 1;
                for (GeoOffset i = 0; i < edges; ++i) {
                    const GeoOffset a = detail.vertexPoint(vertices.start + i);
                    const GeoOffset b = detail.vertexPoint(vertices.start + ((i + 1) % vertices.count));
                    addSegment(&guides->highlightEdges, toVec(detail.pointPosition(a)), toVec(detail.pointPosition(b)));
                }
            });
        }
    }
    // A packed USD primitive's source is not in the viewer's stage, so its bounds stand in for it.
    // Every one carries its source path, so a detail without that attribute has none to scan for.
    if ((kinds & eGuidePacked) && hasPackedUSD(detail)) {
        primitives.forEachLive([&](GeoIndex, GeoOffset primitive) {
            if (detail.primitiveType(primitive) == eGeoPrimitivePackedUSD) {
                addBox(&guides->packedBounds, geoPackedWorldBounds(detail, primitive));
            }
        });
    }
}

LabelSet
bucketLabels(std::vector<LabelAnchor> anchors)
{
    LabelSet set;
    anchors.erase(std::remove_if(anchors.begin(), anchors.end(), [](const LabelAnchor& a) {
                      return !std::isfinite(a.position[0]) || !std::isfinite(a.position[1]) || !std::isfinite(a.position[2]);
                  }),
                  anchors.end());
    if (anchors.empty()) {
        return set;
    }
    float lo[3], hi[3];
    for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = hi[axis] = anchors.front().position[axis];
    }
    for (const LabelAnchor& a : anchors) {
        for (int axis = 0; axis < 3; ++axis) {
            lo[axis] = std::min(lo[axis], a.position[axis]);
            hi[axis] = std::max(hi[axis], a.position[axis]);
        }
    }
    const double wanted = std::cbrt(double(anchors.size()) / double(kLabelsPerCell));
    const int divisions = std::clamp(int(std::ceil(wanted)), 1, kMaxLabelGridDivisions);
    const auto cellOf = [&](const LabelAnchor& a) {
        std::size_t cell = 0;
        for (int axis = 2; axis >= 0; --axis) {
            const float extent = hi[axis] - lo[axis];
            const int i = (extent > 0.f) ? std::min(divisions - 1, int((a.position[axis] - lo[axis]) / extent * float(divisions))) : 0;
            cell = cell * std::size_t(divisions) + std::size_t(std::max(0, i));
        }

        return cell;
    };
    const std::size_t cellCount = std::size_t(divisions) * std::size_t(divisions) * std::size_t(divisions);
    std::vector<std::size_t> starts(cellCount + 1, 0);
    for (const LabelAnchor& a : anchors) {
        ++starts[cellOf(a) + 1];
    }
    for (std::size_t cell = 0; cell < cellCount; ++cell) {
        starts[cell + 1] += starts[cell];
    }
    set.anchors.resize(anchors.size());
    std::vector<std::size_t> next(starts.begin(), starts.end() - 1);
    for (const LabelAnchor& a : anchors) {
        set.anchors[next[cellOf(a)]++] = a;
    }
    for (std::size_t cell = 0; cell < cellCount; ++cell) {
        if (starts[cell] == starts[cell + 1]) {
            continue;
        }
        LabelCell c;
        c.begin = starts[cell];
        c.end = starts[cell + 1];
        for (int axis = 0; axis < 3; ++axis) {
            c.min[axis] = c.max[axis] = set.anchors[c.begin].position[axis];
        }
        for (std::size_t i = c.begin; i < c.end; ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                c.min[axis] = std::min(c.min[axis], set.anchors[i].position[axis]);
                c.max[axis] = std::max(c.max[axis], set.anchors[i].position[axis]);
            }
        }
        set.cells.push_back(c);
    }

    return set;
}

// Whether any of the cell's box may land in view: false only when all eight corners are outside
// one clip plane.
bool
cellInView(const LabelCell& cell,
           const double m[16])
{
    int outside = 0x3f;
    for (int corner = 0; (corner < 8) && outside; ++corner) {
        const double p[3] = { (corner & 1) ? cell.max[0] : cell.min[0], (corner & 2) ? cell.max[1] : cell.min[1], (corner & 4) ? cell.max[2] : cell.min[2] };
        double clip[4];
        for (int j = 0; j < 4; ++j) {
            clip[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
        }
        int code = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (clip[axis] < -clip[3]) {
                code |= 1 << (2 * axis);
            }
            if (clip[axis] > clip[3]) {
                code |= 2 << (2 * axis);
            }
        }
        outside &= code;
    }

    return outside == 0;
}

void
collectLabels(const GeoDetail& detail,
              const GeoDisplayOptions& options,
              std::vector<LabelAnchor>* pointLabels,
              std::vector<LabelAnchor>* primitiveLabels)
{
    const auto anchor = [](const std::array<double, 3>& p,
                           GeoIndex number) {
        LabelAnchor a;
        a.position[0] = float(p[0]);
        a.position[1] = float(p[1]);
        a.position[2] = float(p[2]);
        a.number = number;

        return a;
    };
    if (options.pointNumbers) {
        detail.pointMap().forEachLive([&](GeoIndex index, GeoOffset offset) {
            pointLabels->push_back(anchor(detail.pointPosition(offset), index));
        });
    }
    if (options.primitiveNumbers) {
        detail.primitiveMap().forEachLive([&](GeoIndex index, GeoOffset primitive) {
            const GeoVertexRange vertices = detail.primitiveVertices(primitive);
            if (vertices.count <= 0) {
                return;
            }
            std::array<double, 3> sum { 0., 0., 0. };
            for (GeoOffset i = 0; i < vertices.count; ++i) {
                const std::array<double, 3> p = detail.pointPosition(detail.vertexPoint(vertices.start + i));
                sum[0] += p[0];
                sum[1] += p[1];
                sum[2] += p[2];
            }
            const double n = double(vertices.count);
            primitiveLabels->push_back(anchor({ sum[0] / n, sum[1] / n, sum[2] / n }, index));
        });
    }
}

SdfPrimSpecHandle
ensurePrim(const SdfLayerHandle& layer,
           const SdfPrimSpecHandle& parent,
           const char* name,
           const char* typeName)
{
    const SdfPath path = parent->GetPath().AppendChild(TfToken(name));
    SdfPrimSpecHandle prim = layer->GetPrimAtPath(path);
    if (!prim) {
        prim = SdfPrimSpec::New(parent, name, SdfSpecifierDef, typeName);
    }

    return prim;
}

void
setAttribute(const SdfPrimSpecHandle& prim,
             const char* name,
             const SdfValueTypeName& type,
             const VtValue& value,
             SdfVariability variability = SdfVariabilityVarying,
             bool constantInterpolation = false)
{
    const SdfLayerHandle layer = prim->GetLayer();
    SdfAttributeSpecHandle attribute = layer->GetAttributeAtPath(prim->GetPath().AppendProperty(TfToken(name)));
    if (!attribute) {
        attribute = SdfAttributeSpec::New(prim, name, type, variability);
        if (!attribute) {
            return;
        }
        if (constantInterpolation) {
            attribute->SetInfo(TfToken("interpolation"), VtValue(TfToken("constant")));
        }
    }
    // An unchanged value is left alone, so it notifies nothing.
    if (attribute->GetDefaultValue() != value) {
        attribute->SetDefaultValue(value);
    }
}

void
authorGuide(const SdfLayerHandle& layer,
            const SdfPrimSpecHandle& root,
            const char* name,
            bool curves,
            const VtVec3fArray& points,
            float width,
            const GfVec3f& colour)
{
    const SdfPrimSpecHandle prim = ensurePrim(layer, root, name, curves ? "BasisCurves" : "Points");
    if (!prim) {
        return;
    }
    setAttribute(prim, "purpose", SdfValueTypeNames->Token, VtValue(TfToken("guide")), SdfVariabilityUniform);
    if (curves) {
        setAttribute(prim, "type", SdfValueTypeNames->Token, VtValue(TfToken("linear")), SdfVariabilityUniform);
        setAttribute(prim, "wrap", SdfValueTypeNames->Token, VtValue(TfToken("nonperiodic")), SdfVariabilityUniform);
        setAttribute(prim, "curveVertexCounts", SdfValueTypeNames->IntArray, VtValue(VtIntArray(points.size() / 2, 2)));
    }
    setAttribute(prim, "points", SdfValueTypeNames->Point3fArray, VtValue(points));
    setAttribute(prim, "widths", SdfValueTypeNames->FloatArray, VtValue(VtFloatArray(1, width)), SdfVariabilityVarying, true);
    setAttribute(prim, "primvars:displayColor", SdfValueTypeNames->Color3fArray, VtValue(VtVec3fArray(1, colour)), SdfVariabilityVarying, true);
}

void
removeGuide(const SdfPrimSpecHandle& root,
            const char* name)
{
    const SdfPrimSpecHandle prim = root->GetLayer()->GetPrimAtPath(root->GetPath().AppendChild(TfToken(name)));
    if (prim) {
        root->RemoveNameChild(prim);
    }
}

void
paintAnchors(QPainter& painter,
             const LabelSet& set,
             const double m[16],
             int width,
             int height,
             const QColor& colour,
             int* budget,
             std::size_t* scanned)
{
    if (set.empty() || (*budget <= 0)) {
        return;
    }
    painter.setPen(colour);
    // The cells in view take turns, an anchor each, so a budget spent before the scan ends still
    // spreads its labels over the view.
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (const LabelCell& cell : set.cells) {
        if (cellInView(cell, m)) {
            ranges.emplace_back(cell.begin, cell.end);
        }
    }
    while (!ranges.empty() && (*budget > 0) && (*scanned < kMaxLabelsScanned)) {
        std::size_t kept = 0;
        for (std::size_t r = 0; (r < ranges.size()) && (*budget > 0) && (*scanned < kMaxLabelsScanned); ++r) {
            std::pair<std::size_t, std::size_t>& range = ranges[r];
            const LabelAnchor& anchor = set.anchors[range.first++];
            ++*scanned;
            if (range.first < range.second) {
                ranges[kept++] = range;
            }
            const float* p = anchor.position;
            double clip[4];
            for (int j = 0; j < 4; ++j) {
                clip[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
            }
            if (clip[3] <= 0.) {
                continue;
            }
            const double x = clip[0] / clip[3];
            const double y = clip[1] / clip[3];
            const double z = clip[2] / clip[3];
            if ((x < -1.) || (x > 1.) || (y < -1.) || (y > 1.) || (z < -1.) || (z > 1.)) {
                continue;
            }
            const QPointF at((x + 1.) * 0.5 * width + 3., (1. - y) * 0.5 * height - 3.);
            painter.drawText(at, QString::number(anchor.number));
            --*budget;
        }
        if ((*budget > 0) && (*scanned < kMaxLabelsScanned)) {
            ranges.resize(kept);
        }
    }
}

// The picked points, which the point guide drawn over the markers holds in this order, and the
// bounds of the picked packed primitives.
void
collectSelectionGuides(const GeoDetail& detail,
                       const GeoComponentSelection& selection,
                       std::vector<GeoIndex>* selectedPoints,
                       Guides* guides)
{
    if (selection.type == eGeoComponentPoints) {
        for (GeoIndex index : selection.elements) {
            const GeoOffset offset = detail.pointMap().offsetFromIndex(index);
            if (offset >= 0) {
                guides->selectedPoints.push_back(toVec(detail.pointPosition(offset)));
                selectedPoints->push_back(index);
            }
        }
    } else if (selection.type == eGeoComponentPrimitives) {
        for (GeoIndex index : selection.elements) {
            const GeoOffset primitive = detail.primitiveMap().offsetFromIndex(index);
            if ((primitive >= 0) && geoPrimitiveIsPacked(detail.primitiveType(primitive))) {
                addBox(&guides->selectedPrimitives, geoPackedWorldBounds(detail, primitive));
            }
        }
    }
}

} // namespace

namespace {

// Counts the prims a stage resyncs while it lives.
class ResyncCounter
    : public TfWeakBase {
public:
    explicit ResyncCounter(const UsdStageRefPtr& stage)
    {
        if (stage) {
            _key = TfNotice::Register(TfCreateWeakPtr(this), &ResyncCounter::changed, UsdStageWeakPtr(stage));
        }
    }

    ~ResyncCounter()
    {
        TfNotice::Revoke(_key);
    }

    ResyncCounter(const ResyncCounter&) = delete;
    ResyncCounter& operator=(const ResyncCounter&) = delete;

    U64 resyncs = 0;

private:
    void changed(const UsdNotice::ObjectsChanged& notice)
    {
        resyncs += U64(notice.GetResyncedPaths().size());
    }

    TfNotice::Key _key;
};

} // namespace

struct GeoDisplay::Implementation {
    mutable std::mutex mutex;
    // Under mutex.
    GeoDisplayOptions options;
    std::vector<std::shared_ptr<const GeoUsdConvert>> converts;
    LabelSet pointLabels;
    LabelSet primitiveLabels;
    GeoComponentEnum componentMode = eGeoComponentObjects;
    GeoComponentSelection componentSelection;
    U64 componentVersion = 0;
    // The points the selected point guide was last authored with, in its order.
    std::vector<GeoIndex> selectedPoints;

    std::atomic<bool> staged { false };
    std::atomic<U64> refreshes { 0 };
    std::atomic<U64> refreshResyncs { 0 };
    std::atomic<int> refreshFrame { 0 };
    std::atomic<bool> hasRefreshFrame { false };

    // The syncing thread's. converts holds the details authoredDetails names alive, so a pointer
    // equal to one of them is that detail.
    SdfLayerRefPtr guideLayer;
    ScenePayload stagedSource;
    bool authored = false;
    std::vector<const GeoDetail*> authoredDetails;
    GeoDisplayOptions authoredOptions;
    bool authoredMarkers = false;
    U64 authoredComponentVersion = 0;
    // The guides' widths, sized by the details' bounds, which only a drawn guide needs.
    bool widthsValid = false;
    float markerWidth = 0.f;
    float lineWidth = 0.f;
};

GeoDisplay::GeoDisplay()
    : _imp(new Implementation)
{
}

GeoDisplay::~GeoDisplay()
{
}

bool
GeoDisplay::setOptions(const GeoDisplayOptions& options)
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    if (_imp->options == options) {
        return false;
    }
    _imp->options = options;

    return true;
}

GeoDisplayOptions
GeoDisplay::getOptions() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return _imp->options;
}

ScenePayload
GeoDisplay::stagedPayload(const ScenePayload& payload)
{
    if (GeoViewerLayer::findLayers(payload).empty()) {
        if (_imp->staged.exchange(false)) {
            _imp->stagedSource = ScenePayload();
            _imp->authored = false;
            _imp->authoredDetails.clear();
            _imp->widthsValid = false;
            std::lock_guard<std::mutex> lock(_imp->mutex);
            _imp->converts.clear();
            _imp->selectedPoints.clear();
            _imp->pointLabels = LabelSet();
            _imp->primitiveLabels = LabelSet();
        }

        return payload;
    }
    if (!_imp->guideLayer) {
        _imp->guideLayer = SdfLayer::CreateAnonymous("natron-geometry-display");
    }
    _imp->stagedSource = payload;
    _imp->staged = true;

    return SceneOps::appendLayer(payload, _imp->guideLayer, kGuideLayerHash);
}

namespace {

// The detail the viewer geometry layer @p identifier would be refreshed to, and whether it differs
// from the one its conversion holds, as GeoLayers::refresh() tells them apart.
bool
layerRefreshOwed(const std::string& identifier)
{
    const std::shared_ptr<const GeoUsdConvert> convert = GeoViewerLayer::layerConvert(identifier);
    std::string layerPath;
    SdfLayer::FileFormatArguments args;
    std::string consumer;
    if (!convert || !SdfLayer::SplitIdentifier(identifier, &layerPath, &args) || !GeoLayers::isConsumerLayer(identifier, &consumer)) {
        return true;
    }
    const std::string prefix = "natrongeo:";
    const std::string suffix = ".natrongeo";
    if ((layerPath.size() <= prefix.size() + suffix.size()) || (layerPath.compare(0, prefix.size(), prefix) != 0)
        || (layerPath.compare(layerPath.size() - suffix.size(), suffix.size(), suffix) != 0)) {
        return true;
    }
    const std::string key = layerPath.substr(prefix.size(), layerPath.size() - prefix.size() - suffix.size());
    const GeoUsdInputSceneConstPtr scene = GeoLayers::currentInputScene(key, consumer);
    const GeoUsdInputSceneConstPtr& converted = convert->options().inputScene;
    if ((scene != converted) && !(scene && converted && (scene->hash == converted->hash))) {
        return true;
    }

    return GeoLayers::currentDetail(key, consumer) != convert->detail();
}

} // namespace

bool
GeoDisplay::hasPendingEdits() const
{
    if (!_imp->staged || !_imp->guideLayer) {
        return false;
    }
    if (!_imp->authored) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        if (!(_imp->options == _imp->authoredOptions) || (_imp->componentVersion != _imp->authoredComponentVersion)) {
            return true;
        }
    }
    const std::vector<std::string> identifiers = GeoViewerLayer::findLayers(_imp->stagedSource);
    std::size_t detail = 0;
    for (const std::string& identifier : identifiers) {
        if (layerRefreshOwed(identifier)) {
            return true;
        }
        const std::shared_ptr<const GeoUsdConvert> convert = GeoViewerLayer::layerConvert(identifier);
        if (convert->detail()) {
            if ((detail >= _imp->authoredDetails.size()) || (_imp->authoredDetails[detail] != convert->detail().get())) {
                return true;
            }
            ++detail;
        }
    }

    return detail != _imp->authoredDetails.size();
}

bool
GeoDisplay::editStage(SceneStage& stage,
                      bool* renderChanged)
{
    if (renderChanged) {
        *renderChanged = false;
    }
    if (!_imp->staged || !_imp->guideLayer) {
        return false;
    }
    bool edited = false;
    {
        ResyncCounter counter(stage.getStage());
        std::string error;
        GeoViewerLayer::refreshLayers(_imp->stagedSource, &edited, &error);
        if (edited) {
            ++_imp->refreshes;
            _imp->refreshResyncs += counter.resyncs;
        }
    }
    if (renderChanged) {
        *renderChanged = edited;
    }
    const std::vector<std::string> identifiers = GeoViewerLayer::findLayers(_imp->stagedSource);
    int frame = 0;
    if (!identifiers.empty() && GeoViewerLayer::getServedFrame(identifiers.front(), &frame)) {
        _imp->refreshFrame = frame;
        _imp->hasRefreshFrame = true;
    }

    std::vector<std::shared_ptr<const GeoUsdConvert>> converts;
    std::vector<const GeoDetail*> details;
    for (const std::string& identifier : identifiers) {
        std::shared_ptr<const GeoUsdConvert> convert = GeoViewerLayer::layerConvert(identifier);
        if (convert && convert->detail()) {
            details.push_back(convert->detail().get());
            converts.push_back(convert);
        }
    }
    GeoDisplayOptions options;
    GeoComponentEnum componentMode;
    GeoComponentSelection componentSelection;
    U64 componentVersion;
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        options = _imp->options;
        componentMode = _imp->componentMode;
        componentSelection = _imp->componentSelection;
        componentVersion = _imp->componentVersion;
    }

    // Picking points shows every point as a marker, so that what can be picked is seen.
    const bool markers = options.pointMarkers || ((componentMode == eGeoComponentPoints) && (converts.size() == 1));
    // Each guide is rebuilt only for a change of what it is drawn from: a pick touches the
    // selection's guides alone, which on a dense mesh leaves every point's marker as it is.
    const GeoDisplayOptions& was = _imp->authoredOptions;
    const bool detailsChanged = !_imp->authored || (details != _imp->authoredDetails);
    const bool markersOwed = detailsChanged || (markers != _imp->authoredMarkers);
    const bool normalsOwed = detailsChanged || (options.normals != was.normals) || (options.normals && (options.normalScale != was.normalScale));
    const bool highlightOwed = detailsChanged || (options.highlightGroup != was.highlightGroup);
    const bool labelsOwed = detailsChanged || (options.pointNumbers != was.pointNumbers) || (options.primitiveNumbers != was.primitiveNumbers);
    const bool selectionOwed = detailsChanged || (componentVersion != _imp->authoredComponentVersion);
    bool convertsChanged = false;
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        convertsChanged = (converts != _imp->converts);
    }
    _imp->authoredOptions = options;
    _imp->authoredMarkers = markers;
    _imp->authoredComponentVersion = componentVersion;
    if (!markersOwed && !normalsOwed && !highlightOwed && !labelsOwed && !selectionOwed && !convertsChanged) {
        return edited;
    }

    int kinds = 0;
    if (markersOwed && markers) {
        kinds |= eGuideMarkers;
    }
    if (normalsOwed) {
        kinds |= eGuideNormals;
    }
    if (highlightOwed) {
        kinds |= eGuideHighlight;
    }
    if (detailsChanged) {
        kinds |= eGuidePacked;
    }
    Guides guides;
    LabelSet pointLabels, primitiveLabels;
    std::vector<GeoIndex> selectedPoints;
    bool packed = false;
    {
        std::vector<LabelAnchor> pointAnchors, primitiveAnchors;
        for (const std::shared_ptr<const GeoUsdConvert>& convert : converts) {
            const GeoDetail& detail = *convert->detail();
            packed = packed || hasPackedUSD(detail);
            if (kinds) {
                collectGuides(detail, options, kinds, &guides);
            }
            if (labelsOwed) {
                collectLabels(detail, options, &pointAnchors, &primitiveAnchors);
            }
        }
        if (labelsOwed) {
            pointLabels = bucketLabels(std::move(pointAnchors));
            primitiveLabels = bucketLabels(std::move(primitiveAnchors));
        }
    }
    if (selectionOwed && (converts.size() == 1)) {
        collectSelectionGuides(*converts.front()->detail(), componentSelection, &selectedPoints, &guides);
    }
    if (detailsChanged) {
        _imp->widthsValid = false;
    }
    // The bounds only size guides: playing a frame with none draws no pass over its points.
    const bool anyGuide = markers || options.normals || !options.highlightGroup.empty() || packed || !guides.selectedPoints.empty()
        || !guides.selectedPrimitives.empty();
    if (!_imp->widthsValid && anyGuide) {
        GeoBounds bounds;
        bool hasBounds = false;
        for (const std::shared_ptr<const GeoUsdConvert>& convert : converts) {
            const GeoDetail& detail = *convert->detail();
            if (detail.pointCount() > 0) {
                const GeoBounds b = geoDetailBounds(detail);
                bounds = hasBounds ? geoUnionBounds(bounds, b) : b;
                hasBounds = true;
            }
        }
        double diagonal = 1.;
        if (hasBounds) {
            const double dx = bounds.max[0] - bounds.min[0], dy = bounds.max[1] - bounds.min[1], dz = bounds.max[2] - bounds.min[2];
            diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!(diagonal > 0.)) {
                diagonal = 1.;
            }
        }
        _imp->markerWidth = float(diagonal * 0.004);
        _imp->lineWidth = float(diagonal * 0.0015);
        _imp->widthsValid = true;
    } else if (!_imp->widthsValid) {
        _imp->markerWidth = 0.004f;
        _imp->lineWidth = 0.0015f;
    }
    const float markerWidth = _imp->markerWidth;
    const float lineWidth = _imp->lineWidth;

    {
        SdfChangeBlock block;
        const SdfLayerHandle layer = _imp->guideLayer;
        const SdfPath rootPath = SdfPath::AbsoluteRootPath().AppendChild(TfToken(kRootName));
        SdfPrimSpecHandle root = layer->GetPrimAtPath(rootPath);
        if (!root) {
            root = SdfPrimSpec::New(layer->GetPseudoRoot(), kRootName, SdfSpecifierDef, "Scope");
        }
        if (root) {
            setAttribute(root, "purpose", SdfValueTypeNames->Token, VtValue(TfToken("guide")), SdfVariabilityUniform);
            if (markersOwed) {
                if (markers) {
                    authorGuide(layer, root, kPointMarkersName, false, guides.markers, markerWidth, GfVec3f(0.95f, 0.8f, 0.2f));
                } else {
                    removeGuide(root, kPointMarkersName);
                }
            }
            if (normalsOwed) {
                if (options.normals) {
                    authorGuide(layer, root, kNormalsName, true, guides.normals, lineWidth, GfVec3f(0.25f, 0.6f, 1.f));
                } else {
                    removeGuide(root, kNormalsName);
                }
            }
            if (highlightOwed) {
                if (!options.highlightGroup.empty()) {
                    authorGuide(layer, root, kHighlightPointsName, false, guides.highlightPoints, markerWidth * 2.f, GfVec3f(1.f, 0.4f, 0.1f));
                    authorGuide(layer, root, kHighlightEdgesName, true, guides.highlightEdges, lineWidth * 2.f, GfVec3f(1.f, 0.4f, 0.1f));
                } else {
                    removeGuide(root, kHighlightPointsName);
                    removeGuide(root, kHighlightEdgesName);
                }
            }
            if (detailsChanged) {
                if (!guides.packedBounds.empty()) {
                    authorGuide(layer, root, kPackedBoundsName, true, guides.packedBounds, lineWidth, GfVec3f(0.35f, 0.9f, 0.45f));
                } else {
                    removeGuide(root, kPackedBoundsName);
                }
            }
            if (selectionOwed) {
                if (!guides.selectedPoints.empty()) {
                    authorGuide(layer, root, kSelectedPointsName, false, guides.selectedPoints, markerWidth * 2.f, kSelectionColour);
                } else {
                    removeGuide(root, kSelectedPointsName);
                }
                if (!guides.selectedPrimitives.empty()) {
                    authorGuide(layer, root, kSelectedPrimitivesName, true, guides.selectedPrimitives, lineWidth * 2.f, kSelectionColour);
                } else {
                    removeGuide(root, kSelectedPrimitivesName);
                }
            }
        }
    }
    _imp->authored = true;
    _imp->authoredDetails = details;
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        _imp->converts.swap(converts);
        if (selectionOwed) {
            _imp->selectedPoints.swap(selectedPoints);
        }
        if (labelsOwed) {
            _imp->pointLabels = std::move(pointLabels);
            _imp->primitiveLabels = std::move(primitiveLabels);
        }
    }

    return true;
} // GeoDisplay::editStage

bool
GeoDisplay::showsGuidePurpose() const
{
    return _imp->staged;
}

U64
GeoDisplay::getRefreshCount() const
{
    return _imp->refreshes;
}

U64
GeoDisplay::getRefreshResyncCount() const
{
    return _imp->refreshResyncs;
}

bool
GeoDisplay::getRefreshFrame(int* frame) const
{
    if (frame) {
        *frame = _imp->refreshFrame;
    }

    return _imp->hasRefreshFrame;
}

int
GeoDisplay::paintLabels(QPainter& painter,
                        const double viewProjection[16],
                        int width,
                        int height) const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    if ((_imp->pointLabels.empty() && _imp->primitiveLabels.empty()) || (width <= 0) || (height <= 0)) {
        return 0;
    }
    painter.save();
    QFont font = painter.font();
    font.setPointSizeF(std::max(6., font.pointSizeF() * 0.85));
    painter.setFont(font);
    int budget = kMaxLabelsPerPaint;
    std::size_t scanned = 0;
    paintAnchors(painter, _imp->primitiveLabels, viewProjection, width, height, QColor(240, 210, 90), &budget, &scanned);
    paintAnchors(painter, _imp->pointLabels, viewProjection, width, height, QColor(120, 220, 240), &budget, &scanned);
    painter.restore();

    return kMaxLabelsPerPaint - budget;
}

bool
GeoDisplay::pickPrimitives(const std::string& primPath,
                           GeoViewerLayer::Pick* pick) const
{
    std::vector<std::shared_ptr<const GeoUsdConvert>> converts;
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        converts = _imp->converts;
    }
    for (const std::shared_ptr<const GeoUsdConvert>& convert : converts) {
        if (GeoViewerLayer::pickPrimitives(*convert, primPath, pick)) {
            return true;
        }
    }

    return false;
}

bool
GeoDisplay::setComponentState(GeoComponentEnum mode,
                              const GeoComponentSelection& selection)
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    const bool markersChange = ((mode == eGeoComponentPoints) != (_imp->componentMode == eGeoComponentPoints)) && !_imp->options.pointMarkers;
    // Only picked points and packed primitives have guides; the renderer highlights the rest.
    const auto guided = [](const GeoComponentSelection& s) {
        return ((s.type == eGeoComponentPoints) || (s.type == eGeoComponentPrimitives)) && !s.elements.empty();
    };
    const bool selectionChange = (guided(selection) || guided(_imp->componentSelection)) && !(selection == _imp->componentSelection);
    _imp->componentMode = mode;
    _imp->componentSelection = selection;
    if (!markersChange && !selectionChange) {
        return false;
    }
    ++_imp->componentVersion;

    return true;
}

std::shared_ptr<const GeoUsdConvert>
GeoDisplay::componentConvert() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return (_imp->converts.size() == 1) ? _imp->converts.front() : std::shared_ptr<const GeoUsdConvert>();
}

void
GeoDisplay::mapComponentHits(const std::vector<GeoViewerLayer::ComponentHit>& hits,
                             bool firstOnly,
                             GeoComponentSelection* selection) const
{
    std::shared_ptr<const GeoUsdConvert> convert;
    std::vector<GeoIndex> selectedPoints;
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        if (_imp->converts.size() != 1) {
            return;
        }
        convert = _imp->converts.front();
        if (selection->type == eGeoComponentPoints) {
            selectedPoints = _imp->selectedPoints;
        }
    }
    if (selection->type != eGeoComponentPoints) {
        GeoViewerLayer::mapComponentHits(*convert, hits, firstOnly, selection);

        return;
    }
    const SdfPath root = SdfPath::AbsoluteRootPath().AppendChild(TfToken(kRootName));
    const std::string markersPath = root.AppendChild(TfToken(kPointMarkersName)).GetString();
    const std::string selectedPath = root.AppendChild(TfToken(kSelectedPointsName)).GetString();
    const GeoIndex pointCount = convert->detail()->pointCount();
    for (const GeoViewerLayer::ComponentHit& hit : hits) {
        GeoIndex point = GeoIndexMap::kInvalidIndex;
        // The markers hold every point in number order; the picked points' guide drawn over them
        // holds the picked ones.
        if ((hit.primPath == markersPath) && (hit.index >= 0) && (hit.index < pointCount)) {
            point = hit.index;
        } else if ((hit.primPath == selectedPath) && (hit.index >= 0) && (std::size_t(hit.index) < selectedPoints.size())) {
            point = selectedPoints[std::size_t(hit.index)];
        }
        if (point >= 0) {
            selection->elements.push_back(point);
            if (firstOnly) {
                break;
            }
        }
    }
    selection->normalize();
}

NATRON_NAMESPACE_EXIT
