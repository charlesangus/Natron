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
#include "Engine/GeoDetail.h"
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

// The layer's content changes in place, so one hash stands for it whatever it holds.
const U64 kGuideLayerHash = 0x4e47656f44697370ULL;

const int kMaxLabelsPerPaint = 1000;
// Past this many elements of a kind, a paint looks at an even sample of them.
const std::size_t kMaxLabelsScanned = 250000;

struct LabelAnchor {
    float position[3];
    GeoIndex number;
};

struct Guides {
    VtVec3fArray markers;
    VtVec3fArray normals;
    VtVec3fArray highlightPoints;
    VtVec3fArray highlightEdges;
    VtVec3fArray packedBounds;
    float markerWidth = 0.f;
    float lineWidth = 0.f;
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

void
collectGuides(const GeoDetail& detail,
              const GeoDisplayOptions& options,
              Guides* guides)
{
    const GeoIndexMap& points = detail.pointMap();
    if (options.pointMarkers) {
        points.forEachLive([&](GeoIndex, GeoOffset offset) {
            guides->markers.push_back(toVec(detail.pointPosition(offset)));
        });
    }
    if (options.normals) {
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
    if (!options.highlightGroup.empty()) {
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
    primitives.forEachLive([&](GeoIndex, GeoOffset primitive) {
        if (detail.primitiveType(primitive) == eGeoPrimitivePackedUSD) {
            addBox(&guides->packedBounds, geoPackedWorldBounds(detail, primitive));
        }
    });
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
             const std::vector<LabelAnchor>& anchors,
             const double m[16],
             int width,
             int height,
             const QColor& colour,
             int* budget)
{
    if (anchors.empty() || (*budget <= 0)) {
        return;
    }
    painter.setPen(colour);
    const std::size_t stride = std::max<std::size_t>(1, anchors.size() / kMaxLabelsScanned);
    for (std::size_t i = 0; (i < anchors.size()) && (*budget > 0); i += stride) {
        const float* p = anchors[i].position;
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
        painter.drawText(at, QString::number(anchors[i].number));
        --*budget;
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
    std::vector<LabelAnchor> pointLabels;
    std::vector<LabelAnchor> primitiveLabels;

    std::atomic<bool> staged { false };
    std::atomic<U64> refreshes { 0 };
    std::atomic<U64> refreshResyncs { 0 };

    // The syncing thread's, under the stage lock it holds.
    SdfLayerRefPtr guideLayer;
    ScenePayload stagedSource;
    bool authored = false;
    std::vector<const GeoDetail*> authoredDetails;
    GeoDisplayOptions authoredOptions;
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
            std::lock_guard<std::mutex> lock(_imp->mutex);
            _imp->converts.clear();
            _imp->pointLabels.clear();
            _imp->primitiveLabels.clear();
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

bool
GeoDisplay::editStage(SceneStage& stage)
{
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

    std::vector<std::shared_ptr<const GeoUsdConvert>> converts;
    std::vector<const GeoDetail*> details;
    for (const std::string& identifier : GeoViewerLayer::findLayers(_imp->stagedSource)) {
        std::shared_ptr<const GeoUsdConvert> convert = GeoViewerLayer::layerConvert(identifier);
        if (convert && convert->detail()) {
            details.push_back(convert->detail().get());
            converts.push_back(convert);
        }
    }
    const GeoDisplayOptions options = getOptions();
    if (_imp->authored && !edited && (details == _imp->authoredDetails) && (options == _imp->authoredOptions)) {
        return false;
    }

    Guides guides;
    std::vector<LabelAnchor> pointLabels, primitiveLabels;
    GeoBounds bounds;
    bool hasBounds = false;
    for (const std::shared_ptr<const GeoUsdConvert>& convert : converts) {
        const GeoDetail& detail = *convert->detail();
        collectGuides(detail, options, &guides);
        collectLabels(detail, options, &pointLabels, &primitiveLabels);
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
    guides.markerWidth = float(diagonal * 0.004);
    guides.lineWidth = float(diagonal * 0.0015);

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
            if (options.pointMarkers) {
                authorGuide(layer, root, kPointMarkersName, false, guides.markers, guides.markerWidth, GfVec3f(0.95f, 0.8f, 0.2f));
            } else {
                removeGuide(root, kPointMarkersName);
            }
            if (options.normals) {
                authorGuide(layer, root, kNormalsName, true, guides.normals, guides.lineWidth, GfVec3f(0.25f, 0.6f, 1.f));
            } else {
                removeGuide(root, kNormalsName);
            }
            if (!options.highlightGroup.empty()) {
                authorGuide(layer, root, kHighlightPointsName, false, guides.highlightPoints, guides.markerWidth * 2.f, GfVec3f(1.f, 0.4f, 0.1f));
                authorGuide(layer, root, kHighlightEdgesName, true, guides.highlightEdges, guides.lineWidth * 2.f, GfVec3f(1.f, 0.4f, 0.1f));
            } else {
                removeGuide(root, kHighlightPointsName);
                removeGuide(root, kHighlightEdgesName);
            }
            authorGuide(layer, root, kPackedBoundsName, true, guides.packedBounds, guides.lineWidth, GfVec3f(0.35f, 0.9f, 0.45f));
        }
    }
    _imp->authored = true;
    _imp->authoredDetails = details;
    _imp->authoredOptions = options;
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        _imp->converts.swap(converts);
        _imp->pointLabels.swap(pointLabels);
        _imp->primitiveLabels.swap(primitiveLabels);
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
    paintAnchors(painter, _imp->primitiveLabels, viewProjection, width, height, QColor(240, 210, 90), &budget);
    paintAnchors(painter, _imp->pointLabels, viewProjection, width, height, QColor(120, 220, 240), &budget);
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

NATRON_NAMESPACE_EXIT
