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

#include "Gui/GeoSpreadsheet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QtCore/QEvent>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QLocale>
#include <QtCore/QMetaObject>
#include <QtCore/QThread>
#include <QtGui/QColor>
#include <QtGui/QPalette>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTableView>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Geo/GeoGroupOps.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePrimElements.h"
#include "Engine/TLSHolder.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/ScenePrimInspector.h"
#include "Gui/SceneViewport.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

struct GeoSpreadsheetBuildResult {
    GeoSpreadsheetModel::RowOrderPtr order;
    GeoSpreadsheetModel::RowInversePtr inverse;
    QString error;
};

struct GeoSpreadsheetPickResult {
    GeoComponentSelection selection;
    bool cancelled = false;
};

struct GeoSpreadsheetShowResult {
    // Sorted, non-overlapping runs of view rows, both ends included.
    std::vector<std::pair<int, int>> ranges;
    std::int64_t selected = 0;
    std::int64_t hidden = 0;
    bool cancelled = false;
};

namespace {

// Rows beyond this many are gathered on the worker rather than where the selection was made.
constexpr std::int64_t kInlinePickRows = 1000000;

typedef std::vector<std::pair<int, int>> RowRanges;

const char* const kClassLabels[kGeoAttribClassCount] = {
    QT_TRANSLATE_NOOP("GeoSpreadsheetWidget", "Points"),
    QT_TRANSLATE_NOOP("GeoSpreadsheetWidget", "Vertices"),
    QT_TRANSLATE_NOOP("GeoSpreadsheetWidget", "Primitives"),
    QT_TRANSLATE_NOOP("GeoSpreadsheetWidget", "Detail"),
};

const char* const kClassObjectSuffixes[kGeoAttribClassCount] = { "Points", "Vertices", "Primitives", "Detail" };

QString
classLabel(GeoAttribClassEnum cls)
{
    return GeoSpreadsheetWidget::tr(kClassLabels[cls]);
}

// Exact counts of millions of rows would widen the class bar past the inspector's default width;
// the exact count is in the button's tooltip.
QString
compactCount(std::int64_t count)
{
    if (count < 100000) {
        return QString::number(count);
    }
    if (count < 1000000) {
        return QString::number(count / 1000) + QLatin1Char('k');
    }
    if (count < 1000000000) {
        return QString::number((double)count / 1e6, 'g', 3) + QLatin1Char('M');
    }

    return QString::number((double)count / 1e9, 'g', 3) + QLatin1Char('B');
}

QString
groupHelp()
{
    return GeoSpreadsheetWidget::tr("Show only the elements of a group, by name or with wildcards (top*). Also takes element numbers (0-9 14), "
                                    "ranges with a step (0-99:2), ! to invert and ^ to remove. Geometry also takes attribute tests (@Cd > 0.5).");
}

// A prim's rows that its highlighted points or faces name: a face highlight names faces only, a point
// highlight points and the vertices that use them.
bool
primSelectedRowMask(const ScenePrimElementsSource& prim,
                    GeoAttribClassEnum cls,
                    const GeoComponentSelection& selection,
                    const std::atomic<bool>& cancelled,
                    GeoElementMask* mask)
{
    const std::int64_t rows = prim.rowCount(cls);
    mask->assign((std::size_t)std::max<std::int64_t>(0, rows), 0);
    if (selection.type == eGeoComponentPrimitives) {
        if (cls == eGeoAttribClassPrimitive) {
            for (const GeoIndex element : selection.elements) {
                if ((element >= 0) && (element < rows)) {
                    (*mask)[(std::size_t)element] = 1;
                }
            }
        }

        return true;
    }
    if (selection.type != eGeoComponentPoints) {
        return true;
    }
    if (cls == eGeoAttribClassPoint) {
        for (const GeoIndex element : selection.elements) {
            if ((element >= 0) && (element < rows)) {
                (*mask)[(std::size_t)element] = 1;
            }
        }
    } else if (cls == eGeoAttribClassVertex) {
        GeoIndex highest = -1;
        for (const GeoIndex element : selection.elements) {
            highest = std::max(highest, element);
        }
        std::vector<char> points((std::size_t)(highest + 1), 0);
        for (const GeoIndex element : selection.elements) {
            if (element >= 0) {
                points[(std::size_t)element] = 1;
            }
        }
        for (std::int64_t vertex = 0; vertex < rows; ++vertex) {
            if (((vertex & 0xFFFF) == 0) && cancelled.load()) {
                return true;
            }
            const std::int64_t point = prim.pointOfVertex(vertex);
            if ((point >= 0) && (point <= highest) && points[(std::size_t)point]) {
                (*mask)[(std::size_t)vertex] = 1;
            }
        }
    }

    return true;
}

// The rows of @p cls that @p selection names, as a mask in the same storage-offset space as geoSelectElements.
// Returns false for a source that has no view selection to read; the highlighted points or faces of a prim
// fill the mask for a prim.
bool
selectedRowMask(const GeoSpreadsheetSource& source,
                GeoAttribClassEnum cls,
                const GeoComponentSelection& selection,
                const std::atomic<bool>& cancelled,
                GeoElementMask* mask)
{
    const GeoDetailSpreadsheetSource* geometry = dynamic_cast<const GeoDetailSpreadsheetSource*>(&source);

    if (const ScenePrimElementsSource* prim = dynamic_cast<const ScenePrimElementsSource*>(&source)) {
        return primSelectedRowMask(*prim, cls, selection, cancelled, mask);
    }
    if (!geometry) {
        return false;
    }
    const GeoDetail& detail = *geometry->detail();
    const GeoIndexMap& pointMap = detail.pointMap();
    const GeoIndexMap& primitiveMap = detail.primitiveMap();
    GeoElementMask points;
    GeoElementMask primitives;
    bool havePoints = false;
    bool havePrimitives = false;

    switch (selection.type) {
    case eGeoComponentPoints: {
        points.assign((std::size_t)std::max<GeoOffset>(0, pointMap.offsetSize()), 0);
        for (const GeoIndex element : selection.elements) {
            const GeoOffset offset = pointMap.offsetFromIndex(element);
            if ((offset >= 0) && ((std::size_t)offset < points.size())) {
                points[(std::size_t)offset] = 1;
            }
        }
        havePoints = true;
        break;
    }
    case eGeoComponentPrimitives: {
        primitives.assign((std::size_t)std::max<GeoOffset>(0, primitiveMap.offsetSize()), 0);
        for (const GeoIndex element : selection.elements) {
            const GeoOffset offset = primitiveMap.offsetFromIndex(element);
            if ((offset >= 0) && ((std::size_t)offset < primitives.size())) {
                primitives[(std::size_t)offset] = 1;
            }
        }
        havePrimitives = true;
        break;
    }
    case eGeoComponentEdges: {
        std::vector<GeoEdge> edges;
        for (const GeoEdge& edge : selection.edges) {
            const GeoOffset a = pointMap.offsetFromIndex(edge.p0);
            const GeoOffset b = pointMap.offsetFromIndex(edge.p1);
            if ((a >= 0) && (b >= 0)) {
                edges.push_back(GeoEdge(a, b));
            }
        }
        if (cls == eGeoAttribClassPrimitive) {
            primitives = geoPromoteEdgesToPrimitives(detail, edges);
            havePrimitives = true;
        } else {
            points = geoPromoteEdgesToPoints(detail, edges);
            havePoints = true;
        }
        break;
    }
    default:
        break;
    }
    if (cancelled.load()) {
        return true;
    }
    if (!havePoints && !havePrimitives) {
        mask->clear();

        return true;
    }
    if (cls == eGeoAttribClassPrimitive) {
        *mask = havePrimitives ? std::move(primitives) : geoPromotePointsToPrimitives(detail, points, true);

        return true;
    }
    if (!havePoints) {
        points = geoPromotePrimitivesToPoints(detail, primitives);
    }
    if (cls == eGeoAttribClassPoint) {
        *mask = std::move(points);

        return true;
    }
    const GeoIndexMap& vertexMap = detail.vertexMap();
    const GeoOffset vertexCount = vertexMap.offsetSize();
    mask->assign((std::size_t)std::max<GeoOffset>(0, vertexCount), 0);
    for (GeoOffset vertex = 0; vertex < vertexCount; ++vertex) {
        if (((vertex & 0xFFFF) == 0) && cancelled.load()) {
            return true;
        }
        if (!vertexMap.isLive(vertex)) {
            continue;
        }
        const GeoOffset point = detail.vertexPoint(vertex);
        if ((point >= 0) && ((std::size_t)point < points.size()) && points[(std::size_t)point]) {
            (*mask)[(std::size_t)vertex] = 1;
        }
    }

    return true;
}

// Runs on the worker thread. A null order with no error is a cancelled build.
GeoSpreadsheetBuildResult
runBuild(const GeoSpreadsheetModel::SourcePtr& source,
         GeoAttribClassEnum cls,
         const std::string& pattern,
         int sortColumn,
         bool descending,
         const GeoComponentSelection* selectedOnly,
         const std::atomic<bool>& cancelled)
{
    GeoSpreadsheetBuildResult result;

    if (cancelled.load()) {
        return result;
    }
    GeoElementMask mask;
    bool filtered = false;
    std::string error;
    if (!geoSpreadsheetFilterMask(*source, cls, pattern, selectedOnly, cancelled, &mask, &filtered, &error)) {
        result.error = QString::fromStdString(error);

        return result;
    }
    if (cancelled.load()) {
        return result;
    }
    result.order = geoSpreadsheetBuildOrder(*source, cls, filtered ? &mask : nullptr, sortColumn, descending, cancelled);
    if (result.order) {
        const std::int64_t count = source->rowCount(cls);
        std::shared_ptr<std::vector<GeoIndex>> inverse = std::make_shared<std::vector<GeoIndex>>((std::size_t)count, -1);
        const std::vector<GeoIndex>& order = *result.order;
        for (std::size_t row = 0; row < order.size(); ++row) {
            if (((row & 0xFFFF) == 0) && cancelled.load()) {
                result.order.reset();

                return result;
            }
            const GeoIndex element = order[row];
            if ((element >= 0) && (element < count)) {
                (*inverse)[(std::size_t)element] = (GeoIndex)row;
            }
        }
        result.inverse = inverse;
    }

    return result;
}

// The view rows showing the picked elements, as runs. Edges are shown as their endpoints. With a sort or
// filter order, @p inverse gives each element's row and elements it leaves out are counted as hidden.
GeoSpreadsheetShowResult
showRows(const GeoSpreadsheetModel::SourcePtr& source,
         GeoAttribClassEnum cls,
         const GeoComponentSelection& selection,
         bool ordered,
         const GeoSpreadsheetModel::RowInversePtr& inverse,
         const std::atomic<bool>& cancelled)
{
    GeoSpreadsheetShowResult result;
    const GeoDetailSpreadsheetSource* geometry = dynamic_cast<const GeoDetailSpreadsheetSource*>(source.get());

    if (!geometry && !dynamic_cast<const ScenePrimElementsSource*>(source.get())) {
        return result;
    }
    const std::int64_t count = source->rowCount(cls);
    std::vector<GeoIndex> endpoints;
    const std::vector<GeoIndex>* elements = &selection.elements;
    if (geometry && (selection.type == eGeoComponentEdges)) {
        const GeoDetail& detail = *geometry->detail();
        std::vector<GeoEdge> edges;
        for (const GeoEdge& edge : selection.edges) {
            const GeoOffset a = detail.pointMap().offsetFromIndex(edge.p0);
            const GeoOffset b = detail.pointMap().offsetFromIndex(edge.p1);
            if ((a >= 0) && (b >= 0)) {
                edges.push_back(GeoEdge(a, b));
            }
        }
        const GeoElementMask points = geoPromoteEdgesToPoints(detail, edges);
        const GeoIndexMap& map = detail.pointMap();
        const std::size_t limit = std::min(points.size(), (std::size_t)std::max<GeoOffset>(0, map.offsetSize()));
        for (std::size_t offset = 0; offset < limit; ++offset) {
            if (points[offset] && map.isLive((GeoOffset)offset)) {
                endpoints.push_back(map.indexFromOffset((GeoOffset)offset));
            }
        }
        elements = &endpoints;
    }
    std::vector<int> rows;
    rows.reserve(elements->size());
    std::int64_t visited = 0;
    for (const GeoIndex element : *elements) {
        if (((++visited) & 0xFFFF) == 0 && cancelled.load()) {
            result.cancelled = true;

            return result;
        }
        if ((element < 0) || (element >= count)) {
            continue;
        }
        ++result.selected;
        const GeoIndex row = ordered ? ((inverse && (element < (GeoIndex)inverse->size())) ? (*inverse)[(std::size_t)element] : -1) : element;
        if (row < 0) {
            ++result.hidden;
        } else {
            rows.push_back((int)row);
        }
    }
    if (!std::is_sorted(rows.begin(), rows.end())) {
        std::sort(rows.begin(), rows.end());
    }
    for (const int row : rows) {
        if (!result.ranges.empty() && (row <= result.ranges.back().second + 1)) {
            result.ranges.back().second = std::max(result.ranges.back().second, row);
        } else {
            result.ranges.emplace_back(row, row);
        }
    }

    return result;
}

// Marks the table's selection changes as not the user's while it is alive.
class FlagScope {
public:
    explicit FlagScope(bool& flag)
        : _flag(flag)
        , _previous(flag)
    {
        _flag = true;
    }

