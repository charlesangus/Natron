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

#include "PyGuiApp.h"

#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColorDialog>
#include <QFileInfo>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/ProjectMerge.h"
#include "Engine/PyNode.h"
#include "Engine/PyNodeGroup.h"
#include "Engine/PyParameter.h" // ColorTuple
#include "Engine/PythonUndoCommands.h"
#include "Engine/ScriptObject.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

#include "Gui/Gui.h"
#include "Gui/MergeConflictResolve.h"
#include "Gui/MergePicks.h"
#include "Gui/NodeDiffPanel.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphDiffOverlay.h"
#include "Gui/NodeGraphMergeBar.h"
#include "Gui/NodeGui.h"
#include "Gui/PythonPanels.h"
#include "Gui/SequenceFileDialog.h"
#include "Gui/TabMenuBridge.h"
#include "Gui/TabWidget.h"
#include "Gui/ViewerGL.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER
NATRON_PYTHON_NAMESPACE_ENTER

// The viewer setters write the viewer node's knobs: record them so the edit undoes like a
// parameter set from Python. False leaves a Python error set and nothing must change.
static bool
recordViewerKnobEdits(ViewerInstance* viewer,
                      std::initializer_list<const char*> names)
{
    for (const char* name : names) {
        KnobIPtr knob = viewer->getKnobByName(name);
        if (knob && !recordPythonKnobEdit(knob)) {
            return false;
        }
    }

    return true;
}

GuiApp::GuiApp(const GuiAppInstancePtr& app)
    : App(app)
    , _app(app)
{
}

GuiApp::~GuiApp()
{
}

Gui*
GuiApp::getGui() const
{
    return getInternalGuiApp()->getGui();
}

PyModalDialog*
GuiApp::createModalDialog()
{
    PyModalDialog* ret = new PyModalDialog( getInternalGuiApp()->getGui() );

    return ret;
}

PyTabWidget*
GuiApp::getTabWidget(const QString& name) const
{
    const std::list<TabWidget*>& tabs = getInternalGuiApp()->getGui()->getPanes();

    for (std::list<TabWidget*>::const_iterator it = tabs.begin(); it != tabs.end(); ++it) {
        if ( (*it)->objectName_mt_safe() == name ) {
            return new PyTabWidget(*it);
        }
    }

    return 0;
}

PyTabWidget*
GuiApp::getActiveTabWidget() const
{
    TabWidget* tab =  getInternalGuiApp()->getGui()->getLastEnteredTabWidget();

    if (!tab) {
        return 0;
    }

    return new PyTabWidget(tab);
}

bool
GuiApp::moveTab(const QString& scriptName,
                PyTabWidget* pane)
{
    PanelWidget* w;
    ScriptObject* o;

    getInternalGuiApp()->getGui()->findExistingTab(scriptName.toStdString(), &w, &o);
    if (!w || !o) {
        return false;
    }

    return TabWidget::moveTab( w, o, pane->getInternalTabWidget() );
}

void
GuiApp::registerPythonPanel(PyPanel* panel,
                            const QString& pythonFunction)
{
    getInternalGuiApp()->getGui()->registerPyPanel( panel, pythonFunction.toStdString() );
}

void
GuiApp::unregisterPythonPanel(PyPanel* panel)
{
    getInternalGuiApp()->getGui()->unregisterPyPanel(panel);
}

QString
GuiApp::getFilenameDialog(const QStringList& filters,
                          const QString& location) const
{
    Gui* gui = getInternalGuiApp()->getGui();
    std::vector<std::string> f;

    for (QStringList::const_iterator it = filters.begin(); it != filters.end(); ++it) {
        f.push_back( it->toStdString() );
    }
    SequenceFileDialog dialog(gui,
                              f,
                              false,
                              SequenceFileDialog::eFileDialogModeOpen,
                              location.toStdString(),
                              gui,
                              false);
    if ( dialog.exec() ) {
        QString ret = QString::fromUtf8( dialog.selectedFiles().c_str() );

        return ret;
    }

    return QString();
}

QString
GuiApp::getSequenceDialog(const QStringList& filters,
                          const QString& location) const
{
    Gui* gui = getInternalGuiApp()->getGui();
    std::vector<std::string> f;

    for (QStringList::const_iterator it = filters.begin(); it != filters.end(); ++it) {
        f.push_back( it->toStdString() );
    }
    SequenceFileDialog dialog(gui,
                              f,
                              true,
                              SequenceFileDialog::eFileDialogModeOpen,
                              location.toStdString(),
                              gui,
                              false);
    if ( dialog.exec() ) {
        QString ret = QString::fromUtf8( dialog.selectedFiles().c_str() );

        return ret;
    }

    return QString();
}

