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

#include "ViewerTab.h"
#include "ViewerTabPrivate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <sstream> // stringstream
#include <stdexcept>
#include <string>
#include <vector>

#include <QDebug>
#include <QFontMetrics>

#include <QVBoxLayout>
#include <QCheckBox>
#include <QToolBar>

#include "Engine/Format.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerInstance.h"

#include "Gui/Button.h"
#include "Gui/ChannelsComboBox.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/InfoViewerWidget.h"
#include "Gui/MultiInstancePanel.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeViewerContext.h"
#include "Gui/ScaleSliderQWidget.h"
#include "Gui/SpinBox.h"
#include "Gui/TimeLineGui.h"
#include "Gui/ViewerGL.h"


#ifndef M_LN2
#define M_LN2       0.693147180559945309417232121458176568  /* loge(2)        */
#endif


NATRON_NAMESPACE_ENTER

bool
ViewerTab::isClippedToProject() const
{
    return _imp->viewer->isClippingImageToFormat();
}

namespace {

const char* const kViewerLookNone = "None";

bool
containsName(const std::vector<std::string>& names,
             const std::string& name)
{
    return !name.empty() && std::find(names.begin(), names.end(), name) != names.end();
}

// The widest names run to 35 characters, which would make the toolbar row wider than a typical
// screen. Cap each combo's width and elide the middle of the text; the tooltip and the popup
// list keep the full names. A fixed width also stops the combo resizing as the selection changes.
const int kMenuMaxWidth = 180;

void
fitMenuToItems(ComboBox* menu)
{
    const QFontMetrics fm = menu->fontMetrics();
    const int indent = fm.horizontalAdvance(QLatin1Char('x'));
    int widest = 0;
    for (int i = 0; i < menu->count(); ++i) {
        widest = std::max(widest, fm.horizontalAdvance(menu->itemText(i)));
    }
    menu->setElidedWidth(std::min(widest + 2 * DROP_DOWN_ICON_SIZE + indent, TO_DPIX(kMenuMaxWidth)));

    const QString base = menu->property("baseToolTip").toString();
    const QString current = menu->getCurrentIndexText().toHtmlEscaped();
    if (!base.isEmpty() && !current.isEmpty()) {
        menu->setToolTip(base + QString::fromUtf8("<p>") + current + QString::fromUtf8("</p>"));
    }
}

void
fillMenu(ComboBox* menu,
         const std::vector<std::string>& names,
         const std::string& selected)
{
    menu->clear();
    int index = -1;
    for (std::size_t i = 0; i < names.size(); ++i) {
        menu->addItem(QString::fromUtf8(names[i].c_str()));
        if (names[i] == selected) {
            index = (int)i;
        }
    }
    if (index >= 0) {
        menu->setCurrentIndex_no_emit(index);
    } else if (!selected.empty()) {
        // A name the config lacks stays shown, unlisted, so the saved value reads as missing.
        menu->setCurrentText_no_emit(QString::fromUtf8(selected.c_str()));
    } else if (!names.empty()) {
        menu->setCurrentIndex_no_emit(0);
    }
    if (!menu->property("fitConnected").toBool()) {
        menu->setProperty("fitConnected", true);
        menu->setProperty("baseToolTip", menu->toolTip());
        QObject::connect(menu, QOverload<int>::of(&ComboBox::currentIndexChanged), menu, [menu](int) { fitMenuToItems(menu); });
    }
    fitMenuToItems(menu);
}

} // anon namespace

void
ViewerTab::getDisplayTransform(std::string* display,
                               std::string* view,
                               std::string* look) const
{
    if (display) {
        *display = _imp->viewerValue<std::string>(kViewerParamDisplay, std::string());
    }
    if (view) {
        *view = _imp->viewerValue<std::string>(kViewerParamView, std::string());
    }
    if (look) {
        *look = _imp->viewerValue<std::string>(kViewerParamLook, std::string());
    }
}

void
ViewerTab::setDisplayTransform(const std::string& display,
                               const std::string& view,
                               const std::string& look)
{
    _imp->setViewerValue<std::string>(kViewerParamDisplay, display);
    _imp->setViewerValue<std::string>(kViewerParamView, view);
    _imp->setViewerValue<std::string>(kViewerParamLook, look);
}

