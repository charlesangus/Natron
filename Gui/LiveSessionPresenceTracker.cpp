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

#include "LiveSessionPresenceTracker.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <vector>

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QStatusBar>

#include "Engine/AppInstance.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/KnobTypes.h"
#include "Engine/LiveSession/SessionPresence.h"
#include "Engine/LiveSession/SessionRemoteEdits.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/TimeLine.h"
#include "Engine/UndoRecorder.h"
#include "Engine/ViewerInstance.h"

#include "Gui/DockablePanel.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/TabWidget.h"
#include "Gui/UndoHistory.h"
#include "Gui/ViewerGL.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

namespace {

LiveSessionUserMark
markOf(const SessionPresence::Entry& entry)
{
    LiveSessionUserMark mark;
    mark.name = QString::fromStdString(entry.name);
    mark.colour = QColor::fromRgbF(entry.colour.r, entry.colour.g, entry.colour.b);

    return mark;
}

std::string
viewerNodeIdOf(ViewerTab* tab)
{
    ViewerInstance* viewer = tab ? tab->getInternalNode() : nullptr;
    NodePtr node = viewer ? viewer->getNode() : NodePtr();

    return node ? node->getNodeId() : std::string();
}

// In the order of the display channels knob's entries.
const char* const kChannelsNames[] = { "Luminance", "RGB", "R", "G", "B", "A", "Matte" };

std::string
channelsNameAt(int index)
{
    return ((index >= 0) && (index < (int)std::size(kChannelsNames))) ? kChannelsNames[index] : std::string();
}

int
channelsIndexOf(const std::string& name)
{
    for (int i = 0; i < (int)std::size(kChannelsNames); ++i) {
        if (name == kChannelsNames[i]) {
            return i;
        }
    }

    return -1;
}

template <typename K>
std::shared_ptr<K>
viewerKnobOf(ViewerTab* tab,
             const char* name)
{
    ViewerInstance* viewer = tab ? tab->getInternalNode() : nullptr;

    return viewer ? std::dynamic_pointer_cast<K>(viewer->getKnobByName(name)) : std::shared_ptr<K>();
}

int
inputAOf(ViewerTab* tab)
{
    int a = -1, b = -1;
    tab->getActiveInputs(&a, &b);

    return a;
}

std::string
channelsOf(ViewerTab* tab)
{
    KnobChoicePtr channels = viewerKnobOf<KnobChoice>(tab, kViewerParamDisplayChannels);

    return channels ? channelsNameAt(channels->getValue()) : tab->getChannelsString();
}

bool
showsInputsOf(ViewerTab* tab,
              const SessionPresence::ViewerState& state)
{
    return (inputAOf(tab) == state.inputIndex) && (channelsOf(tab) == state.channels);
}

// Writes the leader's input A and channels into this window's copy of the viewer node.
void
applyViewerInputs(ViewerTab* tab,
                  const SessionPresence::ViewerState& state)
{
    KnobIntPtr input = viewerKnobOf<KnobInt>(tab, kViewerParamAInput);
    if (input && (state.inputIndex >= input->getMinimum()) && (state.inputIndex <= input->getMaximum()) && (input->getValue() != state.inputIndex)) {
        input->setValue(state.inputIndex, ViewSpec::all(), 0);
    }
    KnobChoicePtr channels = viewerKnobOf<KnobChoice>(tab, kViewerParamDisplayChannels);
    const int channelsIndex = channelsIndexOf(state.channels);
    if (channels && (channelsIndex != -1) && (channels->getValue() != channelsIndex)) {
        channels->setValue(channelsIndex, ViewSpec::all(), 0);
    }
}

// Modifier presses alone are forwarded to a viewer whatever has the focus, for overlays to track.
bool
isViewerInteraction(QEvent* e)
{
    switch (e->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:

        return true;
    case QEvent::KeyPress: {
        const int key = static_cast<QKeyEvent*>(e)->key();

        return (key != Qt::Key_Control) && (key != Qt::Key_Shift) && (key != Qt::Key_Alt) && (key != Qt::Key_Meta) && (key != Qt::Key_AltGr);
    }
    default:

        return false;
    }
}

bool
isInsideViewer(QObject* watched,
               const std::list<ViewerTab*>& viewers)
{
    QWidget* widget = qobject_cast<QWidget*>(watched);
    if (!widget) {
        return false;
    }
    for (ViewerTab* tab : viewers) {
        if (tab && ((tab == widget) || tab->isAncestorOf(widget))) {
            return true;
        }
    }

    return false;
}

void
applyLeaderState(const TimeLinePtr& timeline,
                 ViewerTab* tab,
                 const SessionPresence::ViewerState& state)
{
    if (timeline && (timeline->currentFrame() != state.frame)) {
        timeline->seekFrame(state.frame, false, nullptr, eTimelineChangeReasonOtherSeek);
    }
    applyViewerInputs(tab, state);
    ViewerGL* gl = tab->getViewer();
    if (gl && (state.zoomFactor > 0.) && (state.zoomAspectRatio > 0.)) {
        double left = 0., bottom = 0., zoom = 1., aspect = 1.;
        gl->getProjection(&left, &bottom, &zoom, &aspect);
        const RectD visible = gl->getViewportRect();
        // Same widget, other zoom: the visible extent scales by the zoom ratio.
        const double halfWidth = (visible.x2 - visible.x1) / 2. * zoom / state.zoomFactor;
        const double halfHeight = (visible.y2 - visible.y1) / 2. * (zoom * aspect) / (state.zoomFactor * state.zoomAspectRatio);
        tab->setProjection(state.centerX - halfWidth, state.centerY - halfHeight, state.zoomFactor, state.zoomAspectRatio);
        gl->update();
    }
}

} // namespace