QString
GuiApp::getDirectoryDialog(const QString& location) const
{
    Gui* gui = getInternalGuiApp()->getGui();
    std::vector<std::string> f;
    SequenceFileDialog dialog(gui,
                              f,
                              false,
                              SequenceFileDialog::eFileDialogModeDir,
                              location.toStdString(),
                              gui,
                              false);

    if ( dialog.exec() ) {
        QString ret = QString::fromUtf8( dialog.selectedDirectory().c_str() );

        return ret;
    }

    return QString();
}

QString
GuiApp::saveFilenameDialog(const QStringList& filters,
                           const QString& location) const
{
    Gui* gui = getInternalGuiApp()->getGui();
    std::vector<std::string> f;

    for (QStringList::const_iterator it = filters.begin(); it != filters.end(); ++it) {
        f.push_back( it->toStdString() );
    }
    SequenceFileDialog dialog(gui,
                              f,
                              false,
                              SequenceFileDialog::eFileDialogModeSave,
                              location.toStdString(),
                              gui,
                              false);
    if ( dialog.exec() ) {
        QString ret = QString::fromUtf8( dialog.filesToSave().c_str() );

        return ret;
    }

    return QString();
}

QString
GuiApp::saveSequenceDialog(const QStringList& filters,
                           const QString& location) const
{
    Gui* gui = getInternalGuiApp()->getGui();
    std::vector<std::string> f;

    for (QStringList::const_iterator it = filters.begin(); it != filters.end(); ++it) {
        f.push_back( it->toStdString() );
    }
    SequenceFileDialog dialog(gui,
                              f,
                              true,
                              SequenceFileDialog::eFileDialogModeSave,
                              location.toStdString(),
                              gui,
                              false);
    if ( dialog.exec() ) {
        QString ret = QString::fromUtf8( dialog.filesToSave().c_str() );

        return ret;
    }

    return QString();
}

ColorTuple
GuiApp::getRGBColorDialog() const
{
    ColorTuple ret;
    QColorDialog dialog;

    if ( dialog.exec() ) {
        QColor color = dialog.currentColor();

        ret.r = color.redF();
        ret.g = color.greenF();
        ret.b = color.blueF();
        ret.a = 1.;
    } else {
        ret.r = ret.g = ret.b = ret.a = 0.;
    }

    return ret;
}

Effect*
GuiApp::createNodeInGraph(const QString& pluginID,
                          int majorVersion,
                          Group* group,
                          const NodeCreationPropertyMap& props) const
{
    GuiAppInstancePtr app = getInternalGuiApp();
    NodeCollectionPtr collection;
    if (!group && app && !app->getNodeCreationContextCollection() && app->getGui()) {
        collection = app->getGui()->getLastSelectedNodeCollection();
    }
    if (!collection) {
        collection = getCollectionFromGroup(group);
    }

    return createNodeInCollection(pluginID, majorVersion, collection, props, true);
}

std::list<Effect*>
GuiApp::getSelectedNodes(Group* group) const
{
    std::list<Effect*> ret;
    NodeGraph* graph = 0;

    if (group) {
        Effect* isEffect = dynamic_cast<Effect*>(group);
        if (isEffect) {
            NodeGroup* nodeGrp = isEffect->getInternalNode()->isEffectGroup();
            if (nodeGrp) {
                NodeGraphI* graph_i  = nodeGrp->getNodeGraph();
                if (graph_i) {
                    graph = dynamic_cast<NodeGraph*>(graph_i);
                    assert(graph);
                    if (!graph) {
                        throw std::logic_error("");
                    }
                }
            }
        }
    } else {
        graph = getInternalGuiApp()->getGui()->getLastSelectedGraph();
    }
    if (!graph) {
        graph = getInternalGuiApp()->getGui()->getNodeGraph();
    }
    assert(graph);
    if (!graph) {
        throw std::logic_error("");
    }
    const NodesGuiList& nodes = graph->getSelectedNodes();
    for (NodesGuiList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        NodePtr node = (*it)->getNode();
        if ( node->isActivated() && !node->getParentMultiInstance() ) {
            ret.push_back( new Effect(node) );
        }
    }

    return ret;
}

void
GuiApp::selectNode(Effect* effect,
                   bool clearPreviousSelection)
{
    if ( !effect || appPTR->isBackground() ) {
        return;
    }
    NodeCollectionPtr collection = effect->getInternalNode()->getGroup();
    if (!collection) {
        return;
    }

    NodeGuiPtr nodeUi = std::dynamic_pointer_cast<NodeGui>( effect->getInternalNode()->getNodeGui() );
    if (!nodeUi) {
        return;
    }
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( collection.get() );
    NodeGraph* graph = 0;
    if (isGroup) {
        graph = dynamic_cast<NodeGraph*>( isGroup->getNodeGraph() );
    } else {
        graph = getInternalGuiApp()->getGui()->getNodeGraph();
    }
    assert(graph);
    if (!graph) {
        throw std::logic_error("invalid graph");
    }
    graph->selectNode(nodeUi, !clearPreviousSelection);
}