void
ViewerTab::refreshDisplayTransformMenus()
{
    ProjectPtr project = getGui() ? getGui()->getApp()->getProject() : ProjectPtr();
    ProjectColorManagementPtr colorManagement = project ? project->getColorManagement() : ProjectColorManagementPtr();
    std::string display, view, look;
    getDisplayTransform(&display, &view, &look);

    std::string resolvedDisplay, resolvedView, resolvedLook;
    if (_imp->viewerNode) {
        _imp->viewerNode->getResolvedDisplayTransform(&resolvedDisplay, &resolvedView, &resolvedLook);
    }
    // The View menu lists the views of the display the viewer renders with.
    std::vector<std::string> displays, views, looks;
    if (colorManagement) {
        displays = colorManagement->getDisplays();
        views = colorManagement->getViews(resolvedDisplay);
        looks = colorManagement->getLooks();
    }
    std::vector<std::string> lookItems(1, std::string(kViewerLookNone));
    lookItems.insert(lookItems.end(), looks.begin(), looks.end());

    _imp->populatingDisplayTransform = true;
    fillMenu(_imp->viewerDisplay, displays, display.empty() ? resolvedDisplay : display);
    fillMenu(_imp->viewerView, views, view.empty() ? resolvedView : view);
    fillMenu(_imp->viewerLook, lookItems, look.empty() ? std::string(kViewerLookNone) : look);
    _imp->populatingDisplayTransform = false;

    if (_imp->viewer) {
        _imp->viewer->refreshDisplayTransform();
    }
}

void
ViewerTab::setUserRoIEnabled(bool b)
{
    _imp->setViewerValue<bool>(kViewerParamEnableUserRoI, b);
}

bool
ViewerTab::isAutoContrastEnabled() const
{
    return _imp->viewerNode->isAutoContrastEnabled();
}

void
ViewerTab::setAutoContrastEnabled(bool b)
{
    _imp->setViewerValue<bool>(kViewerParamAutoContrast, b);
}

void
ViewerTab::setUserRoI(const RectD & r)
{
    _imp->viewer->setUserRoI(r);
    writeUserRoIKnobs(r);
}

void
ViewerTab::writeUserRoIKnobs(const RectD& r)
{
    _imp->setViewerValue<double>(kViewerParamUserRoIBottomLeft, r.x1, 0);
    _imp->setViewerValue<double>(kViewerParamUserRoIBottomLeft, r.y1, 1);
    _imp->setViewerValue<double>(kViewerParamUserRoISize, r.width(), 0);
    _imp->setViewerValue<double>(kViewerParamUserRoISize, r.height(), 1);
}

void
ViewerTab::setClipToProject(bool b)
{
    _imp->setViewerValue<bool>(kViewerParamClipToFormat, b);
}

void
ViewerTab::setFullFrameProcessing(bool fullFrame)
{
    _imp->setViewerValue<bool>(kViewerParamFullFrame, fullFrame);
}

bool
ViewerTab::isFullFrameProcessingEnabled() const
{
    return _imp->viewerNode->isFullFrameProcessingEnabled();
}

void
ViewerTab::setGain(double d)
{
    if (d <= 0.) {
        return;
    }
    const double fstop = std::log(d) / M_LN2;
    _imp->setViewerValue<bool>(kViewerParamEnableGain, d != 1.);
    if (d != 1.) {
        _imp->setViewerValue<double>(kViewerParamGain, fstop);
    }
}

double
ViewerTab::getGain() const
{
    return _imp->viewerNode->getGain();
}

void
ViewerTab::setGamma(double gamma)
{
    _imp->setViewerValue<bool>(kViewerParamEnableGamma, gamma != 1.);
    if (gamma != 1.) {
        _imp->setViewerValue<double>(kViewerParamGamma, gamma);
    }
}

double
ViewerTab::getGamma() const
{
    return _imp->viewerNode->getGamma();
}

void
ViewerTab::setMipmapLevel(unsigned int level)
{
    _imp->setViewerValue<bool>(kViewerParamEnableDownscale, level > 0);
    if (level > 0) {
        _imp->setViewerValue<int>(kViewerParamDownscaleLevel, (int)level - 1);
    }
}

unsigned int
ViewerTab::getMipmapLevel() const
{
    return _imp->viewerNode->getMipmapLevel();
}

void
ViewerTab::setRenderScaleActivated(bool act)
{
    _imp->setViewerValue<bool>(kViewerParamEnableDownscale, act);
}

bool
ViewerTab::getRenderScaleActivated() const
{
    return _imp->viewerNode->getMipmapLevel() != 0;
}

void
ViewerTab::setZoomOrPannedSinceLastFit(bool enabled)
{
    _imp->viewer->setZoomOrPannedSinceLastFit(enabled);
}

bool
ViewerTab::getZoomOrPannedSinceLastFit() const
{
    return _imp->viewer->getZoomOrPannedSinceLastFit();
}

DisplayChannelsEnum
ViewerTab::getChannels() const
{
    return _imp->viewerNode->getChannels(0);
}

std::string
ViewerTab::getChannelsString(DisplayChannelsEnum c)
{
    switch (c) {
    case eDisplayChannelsRGB:

        return "RGB";
    case eDisplayChannelsR:

        return "R";
    case eDisplayChannelsG:

        return "G";
    case eDisplayChannelsB:

        return "B";
    case eDisplayChannelsA:

        return "A";
    case eDisplayChannelsY:

        return "Luminance";
        break;
    default:

        return "";
    }
}

