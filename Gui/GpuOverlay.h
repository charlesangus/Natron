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

#ifndef Gui_GpuOverlay_h
#define Gui_GpuOverlay_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QGraphicsObject>
#include <QLineF>
#include <QPainterPath>
#include <QString>
#include <QStringList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"
#include "Engine/RenderScheduler.h"
#include "Engine/ViewerInstance.h"

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One layer of the GPU region overlay: the outlines below the pipes, or the pipe markers and labels above them.
 **/
class GpuRegionItem
    : public QGraphicsObject {
public:
    enum LayerEnum {
        eLayerOutlines,
        eLayerMarkers
    };

    struct Outline {
        QPainterPath path;
        bool fellBack = false;
        QString label;
        QRectF labelRect;
        QRectF bounds;
    };

    struct Marker {
        QLineF pipe;
        QPointF at;
        bool exit = false;
        QString label;
        QRectF labelRect;
        QRectF bounds;
    };

    struct Tag {
        QPointF at;
        QString text;
        QRectF bounds;
    };

    // What the item draws of one region, in scene coordinates.
    struct Shapes {
        std::vector<Outline> outlines;
        std::vector<Marker> markers;
        std::vector<Tag> tags;
    };

    GpuRegionItem(NodeGraph* graph,
                  LayerEnum layer,
                  QGraphicsItem* parent);

    void setAllShapes(std::vector<Shapes> regions);

    // Replaces the shapes of one region of the last setAllShapes(), repainting only where they were and are.
    void setShapes(std::size_t region, Shapes shapes);

    virtual QRectF boundingRect() const OVERRIDE FINAL;
    virtual void paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) OVERRIDE FINAL;

private:
    struct Slot {
        Shapes shapes;
        QRectF bounds;
    };

    void place(Slot* slot);
    void refreshBounds();

    NodeGraph* _graph;
    LayerEnum _layer;
    std::vector<Slot> _slots;
    QRectF _bounds;
};

/**
 * @brief Draws the active viewer's GPU placement snapshot over one node graph while the Show GPU Regions preference is on.
 **/
class GpuRegionOverlay
    : public QObject {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    GpuRegionOverlay(NodeGraph* graph,
                     QGraphicsItem* nodeRoot);

    virtual ~GpuRegionOverlay() OVERRIDE;

    /**
     * @brief Schedules a redraw; topology also maps the snapshot onto the graph's nodes again. Does nothing while off.
     **/
    void schedule(bool topology);

    /**
     * @brief Call when node, or one of its edges, may have moved, changed shape or been shown or hidden: the regions
     * drawn near it are redrawn once control returns to the event loop.
     **/
    void noteNodeChanged(NodeGui* node);

    /**
     * @brief The node of collection that shows node: node itself, the Group node of collection enclosing it, or null when
     * node is not under collection.
     **/
    static NodePtr mapNodeToCollection(const NodePtr& node, const NodeCollectionPtr& collection);

    /**
     * @brief The edge of collection that carries the flattened graph's pipe into input inputNb of dest, as the node it
     * enters and that node's input. Group, Input and Output nodes, which the flattened graph skips, are walked through.
     * False when the pipe does not pass through collection.
     **/
    static bool mapPipeToCollection(const NodePtr& dest,
                                    int inputNb,
                                    const NodeCollectionPtr& collection,
                                    NodePtr* mappedDest,
                                    int* mappedInputNb);

    static QString exitLabel(FrameGraph::PlacementReason reason, const std::string& detail, bool mixed);

    static QString fallbackLabel(const GpuPlacementSnapshot::Region& region);

public Q_SLOTS:
    void refresh();

private Q_SLOTS:
    void onSettingChanged(KnobI* knob);
    void onViewersChanged();
    void onSnapshotChanged();
    void refreshChangedRegions();

private:
    struct MappedPipe {
        NodeGuiWPtr dest;
        int inputNb = -1;
        QString label;
    };

    struct MappedMember {
        NodeGuiWPtr gui;
        bool mixed = false;
    };

    struct MappedRun {
        NodeGuiWPtr gui;
        bool ran = false;
    };

    struct MappedRegion {
        std::vector<MappedMember> members;
        std::vector<MappedPipe> exits;
        std::vector<MappedPipe> entries;
        bool fellBack = false;
        QString fallbackLabel;
    };

    // What one region draws, and how much of it, for the counts the outline item publishes.
    struct RegionDraw {
        GpuRegionItem::Shapes outlines;
        GpuRegionItem::Shapes markers;
        bool outlined = false;
        int exitCount = 0;
        int entryCount = 0;
        int mixedCount = 0;
        QStringList exitLabels;
    };

    bool isEnabled() const;
    ViewerInstance* currentViewer() const;
    void watchViewer(ViewerInstance* viewer);
    void remap();
    void indexRegions();
    RegionDraw drawRegion(const MappedRegion& region) const;
    void publishCounts();
    void clear();
    void setFaded(const std::vector<NodeGuiWPtr>& faded);

    NodeGraph* _graph;
    QPointer<GpuRegionItem> _outlines;
    QPointer<GpuRegionItem> _markers;
    QPointer<QObject> _watchedViewer;
    GpuPlacementSnapshotPtr _snapshot;
    std::vector<MappedRegion> _regions;
    std::vector<MappedRun> _runs;
    std::vector<NodeGuiWPtr> _faded;
    std::unordered_set<NodeGui*> _fadedSet;
    QStringList _fadedNames;
    std::vector<RegionDraw> _draws;

    // The regions each node is drawn in or next to, as a member or an end of a marked pipe.
    std::unordered_map<NodeGui*, std::vector<std::size_t>> _regionsOfNode;
    std::unordered_set<std::size_t> _changedRegions;
    bool _pending;
    bool _remapPending;
    bool _changesPending;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_GpuOverlay_h