    ~FlagScope()
    {
        _flag = _previous;
    }

private:
    bool& _flag;
    bool _previous;
};

// The elements the rows name, as component numbers of the view: rows of a vertex class give the points
// their vertices use. Walks ranges, so selecting every row of a large class is one pass over an index list.
GeoSpreadsheetPickResult
pickRows(const GeoSpreadsheetModel::SourcePtr& source,
         GeoAttribClassEnum cls,
         const GeoSpreadsheetModel::RowOrderPtr& order,
         const RowRanges& ranges,
         const std::atomic<bool>& cancelled)
{
    GeoSpreadsheetPickResult result;
    const GeoDetailSpreadsheetSource* geometry = dynamic_cast<const GeoDetailSpreadsheetSource*>(source.get());
    const ScenePrimElementsSource* prim = dynamic_cast<const ScenePrimElementsSource*>(source.get());

    if ((!geometry && !prim) || (cls == eGeoAttribClassDetail)) {
        return result;
    }
    const GeoDetail* detail = geometry ? geometry->detail().get() : nullptr;
    const std::int64_t count = source->rowCount(cls);
    const bool vertices = cls == eGeoAttribClassVertex;
    result.selection.type = (cls == eGeoAttribClassPrimitive) ? eGeoComponentPrimitives : eGeoComponentPoints;
    std::vector<GeoIndex>& elements = result.selection.elements;
    std::int64_t visited = 0;

    for (const std::pair<int, int>& range : ranges) {
        for (std::int64_t row = range.first; row <= range.second; ++row) {
            if (((++visited) & 0xFFFF) == 0 && cancelled.load()) {
                result.cancelled = true;

                return result;
            }
            GeoIndex element = order ? ((row < (std::int64_t)order->size()) ? (*order)[(std::size_t)row] : -1) : row;
            if ((element < 0) || (element >= count)) {
                continue;
            }
            if (vertices) {
                if (prim) {
                    element = (GeoIndex)prim->pointOfVertex(element);
                } else {
                    const GeoOffset vertex = detail->vertexMap().offsetFromIndex(element);
                    element = (vertex >= 0) ? detail->pointMap().indexFromOffset(detail->vertexPoint(vertex)) : -1;
                }
                if (element < 0) {
                    continue;
                }
            }
            elements.push_back(element);
        }
    }
    result.selection.normalize();

    return result;
}

// The rows of the ranges, sorted and with overlaps merged.
RowRanges
mergedRanges(const QItemSelection& selection,
             std::int64_t* rows)
{
    RowRanges ranges;

    for (const QItemSelectionRange& range : selection) {
        if (range.isValid()) {
            ranges.emplace_back(range.top(), range.bottom());
        }
    }
    std::sort(ranges.begin(), ranges.end());
    RowRanges merged;
    *rows = 0;
    for (const std::pair<int, int>& range : ranges) {
        if (!merged.empty() && (range.first <= merged.back().second + 1)) {
            merged.back().second = std::max(merged.back().second, range.second);
        } else {
            merged.push_back(range);
        }
    }
    for (const std::pair<int, int>& range : merged) {
        *rows += (std::int64_t)range.second - range.first + 1;
    }

    return merged;
}

SceneViewport*
viewportOf(const ViewerInstancePtr& viewer)
{
    if (!viewer) {
        return nullptr;
    }
    for (const AppInstancePtr& instance : appPTR->getAppInstances()) {
        GuiAppInstance* guiApp = dynamic_cast<GuiAppInstance*>(instance.get());
        Gui* gui = guiApp ? guiApp->getGui() : nullptr;
        if (!gui) {
            continue;
        }
        for (ViewerTab* tab : gui->getViewersList_mt_safe()) {
            if (tab->getInternalNode() == viewer.get()) {
                return tab->getSceneViewport();
            }
        }
    }

    return nullptr;
}

class ScenePrimSpreadsheetTab
    : public ScenePrimInspectorTab {
public:
    ScenePrimSpreadsheetTab()
        : _widget(new GeoSpreadsheetWidget(QString::fromUtf8("sceneGraphSpreadsheet")))
    {
    }

    QString getTitle() const override
    {
        return ScenePrimInspector::tr("Spreadsheet");
    }

    QWidget* getWidget() const override
    {
        return _widget;
    }

    bool readsWithoutPrim(const NodePtr& node) const override
    {
        return putsOutGeometry(node);
    }

    std::function<void()> read(const ScenePrimInspectorRead& read) override
    {
        if (putsOutGeometry(read.getNode())) {
            return readGeometry(read);
        }
        const ScenePrimElements elements = SceneOps::readPrimElements(
            [&read](const std::function<void(const SceneStage* stage)>& readOne) { return read.readStage(readOne); },
            read.getPath().toStdString(),
            read.getFrame(),
            read.getFramesPerSecond());

        if ((elements.status == eScenePrimElementsStatusCancelled) || read.isCancelled()) {
            return std::function<void()>();
        }
        switch (elements.status) {
        case eScenePrimElementsStatusOk: {
            const GeoSpreadsheetModel::SourcePtr source = elements.source;

            const ViewerInstanceWPtr viewer = read.getViewer();
            const QString path = read.getPath();
            const NodeWPtr node = read.getNode();

            return [this, source, viewer, path, node]() {
                _widget->setSelectionViewer(ViewerInstanceWPtr());
                _widget->setHighlightTarget(viewer, path, node);
                _widget->setSnapshot(source);
            };
        }
        case eScenePrimElementsStatusNotLoaded:
            return [this]() {
                _widget->setHighlightTarget(ViewerInstanceWPtr(), QString(), NodeWPtr());
                _widget->showMessage(GeoSpreadsheetWidget::tr("Not loaded — load the payload from the scene graph to inspect it"));
            };
        case eScenePrimElementsStatusMissing:
        case eScenePrimElementsStatusCancelled:
            break;
        }

        return [this]() {
            _widget->setHighlightTarget(ViewerInstanceWPtr(), QString(), NodeWPtr());
            _widget->setSnapshot(GeoSpreadsheetModel::SourcePtr());
        };
    }

    void setStale(bool stale) override
    {
        _widget->setStale(stale);
    }

    void clear() override
    {
        _widget->setHighlightTarget(ViewerInstanceWPtr(), QString(), NodeWPtr());
        _widget->setSnapshot(GeoSpreadsheetModel::SourcePtr());
    }

    void clearSelection() override
    {
        _widget->clearSelection();
    }

    bool readsAtFrame() const override
    {
        return true;
    }

private:
    static bool putsOutGeometry(const NodePtr& node)
    {
        const EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

        return effect && node->isActivated() && effect->producesGeometryData();
    }

    // The node's whole detail: a prim selected in its scene, one of the layer the viewer made of
    // it, narrows nothing.
    std::function<void()> readGeometry(const ScenePrimInspectorRead& read)
    {
        GeoDetailConstPtr detail;
        const ViewerInstancePtr& viewer = read.getViewer();
        int servedFrame = 0;
        NodePtr servedNode;
        U64 servedHash = 0;
        // Texture 0 is input A, the input the 3D view shows and the only one getViewer() is set for. During
        // playback the 3D view can still be on an earlier frame than the one asked for, and right after
        // the viewer is switched to another node, or the node is edited, it can still hold the geometry
        // it had before.
        const bool fromViewer = viewer && viewer->getShownGeometry(0, &detail, &servedFrame, &servedNode, &servedHash) && detail
            && (double(servedFrame) == read.getFrame()) && servedNode && (servedNode == read.getNode())
            && (servedHash == servedNode->getHashValue());

        // The viewer's own copy, when it has one, numbers the elements as its 3D view picks them.
        if (!fromViewer) {
            detail.reset();
            const EffectInstancePtr effect = read.getNode()->getEffectInstance();
            std::string error;
            EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
            try {
                code = effect->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(read.getFrame(), ViewIdx(0)), &detail, &error);
            } catch (const std::exception& e) {
                error = e.what();
            } catch (...) {
            }
            appPTR->getAppTLS()->cleanupTLSForThread();
            if (read.isCancelled()) {
                return std::function<void()>();
            }
            if ((code != EffectInstance::eRenderRoIRetCodeOk) || !detail) {
                const QString text = error.empty() ? GeoSpreadsheetWidget::tr("The geometry could not be evaluated")
                                                   : GeoSpreadsheetWidget::tr("The geometry could not be evaluated: %1").arg(QString::fromStdString(error));

                return [this, text]() {
                    _widget->setHighlightTarget(ViewerInstanceWPtr(), QString(), NodeWPtr());
                    _widget->showMessage(text);
                };
            }
        }
        if (read.isCancelled()) {
            return std::function<void()>();
        }
        const GeoSpreadsheetModel::SourcePtr source = std::make_shared<GeoDetailSpreadsheetSource>(detail);
        const ViewerInstanceWPtr target = fromViewer ? ViewerInstanceWPtr(viewer) : ViewerInstanceWPtr();

        return [this, source, target]() {
            _widget->setHighlightTarget(ViewerInstanceWPtr(), QString(), NodeWPtr());
            _widget->setSelectionViewer(target);
            _widget->setSnapshot(source);
        };
    }