LiveSessionPresenceTracker::LiveSessionPresenceTracker(Gui* gui)
    : QObject(gui)
    , _gui(gui)
{
    _localTimer.setSingleShot(true);
    _localTimer.setInterval(0);
    QObject::connect(&_localTimer, &QTimer::timeout, this, &LiveSessionPresenceTracker::sendLocal);
    _viewerTimer.setInterval(SessionPresence::kDefaultThrottleMs);
    QObject::connect(&_viewerTimer, &QTimer::timeout, this, &LiveSessionPresenceTracker::sendLocalViewer);

    GuiAppInstancePtr app = gui->getApp();
    if (app) {
        AppInstance* engineApp = app.get();
        QObject::connect(engineApp, &AppInstance::liveSessionChanged, this, &LiveSessionPresenceTracker::onSessionChanged);
        QObject::connect(engineApp, &AppInstance::liveSessionUsersChanged, this, &LiveSessionPresenceTracker::onUsersChanged);
        QObject::connect(engineApp, &AppInstance::liveSessionPresenceChanged, this, &LiveSessionPresenceTracker::onPresenceChanged);
        QObject::connect(engineApp, &AppInstance::liveSessionViewerChanged, this, &LiveSessionPresenceTracker::onViewerChanged);
        QObject::connect(engineApp, &AppInstance::liveSessionRemoteEdit, this, &LiveSessionPresenceTracker::onRemoteEdit);
        TimeLinePtr timeline = app->getTimeLine();
        if (timeline) {
            QObject::connect(timeline.get(), &TimeLine::frameChanged, this, &LiveSessionPresenceTracker::onTimelineFrameChanged);
        }
    }
    if (gui->getUndoHistory()) {
        QObject::connect(gui->getUndoHistory(), &UndoHistory::replayedOverRemoteEdit, this, &LiveSessionPresenceTracker::onReplayedOverRemoteEdit);
    }
}

LiveSessionPresenceTracker::~LiveSessionPresenceTracker()
{
    if (_leader) {
        qApp->removeEventFilter(this);
    }
    delete _leaderLabel.data();
}

SessionPresence*
LiveSessionPresenceTracker::presence() const
{
    GuiAppInstancePtr app = _gui->getApp();

    return app ? app->getLiveSessionPresence() : nullptr;
}