void
GuiApp::setSelection(const std::list<Effect*>& nodes)
{
    if ( appPTR->isBackground() ) {
        return;
    }
    NodesGuiList selection;
    NodeCollectionPtr collection;
    bool printWarn = false;
    for (std::list<Effect*>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        NodeGuiPtr nodeUi = std::dynamic_pointer_cast<NodeGui>( (*it)->getInternalNode()->getNodeGui() );
        if (!nodeUi) {
            continue;
        }
        if (!collection) {
            collection = (*it)->getInternalNode()->getGroup();
        } else {
            if ( (*it)->getInternalNode()->getGroup() != collection ) {
                ///Group mismatch
                printWarn = true;
                continue;
            }
        }
        selection.push_back(nodeUi);
    }
    if (printWarn) {
        getInternalGuiApp()->appendToScriptEditor( tr("Python: Invalid selection from setSelection(): Some nodes in the list do not belong to the same group.").toStdString() );
    } else {
        NodeGroup* isGroup = dynamic_cast<NodeGroup*>( collection.get() );
        NodeGraph* graph = 0;
        if (isGroup) {
            graph = dynamic_cast<NodeGraph*>( isGroup->getNodeGraph() );
        } else {
            graph = getInternalGuiApp()->getGui()->getNodeGraph();
        }
        assert(graph);
        if (!graph) {
            throw std::logic_error("invalid graph");
        }
        graph->setSelection(selection);
    }
}

void
GuiApp::selectAllNodes(Group* group)
{
    if ( appPTR->isBackground() ) {
        return;
    }
    NodeGraph* graph = 0;
    NodeCollectionPtr collection;
    NodeGroup* isGroup = 0;
    if (group) {
        collection = group->getInternalCollection();
        if (collection) {
            isGroup = dynamic_cast<NodeGroup*>( collection.get() );
            if (isGroup) {
                graph = dynamic_cast<NodeGraph*>( isGroup->getNodeGraph() );
            }
        }
    } else {
        graph = getInternalGuiApp()->getGui()->getLastSelectedGraph();
    }
    if (!graph) {
        graph = getInternalGuiApp()->getGui()->getNodeGraph();
    }
    assert(graph);
    if (!graph) {
        throw std::logic_error("invalid graph");
    }
    graph->selectAllNodes(false);
}


void
GuiApp::copySelectedNodes(Group* group)
{
    if ( appPTR->isBackground() ) {
        return;
    }
    NodeGraph* graph = 0;
    NodeCollectionPtr collection;
    NodeGroup* isGroup = 0;
    if (group) {
        collection = group->getInternalCollection();
        if (collection) {
            isGroup = dynamic_cast<NodeGroup*>( collection.get() );
            if (isGroup) {
                graph = dynamic_cast<NodeGraph*>( isGroup->getNodeGraph() );
            }
        }
    } else {
        graph = getInternalGuiApp()->getGui()->getLastSelectedGraph();
    }
    if (!graph) {
        graph = getInternalGuiApp()->getGui()->getNodeGraph();
    }
    assert(graph);
    if (!graph) {
        throw std::logic_error("invalid graph");
    }
    graph->copySelectedNodes();
}

void
GuiApp::pasteNodes(Group* group)
{
    if ( appPTR->isBackground() ) {
        return;
    }
    NodeGraph* graph = 0;
    NodeCollectionPtr collection;
    NodeGroup* isGroup = 0;
    if (group) {
        collection = group->getInternalCollection();
        if (collection) {
            isGroup = dynamic_cast<NodeGroup*>( collection.get() );
            if (isGroup) {
                graph = dynamic_cast<NodeGraph*>( isGroup->getNodeGraph() );
            }
        }
    } else {
        graph = getInternalGuiApp()->getGui()->getLastSelectedGraph();
    }
    if (!graph) {
        graph = getInternalGuiApp()->getGui()->getNodeGraph();
    }
    assert(graph);
    if (!graph) {
        throw std::logic_error("");
    }
    graph->pasteNodeClipBoards();
}

void
GuiApp::deselectNode(Effect* effect)
{
    if ( !effect || appPTR->isBackground() ) {
        return;
    }

    NodeCollectionPtr collection = effect->getInternalNode()->getGroup();
    if (!collection) {
        return;
    }

    NodeGuiPtr nodeUi = std::dynamic_pointer_cast<NodeGui>( effect->getInternalNode()->getNodeGui() );
    if (!nodeUi) {
        return;
    }
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>( collection.get() );
    NodeGraph* graph = 0;
    if (isGroup) {
        graph = dynamic_cast<NodeGraph*>( isGroup->getNodeGraph() );
    } else {
        graph = getInternalGuiApp()->getGui()->getNodeGraph();
    }
    assert(graph);
    if (!graph) {
        throw std::logic_error("invalid graph");
    }
    graph->deselectNode(nodeUi);
}