    GeoSpreadsheetWidget* _widget;
};

} // anon namespace

bool
geoSpreadsheetFilterMask(const GeoSpreadsheetSource& source,
                         GeoAttribClassEnum cls,
                         const std::string& pattern,
                         const GeoComponentSelection* selectedOnly,
                         const std::atomic<bool>& cancelled,
                         GeoElementMask* mask,
                         bool* filtered,
                         std::string* error)
{
    mask->clear();
    *filtered = false;
    if (cls == eGeoAttribClassDetail) {
        return true;
    }
    const GeoDetailSpreadsheetSource* geometry = dynamic_cast<const GeoDetailSpreadsheetSource*>(&source);
    if (pattern.find_first_not_of(" \t\r\n") != std::string::npos) {
        if (geometry) {
            if (!geoSelectElements(*geometry->detail(), cls, pattern, mask, error)) {
                return false;
            }
            *filtered = true;
        } else if (const ScenePrimElementsSource* prim = dynamic_cast<const ScenePrimElementsSource*>(&source)) {
            std::vector<GeoNamedElementGroup> groups;
            for (const ScenePrimElementGroup& group : prim->groups()) {
                if (group.cls == cls) {
                    groups.push_back({ group.name, std::span<const std::int32_t>(group.indices.data(), group.indices.size()) });
                }
            }
            if (!geoSelectElementsInDomain((std::size_t)std::max<std::int64_t>(0, source.rowCount(cls)), groups, pattern, mask, error, false)) {
                return false;
            }
            *filtered = true;
        }
    }
    if (selectedOnly) {
        GeoElementMask selected;
        if (selectedRowMask(source, cls, *selectedOnly, cancelled, &selected)) {
            if (cancelled.load()) {
                return true;
            }
            if (*filtered) {
                for (std::size_t i = 0; i < mask->size(); ++i) {
                    (*mask)[i] = ((*mask)[i] && (i < selected.size()) && selected[i]) ? 1 : 0;
                }
            } else {
                *mask = std::move(selected);
                *filtered = true;
            }
        }
    }
    if (*filtered && geometry) {
        *mask = geoSpreadsheetRowMask(geometry->detail()->indexMap(cls), *mask);
    }

    return true;
}