std::string
ViewerTab::getChannelsString() const
{
    DisplayChannelsEnum c = _imp->viewerNode->getChannels(0);

    return getChannelsString(c);
}

void
ViewerTab::setChannels(const std::string & channelsStr)
{
    int index = _imp->viewerChannels->itemIndex( QString::fromUtf8( channelsStr.c_str() ) );

    if (index != -1) {
        _imp->setViewerValue<int>(kViewerParamDisplayChannels, index);
        _imp->setViewerValue<int>(kViewerParamDisplayChannelsB, index);
    }
}

ViewerGL*
ViewerTab::getViewer() const
{
    return _imp->viewer;
}

ViewerInstance*
ViewerTab::getInternalNode() const
{
    return _imp->viewerNode;
}

void
ViewerTab::discardInternalNodePointer()
{
    _imp->viewerNode = 0;
}

void
ViewerTab::onAutoContrastChanged(bool b)
{
    editViewerKnobs(tr("Auto Contrast"), [this, b]() {
        _imp->setViewerValue<bool>(kViewerParamAutoContrast, b);
    });
    refreshColorControlsFromKnobs();
}

void
ViewerTab::onRenderScaleComboIndexChanged(int index)
{
    editViewerKnobs(tr("Downscale Level"), [this, index]() {
        _imp->setViewerValue<int>(kViewerParamDownscaleLevel, index);
    });
    refreshProcessingButtonsFromKnobs();
}

void
ViewerTab::onRenderScaleButtonClicked(bool checked)
{
    editViewerKnobs(tr("Enable Downscale"), [this, checked]() {
        _imp->setViewerValue<bool>(kViewerParamEnableDownscale, checked);
    });
    refreshProcessingButtonsFromKnobs();
}

#if 0
void
ViewerTab::createTrackerInterface(const NodeGuiPtr& n)
{
    std::map<NodeGuiWPtr, TrackerGui*>::iterator found = _imp->trackerNodes.find(n);

    if ( found != _imp->trackerNodes.end() ) {
        return;
    }

    TrackerGui* tracker = 0;
    if ( n->getNode()->getEffectInstance()->isBuiltinTrackerNode() ) {
        TrackerPanel* panel = n->getTrackerPanel();
        assert(panel);
        tracker = new TrackerGui(panel, this);
    } else {
        assert( n->getNode()->isMultiInstance() );
        n->ensurePanelCreated();
        MultiInstancePanelPtr multiPanel = n->getMultiInstancePanel();
        if (multiPanel) {
            TrackerPanelV1Ptr trackPanel = std::dynamic_pointer_cast<TrackerPanelV1>(multiPanel);
            assert(trackPanel);
            tracker = new TrackerGui(trackPanel, this);
        }
    }


    std::pair<std::map<NodeGui*, TrackerGui*>::iterator, bool> ret = _imp->trackerNodes.insert( std::make_pair(n, tracker) );
    assert(ret.second);
    if (!ret.second) {
        qDebug() << "ViewerTab::createTrackerInterface() failed";
        delete tracker;

        return;
    }
    QObject::connect( n.get(), SIGNAL(settingsPanelClosed(bool)), this, SLOT(onTrackerNodeGuiSettingsPanelClosed(bool)) );
    if ( n->isSettingsPanelVisible() ) {
        setTrackerInterface(n);
    } else if (tracker) {
        QWidget* buttonsBar = tracker->getButtonsBar();
        assert(buttonsBar);
        if (buttonsBar) {
            buttonsBar->hide();
        }
    }
}

void
ViewerTab::setTrackerInterface(const NodeGuiPtr& n)
{
    assert(n);
    std::map<NodeGuiWPtr, TrackerGui*>::iterator it = _imp->trackerNodes.find(n);
    if ( it != _imp->trackerNodes.end() ) {
        if (_imp->currentTracker.first.lock() == n) {
            return;
        }

        ///remove any existing tracker gui
        if ( _imp->currentTracker.first.lock() ) {
            removeTrackerInterface(_imp->currentTracker.first.lock(), false, true);
        }

        ///Add the widgets

        ///if there's a current roto add it before it
        int index;
        if (_imp->currentRoto.second) {
            index = _imp->mainLayout->indexOf( _imp->currentRoto.second->getCurrentButtonsBar() );
            assert(index != -1);
        } else {
            index = _imp->mainLayout->indexOf(_imp->viewerContainer);
        }

        assert(index >= 0);
        QWidget* buttonsBar = it->second->getButtonsBar();
        assert(buttonsBar);
        if (buttonsBar) {
            _imp->mainLayout->insertWidget(index, buttonsBar);
            {
                QMutexLocker l(&_imp->visibleToolbarsMutex);
                if (_imp->topToolbarVisible) {
                    buttonsBar->show();
                }
            }
        }

        _imp->currentTracker.first = n;
        _imp->currentTracker.second = it->second;
        _imp->viewer->redraw();
    }
}