void
GuiApp::clearSelection(Group* group)
{
    if ( appPTR->isBackground() ) {
        return;
    }

    NodeGraph* graph = 0;
    NodeCollectionPtr collection;
    NodeGroup* isGroup = 0;
    if (group) {
        collection = group->getInternalCollection();
        if (collection) {
            isGroup = dynamic_cast<NodeGroup*>( collection.get() );
            if (isGroup) {
                graph = dynamic_cast<NodeGraph*>( isGroup->getNodeGraph() );
            }
        }
    } else {
        graph = getInternalGuiApp()->getGui()->getLastSelectedGraph();
    }
    if (!graph) {
        graph = getInternalGuiApp()->getGui()->getNodeGraph();
    }
    assert(graph);
    if (!graph) {
        throw std::logic_error("invalid graph");
    }
    graph->clearSelection();
}

PyViewer*
GuiApp::getViewer(const QString& scriptName) const
{
    NodePtr ptr = getInternalGuiApp()->getNodeByFullySpecifiedName( scriptName.toStdString() );

    if ( !ptr || !ptr->isActivated() ) {
        return 0;
    }

    ViewerInstance* viewer = ptr->isEffectViewer();
    if (!viewer) {
        return 0;
    }

    return new PyViewer(ptr);
}

PyViewer*
GuiApp::getActiveViewer() const
{
    ViewerTab* tab = getInternalGuiApp()->getGui()->getActiveViewer();

    if (!tab) {
        return 0;
    }
    ViewerInstance* instance = tab->getInternalNode();
    if (!instance) {
        return 0;
    }
    NodePtr node = instance->getNode();
    if (!node) {
        return 0;
    }

    return new PyViewer(node);
}

PyPanel*
GuiApp::getUserPanel(const QString& scriptName) const
{
    PanelWidget* w = getInternalGuiApp()->getGui()->findExistingTab( scriptName.toStdString() );

    if (!w) {
        return 0;
    }

    return dynamic_cast<PyPanel*>( w->getWidget() );
}

void
GuiApp::renderBlocking(Effect* writeNode,
                       int firstFrame,
                       int lastFrame,
                       int frameStep)
{
    renderInternal(true, writeNode, firstFrame, lastFrame, frameStep);
}

void
GuiApp::renderBlocking(const std::list<Effect*>& effects,
                       const std::list<int>& firstFrames,
                       const std::list<int>& lastFrames,
                       const std::list<int>& frameSteps)
{
    renderInternal(true, effects, firstFrames, lastFrames, frameSteps);
}

void
GuiApp::triggerTabMenuAction(QAction* action)
{
    TabMenuBridge::triggerAction(action);
}

PyViewer::PyViewer(const NodePtr& node)
    : _node(node)
{
    ViewerInstance* viewer = node->isEffectViewer();

    assert(viewer);
    ViewerGL* viewerGL = dynamic_cast<ViewerGL*>( viewer->getUiContext() );
    _viewer = viewerGL ? viewerGL->getViewerTab() : NULL;
    assert(_viewer);
}

PyViewer::~PyViewer()
{
}

void
PyViewer::seek(int frame)
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    _viewer->seek(frame);
}

int
PyViewer::getCurrentFrame()
{
    if ( !getInternalNode()->isActivated() ) {
        return 0;
    }

    return getInternalNode()->getApp()->getTimeLine()->currentFrame();
}

void
PyViewer::startForward()
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    _viewer->startPause(true);
}

void
PyViewer::startBackward()
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    _viewer->startBackward(true);
}

void
PyViewer::pause()
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    _viewer->abortRendering();
}

void
PyViewer::redraw()
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    _viewer->redrawGLWidgets();
}

void
PyViewer::renderCurrentFrame(bool useCache)
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    if (useCache) {
        _viewer->getInternalNode()->renderCurrentFrame(false);
    } else {
        _viewer->refresh();
    }
}

void
PyViewer::setFrameRange(int firstFrame,
                        int lastFrame)
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    if (!recordViewerKnobEdits(_viewer->getInternalNode(), { kViewerParamInPoint, kViewerParamOutPoint })) {
        return;
    }
    _viewer->setFrameRange(firstFrame, lastFrame);
}

void
PyViewer::getFrameRange(int* firstFrame,
                        int* lastFrame) const
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    _viewer->getTimelineBounds(firstFrame, lastFrame);
}

void
PyViewer::setPlaybackMode(PlaybackModeEnum mode)
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    _viewer->setPlaybackMode(mode);
}

PlaybackModeEnum
PyViewer::getPlaybackMode() const
{
    if ( !getInternalNode()->isActivated() ) {
        return ePlaybackModeLoop;
    }

    return _viewer->getPlaybackMode();
}

ViewerCompositingOperatorEnum
PyViewer::getCompositingOperator() const
{
    if ( !getInternalNode()->isActivated() ) {
        return eViewerCompositingOperatorNone;
    }

    return _viewer->getCompositingOperator();
}