GeoSpreadsheetWidget::GeoSpreadsheetWidget(const QString& objectName,
                                           QWidget* parent)
    : QWidget(parent)
    , _model(nullptr)
    , _classButtons()
    , _stack(new QStackedWidget(this))
    , _table(new QTableView(_stack))
    , _message(new QLabel(_stack))
    , _group(nullptr)
    , _busyLabel(nullptr)
    , _error(nullptr)
    , _selectionNote(nullptr)
    , _selectedOnlyButton(nullptr)
    , _thread(new QThread(this))
    , _worker(new QObject)
{
    setObjectName(objectName);

    QWidget* classBar = new QWidget(this);
    QHBoxLayout* barLayout = new QHBoxLayout(classBar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(2);
    QButtonGroup* group = new QButtonGroup(this);
    group->setExclusive(true);
    for (int c = 0; c < kGeoAttribClassCount; ++c) {
        QToolButton* button = new QToolButton(classBar);
        button->setObjectName(objectName + QString::fromUtf8(kClassObjectSuffixes[c]));
        button->setCheckable(true);
        button->setAutoRaise(true);
        group->addButton(button, c);
        barLayout->addWidget(button);
        _classButtons[(std::size_t)c] = button;
    }
    barLayout->addStretch(1);
    QObject::connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        showClass((GeoAttribClassEnum)id);
        syncFromViewer(false, false);
    });

    _model = new GeoSpreadsheetModel(_table);
    _table->setObjectName(objectName + QString::fromUtf8("Table"));
    _table->setModel(_model);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setWordWrap(false);
    _table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    _table->horizontalHeader()->setStretchLastSection(false);
    QHeaderView* header = _table->horizontalHeader();
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    // A header click sorts; it must not also select the column's millions of cells.
    QObject::disconnect(header, SIGNAL(sectionPressed(int)), _table, nullptr);
    QObject::connect(header, &QHeaderView::sectionClicked, this, [this](int column) { headerClicked(column); });
    _table->installEventFilter(this);
    QObject::connect(_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { rowsSelected(); });
    QObject::connect(_table, &QTableView::doubleClicked, this, [this](const QModelIndex& index) { openFullValue(index); });

    _message->setObjectName(objectName + QString::fromUtf8("Message"));
    _message->setAlignment(Qt::AlignCenter);
    _message->setWordWrap(true);
    // A long message must not set the inspector's minimum width.
    _message->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _stack->addWidget(_table);
    _stack->addWidget(_message);

    QWidget* filterBar = new QWidget(this);
    QHBoxLayout* filterLayout = new QHBoxLayout(filterBar);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(4);
    _group = new QComboBox(filterBar);
    _group->setObjectName(objectName + QString::fromUtf8("Group"));
    _group->setEditable(true);
    _group->setInsertPolicy(QComboBox::NoInsert);
    _group->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _group->setMinimumContentsLength(8);
    _group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _group->lineEdit()->setPlaceholderText(tr("Group"));
    _group->lineEdit()->setClearButtonEnabled(true);
    _group->setToolTip(groupHelp());
    QObject::connect(_group->lineEdit(), &QLineEdit::editingFinished, this, [this]() { groupSubmitted(); });
    QObject::connect(_group, QOverload<int>::of(&QComboBox::activated), this, [this](int) { groupSubmitted(); });
    _busyLabel = new QLabel(tr("Sorting…"), filterBar);
    _busyLabel->setObjectName(objectName + QString::fromUtf8("Busy"));
    _busyLabel->hide();
    _selectionNote = new QLabel(filterBar);
    _selectionNote->setObjectName(objectName + QString::fromUtf8("SelectionNote"));
    _selectionNote->hide();
    _selectedOnlyButton = new QToolButton(filterBar);
    _selectedOnlyButton->setObjectName(objectName + QString::fromUtf8("SelectedOnly"));
    _selectedOnlyButton->setText(tr("Selected Only"));
    _selectedOnlyButton->setCheckable(true);
    _selectedOnlyButton->setAutoRaise(true);
    _selectedOnlyButton->setEnabled(false);
    _selectedOnlyButton->setToolTip(tr("Show only the elements selected in the viewer, within the group filter. "
                                       "While on, selecting rows does not change the viewer's selection."));
    QObject::connect(_selectedOnlyButton, &QToolButton::toggled, this, [this](bool on) { setSelectedOnly(on); });
    filterLayout->addWidget(_group, 1);
    filterLayout->addWidget(_selectedOnlyButton);
    filterLayout->addWidget(_busyLabel);
    filterLayout->addWidget(_selectionNote);

    _error = new QLabel(this);
    _error->setObjectName(objectName + QString::fromUtf8("Error"));
    _error->setWordWrap(true);
    _error->setStyleSheet(QString::fromUtf8("QLabel { color: #e05a5a; }"));
    _error->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _error->hide();

    _worker->moveToThread(_thread);
    _thread->start();

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(classBar);
    layout->addWidget(filterBar);
    layout->addWidget(_error);
    layout->addWidget(_stack, 1);

    updateClassBar();
}