ViewerTab*
LiveSessionPresenceTracker::localViewer() const
{
    ViewerTab* active = _gui->getActiveViewer();
    if (active) {
        return active;
    }
    const std::list<ViewerTab*>& viewers = _gui->getViewersList();

    return viewers.empty() ? nullptr : viewers.front();
}

void
LiveSessionPresenceTracker::scheduleLocalUpdate()
{
    if (presence() && !_localTimer.isActive()) {
        _localTimer.start();
    }
}

void
LiveSessionPresenceTracker::sendLocal()
{
    SessionPresence* table = presence();
    if (!table || !table->hasLocalUser()) {
        return;
    }
    GuiAppInstancePtr app = _gui->getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (!project) {
        return;
    }
    std::vector<std::string> selection;
    NodesList nodes;
    project->getNodes_recursive(nodes, true);
    for (const NodePtr& node : nodes) {
        NodeGuiPtr nodeGui = std::dynamic_pointer_cast<NodeGui>(node->getNodeGui());
        if (nodeGui && nodeGui->getIsSelected()) {
            selection.push_back(node->getNodeId());
        }
    }
    std::sort(selection.begin(), selection.end());

    std::vector<std::string> panels;
    for (DockablePanel* panel : _gui->getVisiblePanels()) {
        NodeSettingsPanel* settings = dynamic_cast<NodeSettingsPanel*>(panel);
        NodeGuiPtr nodeGui = settings ? settings->getNode() : NodeGuiPtr();
        NodePtr node = nodeGui ? nodeGui->getNode() : NodePtr();
        if (node) {
            panels.push_back(node->getNodeId());
        }
    }
    std::sort(panels.begin(), panels.end());
    panels.erase(std::unique(panels.begin(), panels.end()), panels.end());

    table->setLocalSelection(selection);
    table->setLocalOpenPanels(panels);

    // A panel opened after another user's mark arrived has no dot yet.
    for (const std::pair<const std::string, LiveSessionUserMarks>& marks : _panelMarks) {
        applyPanelMarks(marks.first);
    }
}

void
LiveSessionPresenceTracker::sendLocalViewer()
{
    SessionPresence* table = presence();
    if (!table) {
        return;
    }
    ViewerTab* tab = localViewer();
    ViewerGL* gl = tab ? tab->getViewer() : nullptr;
    GuiAppInstancePtr app = _gui->getApp();
    TimeLinePtr timeline = app ? app->getTimeLine() : TimeLinePtr();
    if (!gl || !timeline) {
        table->setLocalViewer(std::nullopt);

        return;
    }
    SessionPresence::ViewerState state;
    state.frame = timeline->currentFrame();
    state.viewerNodeId = viewerNodeIdOf(tab);
    state.inputIndex = inputAOf(tab);
    double left = 0., bottom = 0.;
    gl->getProjection(&left, &bottom, &state.zoomFactor, &state.zoomAspectRatio);
    const RectD visible = gl->getViewportRect();
    state.centerX = (visible.x1 + visible.x2) / 2.;
    state.centerY = (visible.y1 + visible.y2) / 2.;
    state.channels = channelsOf(tab);
    gl->setProperty("natronLiveSessionZoom", state.zoomFactor);
    gl->setProperty("natronLiveSessionCenterX", state.centerX);
    gl->setProperty("natronLiveSessionCenterY", state.centerY);
    ViewerInstance* viewer = tab->getInternalNode();
    NodePtr viewerNode = viewer ? viewer->getNode() : NodePtr();
    gl->setProperty("natronLiveSessionViewer", viewerNode ? QString::fromStdString(viewerNode->getScriptName()) : QString());
    table->setLocalViewer(state);
}

void
LiveSessionPresenceTracker::onSessionChanged()
{
    GuiAppInstancePtr app = _gui->getApp();
    const bool inSession = app && (app->getLiveSessionState() != AppInstance::LiveSessionState::eNone);
    if (inSession) {
        if (!_viewerTimer.isActive()) {
            _viewerTimer.start();
        }
        scheduleLocalUpdate();
    } else {
        _viewerTimer.stop();
        _localTimer.stop();
        stopFollowing();
    }
    // The host's table can be emptied without a signal reaching this window.
    refreshMarks();
}