void
ViewerTab::removeTrackerInterface(const NodeGuiPtr& n,
                                  bool permanently,
                                  bool removeAndDontSetAnother)
{
    std::map<NodeGuiWPtr, TrackerGui*>::iterator it = _imp->trackerNodes.find(n);

    if ( it != _imp->trackerNodes.end() ) {
        if ( !getGui() ) {
            if (permanently) {
                delete it->second;
            }

            return;
        }

        if (_imp->currentTracker.first.lock() == n) {
            ///Remove the widgets of the current tracker node

            int buttonsBarIndex = _imp->mainLayout->indexOf( _imp->currentTracker.second->getButtonsBar() );
            assert(buttonsBarIndex >= 0);
            if (buttonsBarIndex >= 0) {
                QLayoutItem* buttonsBar = _imp->mainLayout->itemAt(buttonsBarIndex);
                assert(buttonsBar);
                if (buttonsBar) {
                    _imp->mainLayout->removeItem(buttonsBar);
                    buttonsBar->widget()->hide();
                }
            }
            if (!removeAndDontSetAnother) {
                ///If there's another tracker node, set it as the current tracker interface
                std::map<NodeGuiWPtr, TrackerGui*>::iterator newTracker = _imp->trackerNodes.end();
                for (std::map<NodeGuiWPtr, TrackerGui*>::iterator it2 = _imp->trackerNodes.begin(); it2 != _imp->trackerNodes.end(); ++it2) {
                    if ( (it2->second != it->second) && it2->first.lock()->isSettingsPanelVisible() ) {
                        newTracker = it2;
                        break;
                    }
                }

                _imp->currentTracker.first.reset();
                _imp->currentTracker.second = 0;

                if ( newTracker != _imp->trackerNodes.end() ) {
                    setTrackerInterface( newTracker->first.lock() );
                }
            }
        }

        if (permanently) {
            delete it->second;
            _imp->trackerNodes.erase(it);
        }
    }
} // ViewerTab::removeTrackerInterface

#endif // if 0

/**
 * @brief Creates a new viewer interface context for this node. This is not shared among viewers.
 **/
void
ViewerTab::createNodeViewerInterface(const NodeGuiPtr& n)
{
    if (!n) {
        return;
    }
    ViewerTabPrivate::NodeViewerContextsMap::iterator found = _imp->nodesContext.find(n);
    if ( found != _imp->nodesContext.end() ) {
        // Already exists
        return;
    }

    NodeViewerContextPtr nodeContext = NodeViewerContext::create(n, this);
    nodeContext->createGui();
    _imp->nodesContext.insert( std::make_pair(n, nodeContext) );

    if ( n->isSettingsPanelVisible() ) {
        setPluginViewerInterface(n);
    } else {
        QToolBar *toolbar = nodeContext->getToolBar();
        if (toolbar) {
            toolbar->hide();
        }
        QWidget* w = nodeContext->getContainerWidget();
        if (w) {
            w->hide();
        }
    }
}

/**
 * @brief Set the current viewer interface for a given plug-in to be the one of the given node
 **/