void
PyViewer::setCompositingOperator(ViewerCompositingOperatorEnum op)
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    if (!recordViewerKnobEdits(_viewer->getInternalNode(), { kViewerParamOperation })) {
        return;
    }
    _viewer->setCompositingOperator(op);
}

int
PyViewer::getAInput() const
{
    if ( !getInternalNode()->isActivated() ) {
        return -1;
    }
    int a, b;
    _viewer->getInternalNode()->getActiveInputs(a, b);

    return a;
}

void
PyViewer::setAInput(int index)
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    EffectInstancePtr input = _viewer->getInternalNode()->getInput(index);
    if (!input || !recordViewerKnobEdits(_viewer->getInternalNode(), { kViewerParamAInput })) {
        return;
    }
    _viewer->setInputA(index);
}

int
PyViewer::getBInput() const
{
    if ( !getInternalNode()->isActivated() ) {
        return -1;
    }
    int a, b;
    _viewer->getInternalNode()->getActiveInputs(a, b);

    return b;
}

void
PyViewer::setBInput(int index)
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    EffectInstancePtr input = _viewer->getInternalNode()->getInput(index);
    if (!input || !recordViewerKnobEdits(_viewer->getInternalNode(), { kViewerParamBInput })) {
        return;
    }
    _viewer->setInputB(index);
}

void
PyViewer::setChannels(DisplayChannelsEnum channels)
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    if (!recordViewerKnobEdits(_viewer->getInternalNode(), { kViewerParamDisplayChannels, kViewerParamDisplayChannelsB })) {
        return;
    }
    std::string c = ViewerTab::getChannelsString(channels);
    _viewer->setChannels(c);
}

DisplayChannelsEnum
PyViewer::getChannels() const
{
    if ( !getInternalNode()->isActivated() ) {
        return eDisplayChannelsRGB;
    }

    return _viewer->getChannels();
}

void
PyViewer::setProxyModeEnabled(bool enabled)
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    if (!recordViewerKnobEdits(_viewer->getInternalNode(), { kViewerParamEnableDownscale })) {
        return;
    }
    _viewer->setRenderScaleActivated(enabled);
}

bool
PyViewer::isProxyModeEnabled() const
{
    if ( !getInternalNode()->isActivated() ) {
        return false;
    }

    return _viewer->getRenderScaleActivated();
}

void
PyViewer::setProxyIndex(int index)
{
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    if (!recordViewerKnobEdits(_viewer->getInternalNode(), { kViewerParamEnableDownscale, kViewerParamDownscaleLevel })) {
        return;
    }
    _viewer->setMipmapLevel(index + 1);
}

int
PyViewer::getProxyIndex() const
{
    if ( !getInternalNode()->isActivated() ) {
        return 0;
    }

    return _viewer->getMipmapLevel() - 1;
}

void
PyViewer::setCurrentView(int index)
{
    if (index < 0) {
        return;
    }
    if ( !getInternalNode()->isActivated() ) {
        return;
    }
    if (!recordViewerKnobEdits(_viewer->getInternalNode(), { kViewerParamActiveView })) {
        return;
    }
    _viewer->setCurrentView( ViewIdx(index) );
}

int
PyViewer::getCurrentView() const
{
    if ( !getInternalNode()->isActivated() ) {
        return 0;
    }

    return _viewer->getCurrentView().value();
}

QString
GuiApp::importTemplateAsUndoStep(const QString& filePath)
{
    QString error;
    const std::string key = MergeConflictResolve::importTemplate(getInternalGuiApp()->getGui(), filePath, &error);
    if (!error.isEmpty()) {
        getInternalGuiApp()->appendToScriptEditor(error.toStdString());
    }

    return QString::fromStdString(key);
}

int
GuiApp::updateFromTemplate(const QString& templateKey,
                           const QString& filePath)
{
    const ProjectMerge::UpdateResult result = MergeConflictResolve::updateFromTemplate(getInternalGuiApp()->getGui(), templateKey.toStdString(), filePath);
    if (!result.ok) {
        getInternalGuiApp()->appendToScriptEditor(result.error.toStdString());

        return -1;
    }

    return (int)result.conflicts.size();
}

bool
GuiApp::isMergeConflictBadgeShown(Effect* effect) const
{
    if (!effect) {
        return false;
    }
    NodeGuiPtr nodeUi = std::dynamic_pointer_cast<NodeGui>(effect->getInternalNode()->getNodeGui());

    return nodeUi && nodeUi->isMergeConflictBadgeShown();
}

bool
GuiApp::compareWithFile(const QString& filePath)
{
    ProjectMerge::ChangeSummary summary;
    QString error;
    if (!ProjectMerge::compareWithFile(getInternalGuiApp()->getProject(), filePath, &summary, &error)) {
        getInternalGuiApp()->appendToScriptEditor(error.toStdString());

        return false;
    }
    getInternalGuiApp()->getGui()->setGraphComparison(summary, QFileInfo(filePath).fileName());

    return true;
}