void
LiveSessionPresenceTracker::onUsersChanged()
{
    if (_leader) {
        SessionPresence* table = presence();
        if (!table || !table->user(*_leader)) {
            stopFollowing(tr("Stopped following: the user left the session"));
        }
    }
    refreshMarks();
}

void
LiveSessionPresenceTracker::onPresenceChanged(quint32 userId)
{
    SessionPresence* table = presence();
    if (table && table->hasLocalUser() && (table->localUserId() == userId)) {
        return;
    }
    refreshMarks();
}

void
LiveSessionPresenceTracker::refreshMarks()
{
    std::map<std::string, LiveSessionUserMarks> selection;
    std::map<std::string, LiveSessionUserMarks> panels;
    SessionPresence* table = presence();
    if (table) {
        for (const SessionPresence::Entry& user : table->users()) {
            if (table->hasLocalUser() && (user.userId == table->localUserId())) {
                continue;
            }
            const LiveSessionUserMark mark = markOf(user);
            for (const std::string& id : user.selection) {
                selection[id].push_back(mark);
            }
            for (const std::string& id : user.openPanels) {
                panels[id].push_back(mark);
            }
        }
    }
    std::set<std::string> touched;
    for (const std::pair<const std::string, LiveSessionUserMarks>& marks : _selectionMarks) {
        std::map<std::string, LiveSessionUserMarks>::const_iterator now = selection.find(marks.first);
        if ((now == selection.end()) || !(now->second == marks.second)) {
            touched.insert(marks.first);
        }
    }
    for (const std::pair<const std::string, LiveSessionUserMarks>& marks : selection) {
        if (!_selectionMarks.count(marks.first)) {
            touched.insert(marks.first);
        }
    }
    std::set<std::string> touchedPanels;
    for (const std::pair<const std::string, LiveSessionUserMarks>& marks : _panelMarks) {
        std::map<std::string, LiveSessionUserMarks>::const_iterator now = panels.find(marks.first);
        if ((now == panels.end()) || !(now->second == marks.second)) {
            touchedPanels.insert(marks.first);
        }
    }
    for (const std::pair<const std::string, LiveSessionUserMarks>& marks : panels) {
        if (!_panelMarks.count(marks.first)) {
            touchedPanels.insert(marks.first);
        }
    }
    _selectionMarks.swap(selection);
    _panelMarks.swap(panels);
    for (const std::string& id : touched) {
        applySelectionMarks(id);
    }
    for (const std::string& id : touchedPanels) {
        applyPanelMarks(id);
    }
}

void
LiveSessionPresenceTracker::applySelectionMarks(const std::string& nodeId)
{
    GuiAppInstancePtr app = _gui->getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    NodePtr node = project ? project->findNodeById(nodeId) : NodePtr();
    NodeGuiPtr nodeGui = node ? std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()) : NodeGuiPtr();
    if (!nodeGui) {
        return;
    }
    std::map<std::string, LiveSessionUserMarks>::const_iterator found = _selectionMarks.find(nodeId);
    nodeGui->setRemoteUsers((found == _selectionMarks.end()) ? LiveSessionUserMarks() : found->second);
}

void
LiveSessionPresenceTracker::applyPanelMarks(const std::string& nodeId)
{
    GuiAppInstancePtr app = _gui->getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    NodePtr node = project ? project->findNodeById(nodeId) : NodePtr();
    NodeGuiPtr nodeGui = node ? std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()) : NodeGuiPtr();
    NodeSettingsPanel* panel = nodeGui ? nodeGui->getSettingPanel() : nullptr;
    if (!panel) {
        return;
    }
    std::map<std::string, LiveSessionUserMarks>::const_iterator found = _panelMarks.find(nodeId);
    panel->setRemoteUsers((found == _panelMarks.end()) ? LiveSessionUserMarks() : found->second);
}