void
ViewerTab::setPluginViewerInterface(const NodeGuiPtr& n)
{
    if (!n) {
        return;
    }
    ViewerTabPrivate::NodeViewerContextsMap::iterator it = _imp->nodesContext.find(n);
    if ( it == _imp->nodesContext.end() ) {
        return;
    }

    std::string slot = ViewerTabPrivate::getViewerSlot(n->getNode());
    std::list<ViewerTabPrivate::PluginViewerContext>::iterator foundActive = _imp->findActiveNodeContextForSlot(slot);
    NodeGuiPtr activeNodeForPlugin;
    if ( foundActive != _imp->currentNodeContext.end() ) {
        activeNodeForPlugin = foundActive->currentNode.lock();
    }

    // If already active, return
    if (activeNodeForPlugin == n) {
        return;
    }

    QToolBar* newToolbar = it->second->getToolBar();
    QWidget* newContainer = it->second->getContainerWidget();

    // Plugin has no viewer interface
    if (!newToolbar && !newContainer) {
        return;
    }

    ///remove any existing roto gui
    if (activeNodeForPlugin) {
        removeNodeViewerInterface(activeNodeForPlugin, false /*permanently*/, /*setAnother*/ false);
    }

    // Add the widgets

    if (newToolbar) {
        _imp->viewerLayout->insertWidget(0, newToolbar);

        {
            QMutexLocker l(&_imp->visibleToolbarsMutex);
            if (_imp->leftToolbarVisible) {
                newToolbar->show();
            }
        }
    }

    // If there are other interface active, add it after them
    int index;
    if ( _imp->currentNodeContext.empty() ) {
        // insert before the viewer
        index = _imp->mainLayout->indexOf(_imp->viewerContainer);
#       ifdef DEBUG_VIEWERCONTAINER
        qDebug() << "viewer container" << _imp->viewerContainer << "index" << index;
#       endif
    } else {
        // Remove the oldest opened interface if we reached the maximum
        int maxNodeContextOpened = appPTR->getCurrentSettings()->getMaxOpenedNodesViewerContext();
#       ifdef DEBUG_VIEWERCONTAINER
        qDebug() << "currentNodeContext(" << (int)_imp->currentNodeContext.size() << "):";
        for (std::list<ViewerTabPrivate::PluginViewerContext>::const_iterator it = _imp->currentNodeContext.begin();
             it != _imp->currentNodeContext.end();
             ++it) {
            qDebug() << QString::fromUtf8(it->slot.c_str()) << (QObject*)it->currentNode.lock().get() << it->currentContext.get() << it->currentContext->getContainerWidget();
        }
#       endif
        while ( (int)_imp->currentNodeContext.size() >= maxNodeContextOpened ) {
            const ViewerTabPrivate::PluginViewerContext& oldestNodeViewerInterface = _imp->currentNodeContext.front();
            removeNodeViewerInterface(oldestNodeViewerInterface.currentNode.lock(), false /*permanently*/, false /*setAnother*/);
#           ifdef DEBUG_VIEWERCONTAINER
            qDebug() << "after removal, currentNodeContext(" << (int)_imp->currentNodeContext.size() << "):";
            for (std::list<ViewerTabPrivate::PluginViewerContext>::const_iterator it = _imp->currentNodeContext.begin();
                 it != _imp->currentNodeContext.end();
                 ++it) {
                qDebug() << QString::fromUtf8(it->slot.c_str()) << (QObject*)it->currentNode.lock().get() << it->currentContext.get() << it->currentContext->getContainerWidget();
            }
#           endif
        }
        if ( _imp->currentNodeContext.empty() ) {
            // insert before the viewer
            index = _imp->mainLayout->indexOf(_imp->viewerContainer);
        } else {
            QWidget* container = _imp->currentNodeContext.back().currentContext->getContainerWidget();
            index = _imp->mainLayout->indexOf(container);
#           ifdef DEBUG_VIEWERCONTAINER
            qDebug() << "container" << container << "index" << index;
#           endif
            assert(index != -1);
            if (index >= 0) {
                ++index;
            }
        }
    }
    assert(index >= 0);
    if (index >= 0) {
        assert(newContainer);
        if (newContainer) {
            _imp->mainLayout->insertWidget(index, newContainer);
            {
                QMutexLocker l(&_imp->visibleToolbarsMutex);
                if (_imp->topToolbarVisible) {
                    newContainer->show();
                }
            }
        }
    }
    ViewerTabPrivate::PluginViewerContext p;
    p.slot = slot;
    p.currentNode = n;
    p.currentContext = it->second;
    _imp->currentNodeContext.push_back(p);

    _imp->viewer->redraw();
} // ViewerTab::setPluginViewerInterface

/**
 * @brief Removes the interface associated to the given node.
 * @param permanently The interface is destroyed instead of being hidden
 * @param setAnotherFromSamePlugin If true, if another node of the same plug-in is a candidate for a viewer interface, it will replace the existing
 * viewer interface for this plug-in
 **/
