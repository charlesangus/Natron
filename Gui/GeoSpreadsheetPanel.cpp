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

#include "Gui/GeoSpreadsheetPanel.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include "Engine/Node.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerInstance.h"
#include "Gui/GeoSpreadsheet.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/SceneSelectionActions.h"
#include "Gui/SceneViewport.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

namespace {

const int kGreyAfterMs = 100;

} // anon namespace

struct GeoSpreadsheetPanel::Implementation {
    struct Request {
        NodePtr node;
        ViewerInstancePtr viewer;
        double frame = 0.;
        U64 generation = 0;
    };

    // What the rows shown were read from.
    struct Shown {
        NodeWPtr node;
        double frame = 0.;
        U64 hash = 0;
        bool fromViewer = false;
        bool valid = false;
    };

    GeoSpreadsheetPanel* panel;
    Gui* gui;
    QLabel* nodeLabel = nullptr;
    GeoSpreadsheetWidget* sheet = nullptr;
    QTimer refreshTimer;
    QTimer greyTimer;

    QPointer<ViewerTab> viewer;
    QPointer<SceneViewport> viewport;
    QPointer<TimeLine> viewerTimeLine;

    Shown shown;
    NodeWPtr runningNode;
    double runningFrame = 0.;
    U64 runningHash = 0;
    bool busy = false;
    bool stale = false;
    bool readOwed = false;
    bool rereadWhenDone = false;
    qulonglong readsFinished = 0;

    std::atomic<U64> generation { 0 };
    std::mutex queueMutex;
    std::condition_variable queueChanged;
    bool hasRequest = false;
    bool quit = false;
    Request request;
    std::thread worker;

    Implementation(GeoSpreadsheetPanel* panel,
                   Gui* gui)
        : panel(panel)
        , gui(gui)
    {
    }

    void publishState()
    {
        const NodePtr node = shown.valid ? shown.node.lock() : NodePtr();

        panel->setProperty("natronGeometrySpreadsheetNode", node ? QString::fromStdString(node->getScriptName_mt_safe()) : QString());
        panel->setProperty("natronGeometrySpreadsheetBusy", busy);
        panel->setProperty("natronGeometrySpreadsheetStale", stale);
        panel->setProperty("natronGeometrySpreadsheetReadsFinished", readsFinished);
    }

    void setStale(bool newStale)
    {
        stale = newStale;
        sheet->setStale(newStale);
        publishState();
    }

    void setNodeLabel(const NodePtr& node)
    {
        const QString text = node ? QString::fromStdString(node->getLabel_mt_safe()) : QString();

        nodeLabel->setText(text);
        nodeLabel->setToolTip(text);
    }

    ViewerInstancePtr viewerInstance() const
    {
        if (!viewer || !viewer->getInternalNode()) {
            return ViewerInstancePtr();
        }

        return std::dynamic_pointer_cast<ViewerInstance>(viewer->getInternalNode()->shared_from_this());
    }

    double currentFrame() const
    {
        if (viewer && viewer->getTimeLine()) {
            return viewer->getTimeLine()->currentFrame();
        }
        GuiAppInstancePtr app = gui ? gui->getApp() : GuiAppInstancePtr();

        return (app && app->getTimeLine()) ? app->getTimeLine()->currentFrame() : 0.;
    }

    bool viewerServes(const ViewerInstancePtr& instance,
                      const NodePtr& node,
                      double frame,
                      U64 hash) const
    {
        GeoDetailConstPtr detail;
        int servedFrame = 0;
        NodePtr servedNode;
        U64 servedHash = 0;

        return instance && instance->getShownGeometry(0, &detail, &servedFrame, &servedNode, &servedHash) && detail
            && (double(servedFrame) == frame) && (servedNode == node) && (servedHash == hash);
    }