void
LiveSessionPresenceTracker::onRemoteEdit(const QStringList& deleted,
                                         const QStringList& edited)
{
    UndoHistory* history = _gui->getUndoHistory();
    if (history) {
        history->dropEntriesTouchingNodes(deleted);
        history->noteRemoteEditOfNodes(edited);
    }
    // A node created or recreated remotely has a new NodeGui without the marks of its ID.
    for (const QString& id : edited) {
        applySelectionMarks(id.toStdString());
        applyPanelMarks(id.toStdString());
    }
    if (!deleted.isEmpty()) {
        scheduleLocalUpdate();
    }
}

void
LiveSessionPresenceTracker::onReplayedOverRemoteEdit(const QString& text,
                                                     bool undone)
{
    showNote(undone ? tr("Undid \"%1\" over another user's later edit of the same node").arg(text)
                    : tr("Redid \"%1\" over another user's later edit of the same node").arg(text));
}

void
LiveSessionPresenceTracker::follow(quint32 userId)
{
    SessionPresence* table = presence();
    if (!table || !table->user(userId) || (table->hasLocalUser() && (table->localUserId() == userId))) {
        return;
    }
    if (!_leader) {
        qApp->installEventFilter(this);
    }
    _leader = userId;
    showNote(tr("Following %1").arg(QString::fromStdString(table->user(userId)->name)));
    applyLeaderViewer();
    Q_EMIT followChanged();
}

void
LiveSessionPresenceTracker::stopFollowing(const QString& note)
{
    if (!_leader) {
        return;
    }
    _leader.reset();
    qApp->removeEventFilter(this);
    unwatchFollowedViewer();
    delete _leaderLabel.data();
    if (!note.isEmpty()) {
        showNote(note);
    }
    Q_EMIT followChanged();
}

std::optional<quint32>
LiveSessionPresenceTracker::getLeader() const
{
    return _leader;
}

void
LiveSessionPresenceTracker::onViewerChanged(quint32 userId)
{
    if (_leader && (*_leader == userId)) {
        applyLeaderViewer();
    }
}

void
LiveSessionPresenceTracker::applyLeaderViewer()
{
    SessionPresence* table = presence();
    const SessionPresence::Entry* leader = (table && _leader) ? table->user(*_leader) : nullptr;
    if (!leader) {
        return;
    }
    ViewerTab* tab = nullptr;
    if (leader->viewer) {
        for (ViewerTab* candidate : _gui->getViewersList()) {
            if (viewerNodeIdOf(candidate) == leader->viewer->viewerNodeId) {
                tab = candidate;
                break;
            }
        }
    }
    if (!tab) {
        tab = localViewer();
    }
    if (!tab) {
        return;
    }
    GuiAppInstancePtr app = _gui->getApp();
    if (!app) {
        return;
    }
    const std::optional<SessionPresence::ViewerState> state = leader->viewer;
    QPointer<ViewerTab> guard(tab);
    const bool wasApplying = _applyingLeader;
    _applyingLeader = true;
    {
        // The leader's viewer is shown here only: no graph operation, no undo entry.
        LiveSession::RemoteApply remote;
        GraphEditRecorder::Suppress suppress;
        UndoSuppressGuard noUndo(app);
        TabWidget* pane = tab->getParentPane();
        if (pane && (pane->currentWidget() != tab)) {
            pane->setCurrentWidget(tab);
        }
        _gui->setActiveViewer(tab);
        if (state && guard) {
            applyLeaderState(app->getTimeLine(), tab, *state);
        }
    }
    _applyingLeader = wasApplying;
    table = presence();
    leader = (table && _leader) ? table->user(*_leader) : nullptr;
    if (!leader || !guard) {
        return;
    }
    showLeaderLabel(tab, QString::fromStdString(leader->name));
    watchFollowedViewer(tab);
}