void
ViewerTab::removeNodeViewerInterface(const NodeGuiPtr& n,
                                     bool permanently,
                                     bool setAnotherFromSamePlugin)
{
    ViewerTabPrivate::NodeViewerContextsMap::iterator found = _imp->nodesContext.find(n);

    if ( found == _imp->nodesContext.end() ) {
        return;
    }

    std::string slot = ViewerTabPrivate::getViewerSlot(n->getNode());
    NodeGuiPtr activeNodeForPlugin;
    QToolBar* activeItemToolBar = 0;
    QWidget* activeItemContainer = 0;

    {
        // Keep the iterator under this scope since we erase it
        std::list<ViewerTabPrivate::PluginViewerContext>::iterator foundActive = _imp->findActiveNodeContextForSlot(slot);
        if ( foundActive != _imp->currentNodeContext.end() ) {
            activeNodeForPlugin = foundActive->currentNode.lock();

            if (activeNodeForPlugin == n) {
                activeItemToolBar = foundActive->currentContext->getToolBar();
                activeItemContainer = foundActive->currentContext->getContainerWidget();
                _imp->currentNodeContext.erase(foundActive);
            }
        }
    }

    // Remove the widgets of the current node
    if (activeItemToolBar) {
        int nLayoutItems = _imp->viewerLayout->count();
        for (int i = 0; i < nLayoutItems; ++i) {
            QLayoutItem* item = _imp->viewerLayout->itemAt(i);
            if (item->widget() == activeItemToolBar) {
                activeItemToolBar->hide();
                _imp->viewerLayout->removeItem(item);
                break;
            }
        }
    }
    if (activeItemContainer) {
        int buttonsBarIndex = _imp->mainLayout->indexOf(activeItemContainer);
        assert(buttonsBarIndex >= 0);
        if (buttonsBarIndex >= 0) {
            _imp->mainLayout->removeWidget(activeItemContainer);
        }
        activeItemContainer->hide();
    }

    if (setAnotherFromSamePlugin && activeNodeForPlugin == n) {
        /// If there's another node in the same slot (e.g another curve host), set it as the current interface
        ViewerTabPrivate::NodeViewerContextsMap::iterator newInterface = _imp->nodesContext.end();
        for (ViewerTabPrivate::NodeViewerContextsMap::iterator it2 = _imp->nodesContext.begin(); it2 != _imp->nodesContext.end(); ++it2) {
            NodeGuiPtr otherNode = it2->first.lock();
            if (!otherNode) {
                continue;
            }
            if (otherNode == n) {
                continue;
            }
            if (ViewerTabPrivate::getViewerSlot(otherNode->getNode()) != slot) {
                continue;
            }
            if ( (it2->second != found->second) && it2->first.lock()->isSettingsPanelVisible() ) {
                newInterface = it2;
                break;
            }
        }

        if ( newInterface != _imp->nodesContext.end() ) {
            setPluginViewerInterface( newInterface->first.lock() );
        }
    }


    if (permanently) {
        found->second.reset();
        _imp->nodesContext.erase(found);
    }
} // ViewerTab::removeNodeViewerInterface

/**
 * @brief Get the list of all nodes that have a user interface created on this viewer (but not necessarily displayed)
 * and a list for each plug-in of the active node.
 **/
void
ViewerTab::getNodesViewerInterface(std::list<NodeGuiPtr>* nodesWithUI,
                                   std::list<NodeGuiPtr>* perPluginActiveUI) const
{
    for (ViewerTabPrivate::NodeViewerContextsMap::const_iterator it = _imp->nodesContext.begin(); it != _imp->nodesContext.end(); ++it) {
        NodeGuiPtr n = it->first.lock();
        if (n) {
            nodesWithUI->push_back(n);
        }
    }
    for (std::list<ViewerTabPrivate::PluginViewerContext>::const_iterator it = _imp->currentNodeContext.begin(); it != _imp->currentNodeContext.end(); ++it) {
        NodeGuiPtr n = it->currentNode.lock();
        if (n) {
            perPluginActiveUI->push_back(n);
        }
    }
}

void
ViewerTab::updateSelectedToolForNode(const QString& toolID,
                                     const NodeGuiPtr& node)
{
    ViewerTabPrivate::NodeViewerContextsMap::iterator found = _imp->nodesContext.find(node);

    if ( found == _imp->nodesContext.end() ) {
        // Already exists
        return;
    }
    found->second->setCurrentTool(toolID, false /*notifyNode*/);
}

void
ViewerTab::notifyGuiClosing()
{
    _imp->timeLineGui->discardGuiPointer();
    for (ViewerTabPrivate::NodeViewerContextsMap::iterator it = _imp->nodesContext.begin(); it != _imp->nodesContext.end(); ++it) {
        it->second->notifyGuiClosing();
    }
}

void
ViewerTab::refreshCompositingOperatorFromKnob()
{
    const ViewerCompositingOperatorEnum op = getCompositingOperator();
    const bool noWipe = _imp->viewerNode && _imp->viewerNode->isWipeDisabledByDataKind();

    if (op != eViewerCompositingOperatorNone) {
        _imp->compOperatorPrevious = op;
    }
    _imp->compositingOperator->setCurrentIndex_no_emit((int)op);
    _imp->compositingOperator->setEnabled_natron(!noWipe);
    _imp->secondInputImage->setEnabled_natron(op != eViewerCompositingOperatorNone);

    if ((op == eViewerCompositingOperatorNone) || (_imp->secondInputImage->activeIndex() <= 0)) {
        manageSlotsForInfoWidget(1, false);
        _imp->infoWidget[1]->hide();
    } else {
        manageSlotsForInfoWidget(1, true);
        _imp->infoWidget[1]->show();
    }

    _imp->viewer->update();
}

void
ViewerTab::onCompositingOperatorIndexChanged(int index)
{
    if ((index < (int)eViewerCompositingOperatorNone) || (index > (int)eViewerCompositingOperatorStackOnionSkin)) {
        return;
    }
    editViewerKnobs(tr("Operation"), [this, index]() {
        _imp->setViewerValue<int>(kViewerParamOperation, index);
    });
    refreshCompositingOperatorFromKnob();
}

void
ViewerTab::setCompositingOperator(ViewerCompositingOperatorEnum op)
{
    // While an input is a scene the operator reads as None, and the knob's comes back once none is.
    _imp->setViewerValue<int>(kViewerParamOperation, (int)op);
}