GeoSpreadsheetWidget::~GeoSpreadsheetWidget()
{
    clearHighlight();
    if (_pickCancel) {
        _pickCancel->store(true);
    }
    if (_cancel) {
        _cancel->store(true);
    }
    _holdSorts.store(false);
    _thread->quit();
    _thread->wait();
    delete _worker;
}

QStringList
GeoSpreadsheetWidget::columnNames() const
{
    QStringList names;

    for (int c = 0; c < _model->columnCount(); ++c) {
        names << _model->headerData(c, Qt::Horizontal).toString();
    }

    return names;
}

void
GeoSpreadsheetWidget::setSnapshot(const GeoSpreadsheetModel::SourcePtr& source)
{
    const GeoAttribClassEnum shownClass = _model->attribClass();
    GeoAttribClassEnum cls = shownClass;
    QString wanted = wantedPattern();

    cancelBuild();
    cancelPick();

    if (source && (source->rowCount(cls) == 0)) {
        for (int c = 0; c < kGeoAttribClassCount; ++c) {
            if (source->rowCount((GeoAttribClassEnum)c) > 0) {
                cls = (GeoAttribClassEnum)c;
                break;
            }
        }
    }
    // A new frame of the same geometry keeps the widths, measured or set by the user: the reset
    // would put every column back to its default width.
    const bool mayKeepWidths = _model->source() && source && (cls == shownClass) && !_metricsOwed;
    const QStringList names = mayKeepWidths ? columnNames() : QStringList();
    std::vector<int> widths;
    for (int c = 0; c < names.size(); ++c) {
        widths.push_back(_table->columnWidth(c));
    }
    _model->setSnapshot(source, cls);
    _shownValid = false;
    if (!source || (cls != shownClass)) {
        _sortColumn = -1;
        _sortName.clear();
        wanted.clear();
        _pattern.clear();
        _typed.clear();
        setError(QString());
    } else {
        _sortColumn = _sortName.isEmpty() ? -1 : columnNames().indexOf(_sortName);
    }
    _message->clear();
    _stack->setCurrentWidget(_table);
    updateClassBar();
    if (mayKeepWidths && (columnNames() == names)) {
        for (int c = 0; c < names.size(); ++c) {
            _table->setColumnWidth(c, widths[(std::size_t)c]);
        }
    } else {
        updateMetrics();
    }
    populateGroups();
    updateSortIndicator();
    updateSelectedOnlyEnabled();
    if (source) {
        requestBuild(wanted);
        if (_busy) {
            syncFromViewer(false, false);
        }
    } else {
        setSelectionNote(QString());
    }
}

void
GeoSpreadsheetWidget::showMessage(const QString& text)
{
    cancelBuild();
    cancelPick();
    _shownValid = false;
    _model->setSnapshot(GeoSpreadsheetModel::SourcePtr(), _model->attribClass());
    populateGroups();
    _message->setText(text);
    _stack->setCurrentWidget(_message);
    updateClassBar();
    updateSelectedOnlyEnabled();
}

void
GeoSpreadsheetWidget::setStale(bool stale)
{
    QPalette palette = _table->palette();
    QColor color = QApplication::palette().color(QPalette::Text);

    if (stale) {
        color.setAlphaF(0.4);
    }
    palette.setColor(QPalette::Text, color);
    _table->setPalette(palette);
    _message->setEnabled(!stale);
}

void
GeoSpreadsheetWidget::clearSelection()
{
    const QSignalBlocker blocker(_table->selectionModel());
    _table->selectionModel()->clear();
}

void
GeoSpreadsheetWidget::setSelectionViewer(const ViewerInstanceWPtr& viewer)
{
    const bool wasActive = selectedOnlyActive();
    for (QMetaObject::Connection& connection : _viewerConnections) {
        QObject::disconnect(connection);
        connection = QMetaObject::Connection();
    }
    _selectionViewer = viewer;
    SceneViewport* viewport = viewportOf(viewer.lock());
    updateSelectedOnlyEnabled();
    if (!viewport) {
        cancelPick();
        setSelectionNote(QString());
        if (wasActive) {
            rebuildForSelection();
        }

        return;
    }
    _viewerConnections[0] = QObject::connect(viewport, &SceneViewport::componentSelectionChanged, this, [this]() { viewerSelectionChanged(); });
    _viewerConnections[1] = QObject::connect(viewport, &SceneViewport::geoPickModeChanged, this, [this](int) { viewerSelectionChanged(); });
    if (selectedOnlyActive() != wasActive) {
        rebuildForSelection();
    }
}