bool
GuiApp::compareWithTemplateBase(const QString& templateKey)
{
    ProjectMerge::ChangeSummary summary;
    QString error;
    if (!ProjectMerge::compareWithTemplateBase(getInternalGuiApp()->getProject(), templateKey.toStdString(), &summary, &error)) {
        getInternalGuiApp()->appendToScriptEditor(error.toStdString());

        return false;
    }
    getInternalGuiApp()->getGui()->setGraphComparison(summary, Gui::templateBaseDescription(getInternalGuiApp()->getProject(), templateKey.toStdString()));

    return true;
}

void
GuiApp::clearGraphComparison()
{
    getInternalGuiApp()->getGui()->clearGraphComparison();
}

QString
GuiApp::getDiffTint(Effect* effect) const
{
    if (!effect) {
        return QString();
    }
    NodeGuiPtr nodeUi = std::dynamic_pointer_cast<NodeGui>(effect->getInternalNode()->getNodeGui());
    NodeGraph* graph = nodeUi ? nodeUi->getDagGui() : 0;
    NodeGraphDiffOverlay* overlay = graph ? graph->getDiffOverlay() : 0;
    switch (overlay ? overlay->tintOf(nodeUi.get()) : NodeGraphDiffOverlay::eTintNone) {
    case NodeGraphDiffOverlay::eTintAdded:
        return QString::fromUtf8("added");
    case NodeGraphDiffOverlay::eTintChanged:
        return QString::fromUtf8("changed");
    case NodeGraphDiffOverlay::eTintMoved:
        return QString::fromUtf8("moved");
    case NodeGraphDiffOverlay::eTintNone:
        break;
    }

    return QString();
}

namespace {
NodeGraph*
diffGraphOf(GuiAppInstance* app,
            Group* group)
{
    NodeGraph* graph = 0;
    if (Effect* isEffect = dynamic_cast<Effect*>(group)) {
        if (NodeGroup* nodeGrp = isEffect->getInternalNode()->isEffectGroup()) {
            graph = dynamic_cast<NodeGraph*>(nodeGrp->getNodeGraph());
        }
    } else {
        graph = app->getGui()->getNodeGraph();
    }

    return graph;
}
} // anon namespace

int
GuiApp::getDiffGhostCount(Group* group) const
{
    NodeGraph* graph = diffGraphOf(getInternalGuiApp().get(), group);
    NodeGraphDiffOverlay* overlay = graph ? graph->getDiffOverlay() : 0;

    return overlay ? overlay->ghostCount() : 0;
}

RectD
GuiApp::getDiffGhostRect(int index,
                         Group* group) const
{
    NodeGraph* graph = diffGraphOf(getInternalGuiApp().get(), group);
    NodeGraphDiffOverlay* overlay = graph ? graph->getDiffOverlay() : 0;
    const QRectF rect = overlay ? overlay->ghostRect(index) : QRectF();

    return RectD(rect.left(), rect.top(), rect.right(), rect.bottom());
}

QStringList
GuiApp::getNodeDiffRows() const
{
    NodeDiffPanel* panel = getInternalGuiApp()->getGui()->getNodeDiffPanel();

    return panel ? panel->getRows() : QStringList();
}

namespace {
// The graph that draws change: its group's, or the top level's.
NodeGraph*
graphOfChange(Gui* gui,
              const ProjectPtr& project,
              const ProjectMerge::NodeChange& change)
{
    if (change.parentId.empty()) {
        return gui->getNodeGraph();
    }
    const NodePtr parent = project->findNodeById(change.parentId);
    NodeGroup* group = parent ? parent->isEffectGroup() : 0;

    return group ? dynamic_cast<NodeGraph*>(group->getNodeGraph()) : 0;
}

// The removed nodes of the comparison drawn as ghosts, each with the graph drawing it.
std::vector<std::pair<const ProjectMerge::NodeChange*, NodeGraph*>>
drawnGhosts(Gui* gui,
            const ProjectPtr& project)
{
    std::vector<std::pair<const ProjectMerge::NodeChange*, NodeGraph*>> ghosts;
    const ProjectMerge::ChangeSummary* summary = gui->getGraphComparison();
    if (!summary) {
        return ghosts;
    }
    for (const ProjectMerge::NodeChange& change : summary->nodes) {
        if (change.kind != ProjectMerge::NodeChange::eRemoved) {
            continue;
        }
        NodeGraph* graph = graphOfChange(gui, project, change);
        if (graph && graph->getDiffOverlay() && !graph->getDiffOverlay()->ghostRectOf(change.nodeId).isEmpty()) {
            ghosts.push_back(std::make_pair(&change, graph));
        }
    }

    return ghosts;
}

bool
namedIn(const QStringList& names,
        const ProjectMerge::NodeChange& change)
{
    return names.contains(QString::fromStdString(change.scriptName)) || names.contains(QString::fromStdString(change.nodeId));
}

// The node IDs of effects and the graph of the first of them, or of the graph last worked in.
std::vector<std::string>
idsOf(Gui* gui,
      const std::list<Effect*>& effects,
      NodeGraph** graph)
{
    std::vector<std::string> ids;
    *graph = 0;
    for (Effect* effect : effects) {
        const NodePtr node = effect ? effect->getInternalNode() : NodePtr();
        if (!node) {
            continue;
        }
        ids.push_back(node->getNodeId());
        if (!*graph) {
            const NodeGuiPtr nodeUi = std::dynamic_pointer_cast<NodeGui>(node->getNodeGui());
            *graph = nodeUi ? nodeUi->getDagGui() : 0;
        }
    }
    if (!*graph) {
        *graph = gui->getLastSelectedGraph() ? gui->getLastSelectedGraph() : gui->getNodeGraph();
    }

    return ids;
}
} // anon namespace