ViewerCompositingOperatorEnum
ViewerTab::getCompositingOperator() const
{
    // MT-safe
    if (!_imp->viewerNode || _imp->viewerNode->isWipeDisabledByDataKind()) {
        return eViewerCompositingOperatorNone;
    }
    const int op = _imp->viewerValue<int>(kViewerParamOperation, (int)eViewerCompositingOperatorNone);
    if ((op < (int)eViewerCompositingOperatorNone) || (op > (int)eViewerCompositingOperatorStackOnionSkin)) {
        return eViewerCompositingOperatorNone;
    }

    return (ViewerCompositingOperatorEnum)op;
}

ViewerCompositingOperatorEnum
ViewerTab::getCompositingOperatorPrevious() const
{
    return _imp->compOperatorPrevious;
}

void
ViewerTab::setInputA(int index)
{
    if (_imp->inputNamesMap.find(index) == _imp->inputNamesMap.end()) {
        return;
    }
    _imp->setViewerValue<int>(kViewerParamAInput, index);
}

void
ViewerTab::setInputB(int index)
{
    if (_imp->inputNamesMap.find(index) == _imp->inputNamesMap.end()) {
        return;
    }
    _imp->setViewerValue<int>(kViewerParamBInput, index);
}

void
ViewerTab::getActiveInputs(int* a,
                           int* b) const
{
    _imp->viewerNode->getActiveInputs(*a, *b);
}

int
ViewerTab::inputIndexForMenuText(const QString& text) const
{
    for (ViewerTabPrivate::InputNamesMap::const_iterator it = _imp->inputNamesMap.begin(); it != _imp->inputNamesMap.end(); ++it) {
        if (it->second.name == text) {
            return it->first;
        }
    }

    return -1;
}

void
ViewerTab::switchInputAAndB()
{
    const int inputAIndex = inputIndexForMenuText(_imp->firstInputImage->getCurrentIndexText());
    const int inputBIndex = inputIndexForMenuText(_imp->secondInputImage->getCurrentIndexText());

    if (inputBIndex == inputAIndex) {
        return;
    }
    editViewerKnobs(tr("Switch Inputs A And B"), [this, inputAIndex, inputBIndex]() {
        _imp->setViewerValue<int>(kViewerParamAInput, inputBIndex);
        _imp->setViewerValue<int>(kViewerParamBInput, inputAIndex);
    });
    refreshInputMenusFromKnobs();
    _imp->viewer->updatePersistentMessage();
}

///Called when the user change the combobox choice
void
ViewerTab::onFirstInputNameChanged(const QString & text)
{
    const int inputIndex = inputIndexForMenuText(text);

    editViewerKnobs(tr("Input A"), [this, inputIndex]() {
        _imp->setViewerValue<int>(kViewerParamAInput, inputIndex);
    });
    refreshInputMenusFromKnobs();
    _imp->viewer->updatePersistentMessage();
}

///Called when the user change the combobox choice
void
ViewerTab::onSecondInputNameChanged(const QString & text)
{
    const int inputIndex = inputIndexForMenuText(text);

    editViewerKnobs(tr("Input B"), [this, inputIndex]() {
        _imp->setViewerValue<int>(kViewerParamBInput, inputIndex);
        // Picking a B input turns the wipe on.
        if ((inputIndex != -1) && (getCompositingOperator() == eViewerCompositingOperatorNone) && !_imp->viewerNode->isWipeDisabledByDataKind()) {
            _imp->setViewerValue<int>(kViewerParamOperation, (int)eViewerCompositingOperatorWipeUnder);
        }
    });
    refreshInputMenusFromKnobs();
    _imp->viewer->updatePersistentMessage();
}

void
ViewerTab::refreshInputMenusFromKnobs()
{
    const int active[2] = {
        _imp->viewerValue<int>(kViewerParamAInput, -1),
        _imp->viewerValue<int>(kViewerParamBInput, -1),
    };
    ComboBox* menus[2] = { _imp->firstInputImage, _imp->secondInputImage };

    // A disconnected input index shows none.
    for (int i = 0; i < 2; ++i) {
        ViewerTabPrivate::InputNamesMap::iterator found = _imp->inputNamesMap.find(active[i]);
        const int menuIndex = (found != _imp->inputNamesMap.end()) ? menus[i]->itemIndex(found->second.name) : -1;
        menus[i]->setCurrentIndex_no_emit(std::max(menuIndex, 0));
    }
    refreshCompositingOperatorFromKnob();
}