void
GeoSpreadsheetWidget::setHighlightTarget(const ViewerInstanceWPtr& viewer,
                                         const QString& primPath,
                                         const NodeWPtr& node)
{
    const std::string path = primPath.toStdString();
    const bool sameNode = !node.owner_before(_highlightNode) && !_highlightNode.owner_before(node);
    const bool same = sameNode && (path == _highlightPath) && !viewer.owner_before(_highlightViewer) && !_highlightViewer.owner_before(viewer);

    if (!same) {
        if (selectedOnlyActive() && !viewportOf(_selectionViewer.lock())) {
            setSelectedOnly(false);
        }
        cancelPick();
        clearHighlight();
        _highlightViewer = viewer;
        _highlightPath = path;
        _highlightNode = node;
    }
    updateSelectedOnlyEnabled();
}

void
GeoSpreadsheetWidget::clearHighlight()
{
    _highlightSelection = GeoComponentSelection();
    if (SceneViewport* viewport = viewportOf(_highlightViewer.lock())) {
        viewport->clearSceneComponentHighlight();
    }
}

void
GeoSpreadsheetWidget::applyHighlight(const GeoComponentSelection& selection)
{
    SceneViewport* viewport = viewportOf(_highlightViewer.lock());

    _highlightSelection = selection;
    if (!viewport) {
        return;
    }
    if (selection.empty() || ((selection.type != eGeoComponentPoints) && (selection.type != eGeoComponentPrimitives))) {
        viewport->clearSceneComponentHighlight();

        return;
    }
    std::vector<GeoIndex> indices(selection.elements.begin(), selection.elements.end());
    viewport->setSceneComponentHighlight(_highlightPath,
                                         (selection.type == eGeoComponentPrimitives) ? eGeoAttribClassPrimitive : eGeoAttribClassPoint,
                                         std::move(indices));
}

bool
GeoSpreadsheetWidget::selectedOnlyActive() const
{
    const GeoSpreadsheetModel::SourcePtr source = _model->source();

    if (!_selectedOnly || !source || (_model->attribClass() == eGeoAttribClassDetail)) {
        return false;
    }
    if (dynamic_cast<const ScenePrimElementsSource*>(source.get())) {
        return viewportOf(_highlightViewer.lock()) != nullptr;
    }

    return dynamic_cast<const GeoDetailSpreadsheetSource*>(source.get()) && viewportOf(_selectionViewer.lock());
}

void
GeoSpreadsheetWidget::updateSelectedOnlyEnabled()
{
    const GeoSpreadsheetModel::SourcePtr source = _model->source();
    bool usable = false;

    if (source) {
        if (dynamic_cast<const ScenePrimElementsSource*>(source.get())) {
            usable = viewportOf(_highlightViewer.lock()) != nullptr;
        } else {
            usable = dynamic_cast<const GeoDetailSpreadsheetSource*>(source.get()) && viewportOf(_selectionViewer.lock());
        }
    }
    _selectedOnlyButton->setEnabled(usable);
}

void
GeoSpreadsheetWidget::setSelectedOnly(bool on)
{
    if (on == _selectedOnly) {
        return;
    }
    _selectedOnly = on;
    {
        const QSignalBlocker blocker(_selectedOnlyButton);
        _selectedOnlyButton->setChecked(on);
    }
    rebuildForSelection();
}

void
GeoSpreadsheetWidget::rebuildForSelection()
{
    if (!_model->source()) {
        return;
    }
    requestBuild(wantedPattern(), !_error->text().isEmpty());
}

void
GeoSpreadsheetWidget::cancelPick()
{
    if (_pickCancel) {
        _pickCancel->store(true);
    }
    _pickCancel.reset();
    ++_pickGeneration;
}

void
GeoSpreadsheetWidget::rowsSelected()
{
    if (_writingSelection || _showingSelection) {
        return;
    }
    if (selectedOnlyActive()) {
        return;
    }
    setSelectionNote(QString());
    const GeoSpreadsheetModel::SourcePtr source = _model->source();
    const GeoAttribClassEnum cls = _model->attribClass();
    SceneViewport* viewport = viewportOf(_selectionViewer.lock());

    cancelPick();
    const bool scene = source && dynamic_cast<const ScenePrimElementsSource*>(source.get());
    if (scene) {
        viewport = viewportOf(_highlightViewer.lock());
        if (!viewport || (cls == eGeoAttribClassDetail)) {
            clearHighlight();

            return;
        }
    } else if (!viewport || !source || (cls == eGeoAttribClassDetail) || !dynamic_cast<const GeoDetailSpreadsheetSource*>(source.get())) {
        return;
    }
    std::int64_t rows = 0;
    const RowRanges ranges = mergedRanges(_table->selectionModel()->selection(), &rows);
    const GeoSpreadsheetModel::RowOrderPtr order = _model->rowOrder();
    if (rows <= kInlinePickRows) {
        const std::atomic<bool> never(false);
        const GeoComponentSelection picked = pickRows(source, cls, order, ranges, never).selection;
        if (scene) {
            applyHighlight(picked);
        } else {
            writeSelection(viewport, picked);
        }

        return;
    }
    const std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    const quint64 generation = _pickGeneration;
    _pickCancel = cancel;
    QMetaObject::invokeMethod(_worker, [this, source, cls, order, ranges, cancel, generation]() {
        GeoSpreadsheetPickResult result = pickRows(source, cls, order, ranges, *cancel);
        // The destructor joins this thread before the widget goes, so the widget outlives this call.
        QMetaObject::invokeMethod(this, [this, generation, result = std::move(result)]() { finishPick(generation, result); }, Qt::QueuedConnection); }, Qt::QueuedConnection);
}

void
GeoSpreadsheetWidget::viewerSelectionChanged()
{
    if (_writingSelection) {
        return;
    }
    if (selectedOnlyActive()) {
        rebuildForSelection();

        return;
    }
    syncFromViewer(true, true);
}

void
GeoSpreadsheetWidget::syncFromViewer(bool switchClass,
                                     bool scroll)
{
    if (_showingSelection) {
        return;
    }
    const FlagScope scope(_showingSelection);
    const GeoSpreadsheetModel::SourcePtr source = _model->source();
    const bool scene = source && dynamic_cast<const ScenePrimElementsSource*>(source.get());
    SceneViewport* viewport = viewportOf((scene ? _highlightViewer : _selectionViewer).lock());

    cancelPick();
    if (!viewport || !source || (!scene && !dynamic_cast<const GeoDetailSpreadsheetSource*>(source.get()))) {
        clearRows();

        return;
    }
    const GeoComponentSelection selection = scene ? _highlightSelection : viewport->getComponentSelection();
    if ((selection.type == eGeoComponentObjects) || selection.empty()) {
        clearRows();

        return;
    }
    const GeoAttribClassEnum cls = (selection.type == eGeoComponentPrimitives) ? eGeoAttribClassPrimitive : eGeoAttribClassPoint;
    if (cls != _model->attribClass()) {
        if (!switchClass) {
            clearRows();

            return;
        }
        showClass(cls);
    }
    const bool ordered = (bool)_model->rowOrder();
    const GeoSpreadsheetModel::RowInversePtr inverse = _model->rowInverse();
    if ((std::int64_t)(selection.elements.size() + selection.edges.size()) <= kInlinePickRows) {
        const std::atomic<bool> never(false);
        applyShown(showRows(source, cls, selection, ordered, inverse, never), scroll);

        return;
    }
    const std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    const quint64 generation = _pickGeneration;
    _pickCancel = cancel;
    QMetaObject::invokeMethod(_worker, [this, source, cls, selection, ordered, inverse, cancel, generation, scroll]() {
        GeoSpreadsheetShowResult result = showRows(source, cls, selection, ordered, inverse, *cancel);
        // The destructor joins this thread before the widget goes, so the widget outlives this call.
        QMetaObject::invokeMethod(this, [this, generation, scroll, result = std::move(result)]() { finishShow(generation, result, scroll); }, Qt::QueuedConnection); }, Qt::QueuedConnection);
}