bool
GuiApp::restoreFromComparison(const QStringList& nodeNames,
                              const QString& splice)
{
    Gui* gui = getInternalGuiApp()->getGui();
    ProjectMerge::SpliceMode mode = ProjectMerge::SpliceMode::eAsk;
    if (splice == QString::fromUtf8("keepMine")) {
        mode = ProjectMerge::SpliceMode::eKeepMine;
    } else if (splice == QString::fromUtf8("everywhere")) {
        mode = ProjectMerge::SpliceMode::eEverywhere;
    } else if (!splice.isEmpty() && (splice != QString::fromUtf8("ask"))) {
        getInternalGuiApp()->appendToScriptEditor(tr("Unknown splice mode %1: use \"ask\", \"keepMine\" or \"everywhere\".").arg(splice).toStdString());

        return false;
    }
    std::vector<std::string> ids;
    NodeGraph* graph = 0;
    for (const std::pair<const ProjectMerge::NodeChange*, NodeGraph*>& ghost : drawnGhosts(gui, getInternalGuiApp()->getProject())) {
        const bool picked = nodeNames.isEmpty() ? ghost.second->getDiffOverlay()->isGhostSelected(ghost.first->nodeId) : namedIn(nodeNames, *ghost.first);
        if (picked) {
            ids.push_back(ghost.first->nodeId);
            if (!graph) {
                graph = ghost.second;
            }
        }
    }
    QString error;
    if (!graph) {
        error = tr("No ghost of the comparison was named or selected.");
    } else if (MergePicks::restore(graph, ids, mode, &error)) {
        return true;
    }
    getInternalGuiApp()->appendToScriptEditor(error.toStdString());

    return false;
}

bool
GuiApp::revertFromComparison(const std::list<Effect*>& effects)
{
    NodeGraph* graph = 0;
    const std::vector<std::string> ids = idsOf(getInternalGuiApp()->getGui(), effects, &graph);
    QString error;
    if (MergePicks::revert(graph, ids, &error)) {
        return true;
    }
    getInternalGuiApp()->appendToScriptEditor(error.toStdString());

    return false;
}

bool
GuiApp::takeKnobFromComparison(Effect* effect,
                               const QString& knobName)
{
    NodeGraph* graph = 0;
    const std::vector<std::string> ids = idsOf(getInternalGuiApp()->getGui(), std::list<Effect*>(1, effect), &graph);
    QString error;
    if (!ids.empty() && MergePicks::takeKnob(graph, ids.front(), knobName.toStdString(), &error)) {
        return true;
    }
    getInternalGuiApp()->appendToScriptEditor((ids.empty() ? tr("No node given.") : error).toStdString());

    return false;
}

bool
GuiApp::removeAddedFromComparison(const std::list<Effect*>& effects)
{
    NodeGraph* graph = 0;
    const std::vector<std::string> ids = idsOf(getInternalGuiApp()->getGui(), effects, &graph);
    QString error;
    if (MergePicks::removeAdded(graph, ids, &error)) {
        return true;
    }
    getInternalGuiApp()->appendToScriptEditor(error.toStdString());

    return false;
}

bool
GuiApp::restorePositionFromComparison(const std::list<Effect*>& effects)
{
    NodeGraph* graph = 0;
    const std::vector<std::string> ids = idsOf(getInternalGuiApp()->getGui(), effects, &graph);
    QString error;
    if (MergePicks::restorePosition(graph, ids, &error)) {
        return true;
    }
    getInternalGuiApp()->appendToScriptEditor(error.toStdString());

    return false;
}