///This function is called only when the user changed inputs on the node graph
void
ViewerTab::onActiveInputsChanged()
{
    int activeInputs[2];

    _imp->viewerNode->getActiveInputs(activeInputs[0], activeInputs[1]);
    refreshInputMenusFromKnobs();

    const bool autoWipe = getInternalNode()->isInputChangeRequestedFromViewer();
    if (autoWipe && (activeInputs[0] != -1) && (activeInputs[1] != -1) && (activeInputs[0] != activeInputs[1])
        && (getCompositingOperator() == eViewerCompositingOperatorNone) && !_imp->viewerNode->isWipeDisabledByDataKind()) {
        setCompositingOperator(eViewerCompositingOperatorWipeUnder);
    }
    _imp->viewer->updatePersistentMessage();
}

void
ViewerTab::connectToInput(int inputNb,
                          bool isASide)
{
    InspectorNode* node = dynamic_cast<InspectorNode*>( getInternalNode()->getNode().get() );

    assert(node);
    if (!node) {
        return;
    }
    bool isAutoWipeEnabled = appPTR->getCurrentSettings()->isAutoWipeEnabled();
    if (isAutoWipeEnabled) {
        getInternalNode()->setActivateInputChangeRequestedFromViewer(true);
    }
    node->setActiveInputAndRefresh(inputNb, isASide);
    if (isAutoWipeEnabled) {
        getInternalNode()->setActivateInputChangeRequestedFromViewer(false);
    }
}

void
ViewerTab::connectToAInput(int inputNb)
{
    connectToInput(inputNb, true);
}

void
ViewerTab::connectToBInput(int inputNb)
{
    connectToInput(inputNb, false);
}

void
ViewerTab::onAvailableComponentsChanged()
{
    refreshLayerAndAlphaChannelComboBox();
}

double
ViewerTab::getLockedFps() const
{
    int activeInputs[2];

    _imp->viewerNode->getActiveInputs(activeInputs[0], activeInputs[1]);
    EffectInstancePtr input = activeInputs[0] != -1 ? _imp->viewerNode->getInput(activeInputs[0]) : EffectInstancePtr();
    if (!input && activeInputs[1] != -1) {
        input = _imp->viewerNode->getInput(activeInputs[1]);
    }
    NodeMetadataConstPtr metadata;
    if (input) {
        metadata = input->getMetadata(getTimeLine()->currentFrame(), getCurrentView());
    }

    return metadata ? metadata->getOutputFrameRate() : getGui()->getApp()->getProjectFrameRate();
}

static std::string makeUpFormatName(const RectI& format, double par)
{
    // Format name was empty, too bad, make up one
    std::stringstream ss;
    ss << format.width();
    ss << 'x';
    ss << format.height();
    if (par != 1.) {
        ss << ':';
        ss << QString::number(par, 'f', 2).toStdString();
    }
    return ss.str();
}

void
ViewerTab::setInfoBarAndViewerResolution(const RectI& rect, const RectD& canonicalRect, double par, int texIndex)
{
    std::string formatName, infoBarName;
    if (!getGui()->getApp()->getProject()->getFormatNameFromRect(rect, par, &formatName)) {
        formatName = makeUpFormatName(rect, par);
        infoBarName = formatName;
    } else {
        // If the format has a name, for the info bar also add the resolution
        std::stringstream ss;
        ss << formatName;
        ss << ' ';
        ss << rect.width();
        ss << 'x';
        ss << rect.height();
        infoBarName = ss.str();
    }
    _imp->infoWidget[texIndex]->setResolution(QString::fromUtf8(infoBarName.c_str()));
    _imp->viewer->setFormat(formatName, canonicalRect, par, texIndex);
}

void
ViewerTab::onClipPreferencesChanged()
{
    // A locked fps follows the input's.
    refreshFpsFromKnobs();

    Format format;
    getGui()->getApp()->getProject()->getProjectDefaultFormat(&format);
    RectD canonicalFormat = format.toCanonicalFormat();

    // The inputs' format and PAR can change from one frame to the next: show the current frame's.
    const double time = getTimeLine()->currentFrame();
    const ViewIdx view = getCurrentView();
    int activeInputs[2];
    _imp->viewerNode->getActiveInputs(activeInputs[0], activeInputs[1]);
    for (int i = 0; i < 2; ++i) {
        EffectInstancePtr input;
        if (activeInputs[i] != -1) {
            input = _imp->viewerNode->getInput(activeInputs[i]);
        }
        NodeMetadataConstPtr metadata;
        if (input) {
            metadata = input->getMetadata(time, view);
        }
        if (!metadata) {
            setInfoBarAndViewerResolution(format, canonicalFormat, format.getPixelAspectRatio(), i);
        } else {
            RectI inputFormat = metadata->getOutputFormat();
            double inputPar = metadata->getPixelAspectRatio(-1);
            RectD inputCanonicalFormat = inputFormat.toCanonical_noClipping(0, inputPar);

            setInfoBarAndViewerResolution(inputFormat, inputCanonicalFormat, inputPar, i);
        }
    }

}

NATRON_NAMESPACE_EXIT