void
GeoSpreadsheetWidget::finishShow(quint64 generation,
                                 const GeoSpreadsheetShowResult& result,
                                 bool scroll)
{
    if ((generation != _pickGeneration) || result.cancelled) {
        return;
    }
    _pickCancel.reset();
    applyShown(result, scroll);
}

void
GeoSpreadsheetWidget::applyShown(const GeoSpreadsheetShowResult& result,
                                 bool scroll)
{
    const FlagScope scope(_showingSelection);
    const int columns = _model->columnCount();
    const int rows = _model->rowCount();
    QItemSelection selection;

    for (const std::pair<int, int>& range : result.ranges) {
        if ((columns > 0) && (range.first >= 0) && (range.second < rows)) {
            selection.append(QItemSelectionRange(_model->index(range.first, 0), _model->index(range.second, columns - 1)));
        }
    }
    if (selection.isEmpty()) {
        _table->selectionModel()->clearSelection();
    } else {
        _table->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        if (scroll) {
            _table->scrollTo(_model->index(selection.first().top(), std::max(0, _table->columnAt(0))), QAbstractItemView::EnsureVisible);
        }
    }
    if (result.selected <= 0) {
        setSelectionNote(QString());
    } else if (result.hidden > 0) {
        setSelectionNote(tr("%1 selected, %2 hidden by filter").arg(result.selected).arg(result.hidden));
    } else {
        setSelectionNote(tr("%1 selected").arg(result.selected));
    }
}

void
GeoSpreadsheetWidget::clearRows()
{
    const FlagScope scope(_showingSelection);
    _table->selectionModel()->clearSelection();
    setSelectionNote(QString());
}

void
GeoSpreadsheetWidget::setSelectionNote(const QString& text)
{
    _selectionNote->setText(text);
    _selectionNote->setVisible(!text.isEmpty());
}

QString
GeoSpreadsheetWidget::getSelectionNote() const
{
    return _selectionNote->text();
}

void
GeoSpreadsheetWidget::finishPick(quint64 generation,
                                 const GeoSpreadsheetPickResult& result)
{
    if ((generation != _pickGeneration) || result.cancelled) {
        return;
    }
    _pickCancel.reset();
    const GeoSpreadsheetModel::SourcePtr source = _model->source();
    if (source && dynamic_cast<const ScenePrimElementsSource*>(source.get())) {
        applyHighlight(result.selection);

        return;
    }
    SceneViewport* viewport = viewportOf(_selectionViewer.lock());
    if (!viewport) {
        return;
    }
    writeSelection(viewport, result.selection);
}

void
GeoSpreadsheetWidget::writeSelection(SceneViewport* viewport,
                                     const GeoComponentSelection& selection)
{
    // The view announces the change it is given, which is not a choice made in the table.
    _writingSelection = true;
    viewport->setPickedComponents(selection);
    _writingSelection = false;
}

qlonglong
GeoSpreadsheetWidget::getShownRows() const
{
    return (qlonglong)_model->rowCount();
}

QString
GeoSpreadsheetWidget::getShownClassName() const
{
    return classLabel(_model->attribClass());
}

qulonglong
GeoSpreadsheetWidget::getCellReads() const
{
    return (qulonglong)_model->cellsRead();
}

bool
GeoSpreadsheetWidget::eventFilter(QObject* watched,
                                  QEvent* e)
{
    if ((watched == _table) && _metricsOwed && ((e->type() == QEvent::Show) || (e->type() == QEvent::Resize))) {
        updateMetrics();
    }

    return QWidget::eventFilter(watched, e);
}

void
GeoSpreadsheetWidget::showClass(GeoAttribClassEnum cls)
{
    if (cls != _model->attribClass()) {
        cancelBuild();
        cancelPick();
        _model->setClass(cls);
        _shownValid = false;
        _sortColumn = -1;
        _sortName.clear();
        _pattern.clear();
        _typed.clear();
        setError(QString());
        updateMetrics();
        populateGroups();
        updateSortIndicator();
        requestBuild(QString());
    }
    updateClassBar();
}

void
GeoSpreadsheetWidget::updateClassBar()
{
    const GeoSpreadsheetModel::SourcePtr& source = _model->source();
    const QLocale locale;

    for (int c = 0; c < kGeoAttribClassCount; ++c) {
        QToolButton* button = _classButtons[(std::size_t)c];
        const GeoAttribClassEnum cls = (GeoAttribClassEnum)c;
        const std::int64_t rows = source ? source->rowCount(cls) : 0;
        button->setText(QString::fromUtf8("%1 %2").arg(classLabel(cls), compactCount(rows)));
        button->setToolTip(tr("%1: %2 rows").arg(classLabel(cls), locale.toString((qlonglong)rows)));
        button->setChecked(cls == _model->attribClass());
        button->setEnabled((bool)source);
    }
}

// Column widths come from the rows in view, so they are measured again once the table has its size.
void
GeoSpreadsheetWidget::updateMetrics()
{
    _metricsOwed = !_table->isVisible() || (_table->viewport()->height() <= 0);
    applyGeoSpreadsheetMetrics(_table);
}

void
GeoSpreadsheetWidget::openFullValue(const QModelIndex& index)
{
    const GeoSpreadsheetModel::SourcePtr source = _model->source();

    if (!source || !index.isValid()) {
        return;
    }
    const GeoAttribClassEnum cls = _model->attribClass();
    const GeoIndex element = _model->elementAt(index.row());
    if ((element < 0) || (element >= source->rowCount(cls)) || (index.column() >= _model->attributeColumnCount())) {
        return;
    }
    const int column = index.column();
    openScenePrimFullValue(_table, [source, cls, column, element](const std::atomic<bool>* /*cancelled*/) {
        return QString::fromStdString(source->fullValue(cls, column, element));
    });
}

QString
GeoSpreadsheetWidget::wantedPattern() const
{
    return _busy ? _pendingPattern : _pattern;
}