void
LiveSessionPresenceTracker::watchFollowedViewer(ViewerTab* tab)
{
    if (_followedTab == tab) {
        return;
    }
    unwatchFollowedViewer();
    _followedTab = tab;
    const std::pair<const char*, bool> knobs[] = {
        { kViewerParamAInput, true },
        { kViewerParamDisplayChannels, true },
        { kViewerParamBInput, false },
        { kViewerParamDisplayChannelsB, false },
    };
    for (const std::pair<const char*, bool>& knob : knobs) {
        KnobIPtr found = viewerKnobOf<KnobI>(tab, knob.first);
        KnobSignalSlotHandlerPtr handler = found ? found->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
        if (handler) {
            const bool mirrored = knob.second;
            _followedTabKnobs.push_back(QObject::connect(handler.get(), &KnobSignalSlotHandler::valueChanged, this, [this, mirrored]() {
                onFollowedViewerKnobChanged(mirrored);
            }));
        }
    }
}

void
LiveSessionPresenceTracker::unwatchFollowedViewer()
{
    for (const QMetaObject::Connection& connection : _followedTabKnobs) {
        QObject::disconnect(connection);
    }
    _followedTabKnobs.clear();
    _followedTab.clear();
}

void
LiveSessionPresenceTracker::onFollowedViewerKnobChanged(bool mirrored)
{
    if (!_leader || _applyingLeader || LiveSession::RemoteApply::isApplying() || !_followedTab) {
        return;
    }
    if (mirrored) {
        // A change that leaves this viewer agreeing with the leader is no departure from it, such as
        // the switch to A both viewers make for a single-channel layer.
        SessionPresence* table = presence();
        const SessionPresence::Entry* leader = table ? table->user(*_leader) : nullptr;
        if (leader && leader->viewer && showsInputsOf(_followedTab, *leader->viewer)) {
            return;
        }
    }
    stopFollowing(tr("Stopped following: the viewer was used here"));
}

void
LiveSessionPresenceTracker::showLeaderLabel(ViewerTab* tab,
                                            const QString& name)
{
    ViewerGL* gl = tab->getViewer();
    if (!gl) {
        return;
    }
    if (_leaderLabel && (_leaderLabel->parentWidget() != gl)) {
        delete _leaderLabel.data();
    }
    if (!_leaderLabel) {
        _leaderLabel = new QLabel(gl);
        _leaderLabel->setObjectName(QString::fromUtf8("liveSessionFollowLabel"));
        _leaderLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        _leaderLabel->setStyleSheet(QString::fromUtf8("color: white; background-color: rgba(0, 0, 0, 140); padding: 2px 6px;"));
    }
    _leaderLabel->setText(tr("Following %1").arg(name));
    _leaderLabel->adjustSize();
    _leaderLabel->move(gl->width() - _leaderLabel->width() - 8, 8);
    _leaderLabel->show();
    _leaderLabel->raise();
}

void
LiveSessionPresenceTracker::frameSelectionOf(quint32 userId)
{
    SessionPresence* table = presence();
    const SessionPresence::Entry* user = table ? table->user(userId) : nullptr;
    GuiAppInstancePtr app = _gui->getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    NodeGraph* graph = _gui->getNodeGraph();
    if (!user || !project || !graph) {
        return;
    }
    for (const std::string& id : user->selection) {
        NodePtr node = project->findNodeById(id);
        NodeGuiPtr nodeGui = node ? std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()) : NodeGuiPtr();
        if (nodeGui) {
            graph->centerOnItem(nodeGui.get());

            return;
        }
    }
}

void
LiveSessionPresenceTracker::onTimelineFrameChanged(SequenceTime /*time*/,
                                                   int reason)
{
    if (!_leader || _applyingLeader || (reason == eTimelineChangeReasonOtherSeek)) {
        return;
    }
    stopFollowing(tr("Stopped following: the timeline moved here"));
}

bool
LiveSessionPresenceTracker::eventFilter(QObject* watched,
                                        QEvent* e)
{
    if (_leader && !_applyingLeader && isViewerInteraction(e) && isInsideViewer(watched, _gui->getViewersList())) {
        stopFollowing(tr("Stopped following: the viewer was used here"));
    }

    return QObject::eventFilter(watched, e);
}

void
LiveSessionPresenceTracker::showNote(const QString& note)
{
    _gui->statusBar()->showMessage(note, 8000);
}

NATRON_NAMESPACE_EXIT