    void followViewer()
    {
        ViewerTab* newViewer = gui ? SceneSelectionActions::selectionViewer(gui) : nullptr;

        if (newViewer != viewer) {
            if (viewer) {
                QObject::disconnect(viewer, nullptr, panel, nullptr);
            }
            viewer = newViewer;
            if (viewer) {
                // The scene view, and with it its viewport, is made the first time the viewer shows a scene.
                QObject::connect(viewer, &ViewerTab::sceneViewChanged, panel, &GeoSpreadsheetPanel::onActiveViewerChanged);
                QObject::connect(viewer, &ViewerTab::sceneViewedNodeChanged, panel, [this]() { refreshTimer.start(); });
            }
            if (viewerTimeLine) {
                QObject::disconnect(viewerTimeLine, nullptr, panel, nullptr);
            }
            viewerTimeLine = viewer ? viewer->getTimeLine().get() : nullptr;
            if (viewerTimeLine) {
                QObject::connect(viewerTimeLine, &TimeLine::frameChanged, panel, [this]() { refreshTimer.start(); });
            }
        }
        SceneViewport* newViewport = viewer ? viewer->getSceneViewport() : nullptr;
        if (newViewport != viewport) {
            if (viewport) {
                QObject::disconnect(viewport, nullptr, panel, nullptr);
            }
            viewport = newViewport;
            if (viewport) {
                // The viewer evaluates after an edit, and syncs its view once it has.
                QObject::connect(viewport, &SceneViewport::sceneSynced, panel, [this]() { refreshTimer.start(); });
            }
        }
        refreshTimer.start();
    }

    void cancelRead()
    {
        ++generation;
        rereadWhenDone = false;
        runningNode.reset();
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            hasRequest = false;
            request = Request();
        }
        queueChanged.notify_all();
    }

    void showNothing(const QString& text)
    {
        cancelRead();
        busy = false;
        greyTimer.stop();
        shown = Shown();
        setNodeLabel(NodePtr());
        sheet->setHighlightTarget(ViewerInstanceWPtr(), QString(), NodeWPtr());
        sheet->setSelectionViewer(ViewerInstanceWPtr());
        sheet->showMessage(text);
        setStale(false);
    }

    void refresh()
    {
        if (!panel->isVisible()) {
            readOwed = true;

            return;
        }
        readOwed = false;
        const NodePtr node = viewer ? viewer->getSceneViewedNode() : NodePtr();
        if (!geoSpreadsheetListsNode(node)) {
            showNothing(viewer ? GeoSpreadsheetPanel::tr("The viewer shows no native geometry") : GeoSpreadsheetPanel::tr("No viewer"));

            return;
        }
        // A read is let finish while the frame changes or the node is edited, or one replaced on
        // every frame of playback would never show anything.
        if (busy && (runningNode.lock() == node)) {
            rereadWhenDone = true;

            return;
        }
        const ViewerInstancePtr instance = viewerInstance();
        const double frame = currentFrame();
        const U64 hash = node->getHashValue();
        if (!busy && shown.valid && (shown.node.lock() == node) && (shown.frame == frame) && (shown.hash == hash)
            && (shown.fromViewer || !viewerServes(instance, node, frame, hash))) {
            return;
        }
        cancelRead();
        runningNode = node;
        runningFrame = frame;
        runningHash = hash;
        busy = true;
        if (!stale && !greyTimer.isActive()) {
            greyTimer.start();
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            request.node = node;
            request.viewer = instance;
            request.frame = frame;
            request.generation = generation;
            hasRequest = true;
        }
        queueChanged.notify_one();
        publishState();
    }

    void onReadEnded(U64 readGeneration,
                     const GeoSpreadsheetNodeRead& read)
    {
        if ((readGeneration != generation) || read.cancelled) {
            return;
        }
        const NodePtr node = runningNode.lock();
        runningNode.reset();
        busy = false;
        greyTimer.stop();
        ++readsFinished;
        sheet->setHighlightTarget(ViewerInstanceWPtr(), QString(), NodeWPtr());
        if (read.source) {
            sheet->setSelectionViewer(read.selectionViewer);
            sheet->setSnapshot(read.source);
        } else {
            sheet->setSelectionViewer(ViewerInstanceWPtr());
            sheet->showMessage(read.error);
        }
        shown.node = node;
        shown.frame = runningFrame;
        shown.hash = runningHash;
        shown.fromViewer = read.fromViewer;
        shown.valid = bool(node);
        setNodeLabel(node);
        setStale(false);
        if (rereadWhenDone) {
            rereadWhenDone = false;
            refreshTimer.start();
        }
    }

    void run()
    {
        for (;;) {
            Request next;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueChanged.wait(lock, [this]() { return quit || hasRequest; });
                if (quit) {
                    return;
                }
                next = std::move(request);
                request = Request();
                hasRequest = false;
            }
            const U64 readGeneration = next.generation;
            GeoSpreadsheetNodeRead read;
            if (readGeneration == generation) {
                read = geoSpreadsheetReadNode(next.node, next.viewer, next.frame, [this, readGeneration]() { return readGeneration != generation; });
            } else {
                read.cancelled = true;
            }
            next = Request();
            QMetaObject::invokeMethod(panel, [this, readGeneration, read]() { onReadEnded(readGeneration, read); }, Qt::QueuedConnection);
        }
    }

    void stop()
    {
        ++generation;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            quit = true;
        }
        queueChanged.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }
};