void
GeoSpreadsheetWidget::populateGroups()
{
    const GeoSpreadsheetModel::SourcePtr& source = _model->source();
    const GeoAttribClassEnum cls = _model->attribClass();
    QStringList names;

    if (source && (cls != eGeoAttribClassDetail)) {
        if (const GeoDetailSpreadsheetSource* geometry = dynamic_cast<const GeoDetailSpreadsheetSource*>(source.get())) {
            for (const std::string& name : geometry->detail()->groupNames(cls)) {
                names << QString::fromStdString(name);
            }
        } else if (const ScenePrimElementsSource* prim = dynamic_cast<const ScenePrimElementsSource*>(source.get())) {
            for (const ScenePrimElementGroup& group : prim->groups()) {
                if (group.cls == cls) {
                    names << QString::fromStdString(group.name);
                }
            }
        }
    }
    const QSignalBlocker blocker(_group);
    _group->clear();
    _group->addItem(QString());
    _group->addItems(names);
    _group->setEditText(_typed);
    _group->setEnabled(source && (cls != eGeoAttribClassDetail));
}

void
GeoSpreadsheetWidget::updateSortIndicator()
{
    _table->horizontalHeader()->setSortIndicator(_sortColumn, _descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void
GeoSpreadsheetWidget::setError(const QString& text)
{
    _error->setText(text);
    _error->setVisible(!text.isEmpty());
    _group->setToolTip(text.isEmpty() ? groupHelp() : text);
}

void
GeoSpreadsheetWidget::headerClicked(int column)
{
    if (!_model->source() || (column < 0) || (column >= _model->attributeColumnCount())) {
        return;
    }
    if (column == _sortColumn) {
        _descending = !_descending;
    } else {
        _sortColumn = column;
        _sortName = _model->headerData(column, Qt::Horizontal).toString();
        _descending = false;
    }
    updateSortIndicator();
    requestBuild(wantedPattern());
}

void
GeoSpreadsheetWidget::groupSubmitted()
{
    const QString text = _group->currentText();

    if (!_model->source() || (text == _typed)) {
        return;
    }
    _typed = text;
    requestBuild(text);
}

bool
GeoSpreadsheetWidget::shownMatchesWanted() const
{
    return _shownValid && (_shownColumn == _sortColumn) && (_shownDescending == _descending) && (_shownPattern == _pattern)
        && (_shownSelectedOnly == selectedOnlyActive());
}

void
GeoSpreadsheetWidget::cancelBuild()
{
    if (_busy && _cancel) {
        _cancel->store(true);
        ++_sortsCancelled;
    }
    _cancel.reset();
    _busy = false;
    ++_generation;
    _busyLabel->hide();
}

void
GeoSpreadsheetWidget::requestBuild(const QString& pattern,
                                   bool keepError)
{
    cancelBuild();
    if (!_model->source()) {
        return;
    }
    if ((_sortColumn < 0) && pattern.trimmed().isEmpty() && !selectedOnlyActive()) {
        _pattern = pattern;
        if (!keepError) {
            setError(QString());
        }
        applyOrder(GeoSpreadsheetModel::RowOrderPtr());
        _shownValid = true;
        _shownPattern = _pattern;
        _shownColumn = -1;
        _shownDescending = _descending;
        _shownSelectedOnly = false;

        return;
    }
    launchBuild(pattern, keepError);
}

void
GeoSpreadsheetWidget::launchBuild(const QString& pattern,
                                  bool keepError)
{
    const GeoSpreadsheetModel::SourcePtr source = _model->source();
    const GeoAttribClassEnum cls = _model->attribClass();
    const std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    const quint64 generation = ++_generation;
    const std::string text = pattern.toStdString();
    const int column = _sortColumn;
    const bool descending = _descending;
    std::shared_ptr<const GeoComponentSelection> selectedOnly;
    if (selectedOnlyActive()) {
        if (dynamic_cast<const ScenePrimElementsSource*>(source.get())) {
            selectedOnly = std::make_shared<const GeoComponentSelection>(_highlightSelection);
        } else if (SceneViewport* viewport = viewportOf(_selectionViewer.lock())) {
            selectedOnly = std::make_shared<const GeoComponentSelection>(viewport->getComponentSelection());
        }
    }

    _cancel = cancel;
    _busy = true;
    _pendingPattern = pattern;
    ++_sortsStarted;
    _busyLabel->show();
    QMetaObject::invokeMethod(_worker, [this, source, cls, text, column, descending, selectedOnly, cancel, generation, pattern, keepError]() {
        while (_holdSorts.load() && !cancel->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        GeoSpreadsheetBuildResult result = runBuild(source, cls, text, column, descending, selectedOnly.get(), *cancel);
        // The destructor joins this thread before the widget goes, so the widget outlives this call.
        QMetaObject::invokeMethod(this, [this, generation, result, pattern, keepError]() { finishBuild(generation, result, pattern, keepError); }, Qt::QueuedConnection); }, Qt::QueuedConnection);
}

void
GeoSpreadsheetWidget::finishBuild(quint64 generation,
                                  const GeoSpreadsheetBuildResult& result,
                                  const QString& pattern,
                                  bool keepError)
{
    if (generation != _generation) {
        return;
    }
    _busy = false;
    _cancel.reset();
    _busyLabel->hide();
    if (!result.error.isEmpty()) {
        setError(result.error);
        if (!shownMatchesWanted()) {
            // A pattern that cannot be applied leaves the previous filter, or none when the snapshot cannot take it.
            const QString fallback = (pattern == _pattern) ? QString() : _pattern;
            requestBuild(fallback, true);
        }

        return;
    }
    if (!result.order) {
        return;
    }
    _pattern = pattern;
    if (!keepError) {
        setError(QString());
    }
    applyOrder(result.order, result.inverse);
    _shownValid = true;
    _shownPattern = _pattern;
    _shownColumn = _sortColumn;
    _shownDescending = _descending;
    _shownSelectedOnly = selectedOnlyActive();
}

// A model reset drops the column widths, which the user may have set.
void
GeoSpreadsheetWidget::applyOrder(const GeoSpreadsheetModel::RowOrderPtr& order,
                                 const GeoSpreadsheetModel::RowInversePtr& inverse)
{
    cancelPick();
    std::vector<int> widths;
    const int columns = _model->columnCount();

    for (int c = 0; c < columns; ++c) {
        widths.push_back(_table->columnWidth(c));
    }
    _model->setRowOrder(order, inverse);
    if (_model->columnCount() == columns) {
        for (int c = 0; c < columns; ++c) {
            _table->setColumnWidth(c, widths[(std::size_t)c]);
        }
    }
    syncFromViewer(false, false);
}

std::unique_ptr<ScenePrimInspectorTab>
createScenePrimSpreadsheetTab()
{
    return std::make_unique<ScenePrimSpreadsheetTab>();
}

NATRON_NAMESPACE_EXIT