void
GuiApp::selectComparisonGhosts(const QStringList& nodeNames)
{
    Gui* gui = getInternalGuiApp()->getGui();
    std::map<NodeGraph*, std::set<std::string>> selection;
    for (const std::pair<const ProjectMerge::NodeChange*, NodeGraph*>& ghost : drawnGhosts(gui, getInternalGuiApp()->getProject())) {
        std::set<std::string>& ids = selection[ghost.second];
        if (namedIn(nodeNames, *ghost.first)) {
            ids.insert(ghost.first->nodeId);
        }
    }
    for (const std::pair<NodeGraph* const, std::set<std::string>>& graph : selection) {
        graph.first->setSelectedGhostIds(graph.second);
    }
}

PyObject*
GuiApp::getComparisonGhosts() const
{
    const ProjectPtr project = getInternalGuiApp()->getProject();
    const std::vector<std::pair<const ProjectMerge::NodeChange*, NodeGraph*>> ghosts = drawnGhosts(getInternalGuiApp()->getGui(), project);
    PyObject* list = PyList_New((Py_ssize_t)ghosts.size());
    for (std::size_t i = 0; list && (i < ghosts.size()); ++i) {
        const ProjectMerge::NodeChange& change = *ghosts[i].first;
        const NodePtr parent = change.parentId.empty() ? NodePtr() : project->findNodeById(change.parentId);
        PyObject* dict = PyDict_New();
        PyObject* name = PyUnicode_FromString(change.scriptName.c_str());
        PyObject* nodeId = PyUnicode_FromString(change.nodeId.c_str());
        PyObject* group = PyUnicode_FromString(parent ? parent->getFullyQualifiedName().c_str() : "");
        PyObject* selected = PyBool_FromLong(ghosts[i].second->getDiffOverlay()->isGhostSelected(change.nodeId));
        const bool ok = dict && name && nodeId && group && selected && (PyDict_SetItemString(dict, "name", name) == 0) && (PyDict_SetItemString(dict, "nodeId", nodeId) == 0) && (PyDict_SetItemString(dict, "group", group) == 0) && (PyDict_SetItemString(dict, "selected", selected) == 0);
        Py_XDECREF(name);
        Py_XDECREF(nodeId);
        Py_XDECREF(group);
        Py_XDECREF(selected);
        if (!ok) {
            Py_XDECREF(dict);
            Py_DECREF(list);

            return 0;
        }
        PyList_SetItem(list, (Py_ssize_t)i, dict);
    }

    return list;
}

bool
GuiApp::openMergePopover(Effect* effect)
{
    const NodePtr node = effect ? effect->getInternalNode() : NodePtr();
    const NodeGuiPtr nodeUi = node ? std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()) : NodeGuiPtr();
    NodeGraph* graph = nodeUi ? nodeUi->getDagGui() : 0;

    return graph && graph->openMergePopover(nodeUi);
}

bool
GuiApp::isMergePopoverOpen() const
{
    Gui* gui = getInternalGuiApp()->getGui();
    const ProjectPtr project = getInternalGuiApp()->getProject();
    std::set<NodeGraph*> graphs;
    graphs.insert(gui->getNodeGraph());
    NodesList nodes;
    project->getNodes_recursive(nodes, true);
    for (const NodePtr& node : nodes) {
        if (NodeGroup* group = node->isEffectGroup()) {
            graphs.insert(dynamic_cast<NodeGraph*>(group->getNodeGraph()));
        }
    }
    for (NodeGraph* graph : graphs) {
        if (graph && graph->isMergePopoverOpen()) {
            return true;
        }
    }

    return false;
}

QString
GuiApp::mergeBarMode(Group* group) const
{
    NodeGraph* graph = diffGraphOf(getInternalGuiApp().get(), group);
    NodeGraphMergeBar* bar = graph ? graph->getMergeBar() : 0;

    return NodeGraphMergeBar::modeName(bar ? bar->mode() : NodeGraphMergeBar::eModeNone);
}

bool
GuiApp::resolveMergeConflict(int index,
                             const QString& choice)
{
    Gui* gui = getInternalGuiApp()->getGui();
    const ProjectPtr project = getInternalGuiApp()->getProject();
    const std::vector<Serialization::MergeConflict> conflicts = project->getMergeConflicts();
    ProjectMerge::ConflictChoice found;
    QString error;
    if ((index < 0) || ((std::size_t)index >= conflicts.size())) {
        error = tr("There is no merge conflict %1.").arg(index);
    } else if (!ProjectMerge::findChoice(project, conflicts[(std::size_t)index], choice.toStdString(), &found)) {
        error = tr("Merge conflict %1 offers no choice %2.").arg(index).arg(choice);
    } else if (found.option.empty() ? MergeConflictResolve::resolve(gui, std::vector<std::size_t>(1, (std::size_t)index), found.side, &error)
                                    : MergeConflictResolve::resolveOption(gui, (std::size_t)index, found.option, &error)) {
        return true;
    }
    getInternalGuiApp()->appendToScriptEditor(error.toStdString());

    return false;
}

NATRON_PYTHON_NAMESPACE_EXIT
NATRON_NAMESPACE_EXIT