GeoSpreadsheetPanel::GeoSpreadsheetPanel(Gui* gui)
    : QWidget(gui)
    , PanelWidget(this, gui)
    , _imp(new Implementation(this, gui))
{
    setObjectName(QString::fromUtf8("geometrySpreadsheetPanel"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    _imp->nodeLabel = new QLabel(this);
    _imp->nodeLabel->setObjectName(QString::fromUtf8("geometrySpreadsheetNode"));
    _imp->nodeLabel->setContentsMargins(4, 2, 4, 0);
    // A long node label must not set the panel's minimum width.
    _imp->nodeLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(_imp->nodeLabel);

    _imp->sheet = new GeoSpreadsheetWidget(QString::fromUtf8("geometrySpreadsheet"), this);
    layout->addWidget(_imp->sheet, 1);

    _imp->refreshTimer.setSingleShot(true);
    _imp->refreshTimer.setInterval(0);
    QObject::connect(&_imp->refreshTimer, &QTimer::timeout, this, [this]() { _imp->refresh(); });
    _imp->greyTimer.setSingleShot(true);
    _imp->greyTimer.setInterval(kGreyAfterMs);
    QObject::connect(&_imp->greyTimer, &QTimer::timeout, this, [this]() {
        if (_imp->busy) {
            _imp->setStale(true);
        }
    });
    if (gui) {
        QObject::connect(gui, &Gui::activeViewerChanged, this, &GeoSpreadsheetPanel::onActiveViewerChanged);
        QObject::connect(gui, &Gui::viewersChanged, this, &GeoSpreadsheetPanel::onActiveViewerChanged);
    }
    _imp->publishState();
    _imp->worker = std::thread([this]() { _imp->run(); });
    _imp->followViewer();
}

GeoSpreadsheetPanel::~GeoSpreadsheetPanel()
{
    _imp->stop();
}

void
GeoSpreadsheetPanel::releaseNodes()
{
    _imp->cancelRead();
    _imp->busy = false;
    _imp->greyTimer.stop();
    _imp->shown = Implementation::Shown();
    _imp->setNodeLabel(NodePtr());
    _imp->sheet->setSelectionViewer(ViewerInstanceWPtr());
    _imp->sheet->setSnapshot(GeoSpreadsheetModel::SourcePtr());
    _imp->setStale(false);
}

void
GeoSpreadsheetPanel::onActiveViewerChanged()
{
    _imp->followViewer();
}

void
GeoSpreadsheetPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (_imp->readOwed) {
        _imp->refreshTimer.start();
    }
}

void
GeoSpreadsheetPanel::enterEvent(QEnterEvent* e)
{
    enterEventBase();
    QWidget::enterEvent(e);
}

void
GeoSpreadsheetPanel::leaveEvent(QEvent* e)
{
    leaveEventBase();
    QWidget::leaveEvent(e);
}

void
GeoSpreadsheetPanel::keyPressEvent(QKeyEvent* e)
{
    handleUnCaughtKeyPressEvent(e);
    QWidget::keyPressEvent(e);
}

void
GeoSpreadsheetPanel::keyReleaseEvent(QKeyEvent* e)
{
    handleUnCaughtKeyUpEvent(e);
    QWidget::keyReleaseEvent(e);
}

void
GeoSpreadsheetPanel::mousePressEvent(QMouseEvent* e)
{
    takeClickFocus();
    QWidget::mousePressEvent(e);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_GeoSpreadsheetPanel.cpp"
