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

#include "ProjectGui.h"

#include <algorithm> // min, find
#include <cassert>
#include <cmath> // abs
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QGuiApplication>
#include <QPointF>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QStringList>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/GlobalDefines.h"
#include "Global/MathUtils.h"

#include "Serialization/BasicSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/ProjectGuiSerialization.h"

#include "Engine/AppManager.h" // interpretPythonScript, makeNameScriptFriendly
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/MissingPluginNode.h"
#include "Engine/Project.h"
#include "Engine/PyParameter.h" // Param
#include "Engine/RectD.h"
#include "Engine/Settings.h"
#include "Engine/ViewerInstance.h"

#include "Gui/BackdropGui.h"
#include "Gui/DockablePanel.h"
#include "Gui/FloatingWidget.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiPrivate.h"
#include "Gui/Histogram.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/PythonPanels.h"
#include "Gui/RegisteredTabs.h"
#include "Gui/ScriptEditor.h"
#include "Gui/Splitter.h"
#include "Gui/TabWidget.h"
#include "Gui/ViewerGL.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

void
paneToSerialization(TabWidget* tab,
                    Serialization::PaneLayoutSerialization* serialization)
{
    QStringList tabScriptNames = tab->getTabScriptNames();

    for (int i = 0; i < tabScriptNames.size(); ++i) {
        serialization->tabs.push_back(tabScriptNames[i].toStdString());
    }
    serialization->currentIndex = tab->activeIndex();
    serialization->name = tab->objectName_mt_safe().toStdString();
    serialization->isAnchor = tab->isAnchor();
}

void
splitterToSerialization(Splitter* splitter,
                        Serialization::SplitterSerialization* serialization)
{
    serialization->sizes = splitter->serializeNatron().toStdString();
    OrientationEnum orientation = eOrientationHorizontal;
    switch (splitter->orientation()) {
    case Qt::Horizontal:
        orientation = eOrientationHorizontal;
        break;
    case Qt::Vertical:
        orientation = eOrientationVertical;
        break;
    default:
        assert(false);
        break;
    }
    serialization->orientation = (int)orientation;

    std::list<QWidget*> children;
    splitter->getChildren_mt_safe(children);
    assert(children.size() == 2);

    for (std::list<QWidget*>::iterator it = children.begin(); it != children.end(); ++it) {
        Serialization::SplitterChildSerialization child;
        Splitter* isSplitter = dynamic_cast<Splitter*>(*it);
        TabWidget* isTabWidget = dynamic_cast<TabWidget*>(*it);
        if (isSplitter) {
            child.splitter = std::make_shared<Serialization::SplitterSerialization>();
            splitterToSerialization(isSplitter, child.splitter.get());
        } else if (isTabWidget) {
            child.pane = std::make_shared<Serialization::PaneLayoutSerialization>();
            paneToSerialization(isTabWidget, child.pane.get());
        }
        serialization->children.push_back(child);
    }
}

void
applicationWindowToSerialization(bool isMainWindow,
                                 SerializableWindow* widget,
                                 Serialization::ApplicationWindowSerialization* serialization)
{
    serialization->isMainWindow = isMainWindow;
    widget->getMtSafePosition(serialization->positionX, serialization->positionY);
    widget->getMtSafeWindowSize(serialization->width, serialization->height);

    if (isMainWindow) {
        Gui* gui = dynamic_cast<Gui*>(widget);
        assert(gui);
        if (gui) {
            QWidget* centralWidget = gui->getCentralWidget();
            Splitter* isSplitter = dynamic_cast<Splitter*>(centralWidget);
            TabWidget* isTabWidget = dynamic_cast<TabWidget*>(centralWidget);

            assert(isSplitter || isTabWidget);

            if (isSplitter) {
                serialization->childSplitter = std::make_shared<Serialization::SplitterSerialization>();
                splitterToSerialization(isSplitter, serialization->childSplitter.get());
            } else if (isTabWidget) {
                serialization->childPane = std::make_shared<Serialization::PaneLayoutSerialization>();
                paneToSerialization(isTabWidget, serialization->childPane.get());
            }
        }
    } else {
        FloatingWidget* isFloating = dynamic_cast<FloatingWidget*>(widget);
        assert(isFloating);
        if (isFloating) {
            QWidget* embedded = isFloating->getEmbeddedWidget();
            Splitter* isSplitter = dynamic_cast<Splitter*>(embedded);
            TabWidget* isTabWidget = dynamic_cast<TabWidget*>(embedded);
            DockablePanel* isPanel = dynamic_cast<DockablePanel*>(embedded);
            assert(isSplitter || isTabWidget || isPanel);

            if (isSplitter) {
                serialization->childSplitter = std::make_shared<Serialization::SplitterSerialization>();
                splitterToSerialization(isSplitter, serialization->childSplitter.get());
            } else if (isTabWidget) {
                serialization->childPane = std::make_shared<Serialization::PaneLayoutSerialization>();
                paneToSerialization(isTabWidget, serialization->childPane.get());
            } else if (isPanel) {
                // Only the project settings panel belongs to a holder without a script name.
                NamedKnobHolder* isNamedHolder = dynamic_cast<NamedKnobHolder*>(isPanel->getHolder());
                if (isNamedHolder) {
                    serialization->childDockablePanel = isNamedHolder->getScriptName_mt_safe();
                } else {
                    serialization->childDockablePanel = NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME;
                }
            }
        }
    }
}

void
pythonPanelToSerialization(NATRON_PYTHON_NAMESPACE::PyPanel* panel,
                           const std::string& pythonFunction,
                           Serialization::PythonPanelSerialization* serialization)
{
    serialization->name = panel->getLabel();
    serialization->pythonFunction = pythonFunction;
    std::list<NATRON_PYTHON_NAMESPACE::Param*> parameters = panel->getParams();
    for (std::list<NATRON_PYTHON_NAMESPACE::Param*>::iterator it = parameters.begin(); it != parameters.end(); ++it) {
        KnobIPtr knob = (*it)->getInternalKnob();
        KnobGroup* isGroup = dynamic_cast<KnobGroup*>(knob.get());
        KnobPage* isPage = dynamic_cast<KnobPage*>(knob.get());
        KnobButton* isButton = dynamic_cast<KnobButton*>(knob.get());

        if (!isGroup && !isPage && !isButton) {
            serialization->knobs.push_back(Serialization::KnobSerialization());
            knob->toSerialization(&serialization->knobs.back());
        }
        delete *it;
    }

    serialization->userData = panel->save_serialization_thread().toStdString();
}

void
restorePaneFromSerialization(TabWidget* pane,
                             const Serialization::PaneLayoutSerialization& serialization)
{
    QString availableName = pane->getGui()->getAvailablePaneName(QString::fromUtf8(serialization.name.c_str()));

    pane->setObjectName_mt_safe(availableName);
    pane->setAsAnchor(serialization.isAnchor);
    const RegisteredTabs& tabs = pane->getGui()->getRegisteredTabs();
    for (std::list<std::string>::const_iterator it = serialization.tabs.begin(); it != serialization.tabs.end(); ++it) {
        RegisteredTabs::const_iterator found = tabs.find(*it);

        // A tab the current session does not have is skipped.
        if (found != tabs.end()) {
            TabWidget::moveTab(found->second.first, found->second.second, pane);
        }
    }
    pane->makeCurrentTab(serialization.currentIndex);
}

void
restoreSplitterFromSerialization(Gui* gui,
                                 Splitter* splitter,
                                 const Serialization::SplitterSerialization& serialization)
{
    Qt::Orientation qtOrientation;
    OrientationEnum orientation = (OrientationEnum)serialization.orientation;

    switch (orientation) {
    case eOrientationHorizontal:
        qtOrientation = Qt::Horizontal;
        break;
    case eOrientationVertical:
        qtOrientation = Qt::Vertical;
        break;
    default:
        throw std::runtime_error("Unrecognized splitter orientation");
        break;
    }
    splitter->setOrientation(qtOrientation);

    if (serialization.children.size() != 2) {
        throw std::runtime_error("Splitter has a child count that is not 2");
    }

    for (std::vector<Serialization::SplitterChildSerialization>::const_iterator it = serialization.children.begin();
         it != serialization.children.end(); ++it) {
        if (it->splitter) {
            Splitter* child = new Splitter(splitter);
            splitter->addWidget_mt_safe(child);
            restoreSplitterFromSerialization(gui, child, *it->splitter);
        } else {
            if (!it->pane) {
                throw std::runtime_error("Splitter child is neither a splitter nor a pane");
            }
            TabWidget* pane = new TabWidget(gui, splitter);
            gui->registerPane(pane);
            splitter->addWidget_mt_safe(pane);
            restorePaneFromSerialization(pane, *it->pane);
        }
    }

    splitter->restoreNatron(QString::fromUtf8(serialization.sizes.c_str()));
}

void
restoreNodeGuiFromSerialization(Gui* gui,
                                const std::map<std::string, Serialization::ViewerDataSerialization>& viewers,
                                const SettingsPtr& settings,
                                double leftBound,
                                double rightBound,
                                const Serialization::NodeGuiSerialization& serialization)
{
    const std::string& name = serialization.nodeName;
    NodePtr internalNode = gui->getApp()->getProject()->getNodeByFullySpecifiedName(name);

    if (!internalNode) {
        return;
    }

    NodeGuiIPtr nodeGuiInterface = internalNode->getNodeGui();
    assert(nodeGuiInterface);
    NodeGuiPtr nodeGui = std::dynamic_pointer_cast<NodeGui>(nodeGuiInterface);

    nodeGui->refreshPosition(serialization.positionX, serialization.positionY, true);

    if ((serialization.previewEnabled && !nodeGui->getNode()->isPreviewEnabled()) || (!serialization.previewEnabled && nodeGui->getNode()->isPreviewEnabled())) {
        nodeGui->togglePreview();
    }

    BackdropGui* isBackdrop = dynamic_cast<BackdropGui*>(nodeGui.get());
    if (!isBackdrop) {
        nodeGui->restoreCategoryAndUserColor(serialization);
    }

    if (serialization.colorWasFound) {
        if (isBackdrop) {
            QColor defaultColor;
            NodeGui::getCategoryColor(nodeGui->getNode(), &defaultColor);
            float defaultR, defaultG, defaultB;
            defaultColor.getRgbF(&defaultR, &defaultG, &defaultB);

            // A colour within 0.05 of the default is left to follow the default.
            if ((std::abs(serialization.colorR - defaultR) > 0.05) || (std::abs(serialization.colorG - defaultG) > 0.05) || (std::abs(serialization.colorB - defaultB) > 0.05)) {
                QColor color;
                color.setRgbF(serialization.colorR, serialization.colorG, serialization.colorB);
                nodeGui->setCurrentColor(color);
            }
        }

        if (serialization.hasOverlayColor) {
            QColor overlayColor;
            overlayColor.setRgbF(serialization.overlayColorR, serialization.overlayColorG, serialization.overlayColorB);
            nodeGui->setOverlayColor(overlayColor);
        }

        if (isBackdrop) {
            isBackdrop->resize(serialization.width, serialization.height, true);
        }
    }

    ViewerInstance* viewer = nodeGui->getNode()->isEffectViewer();
    if (viewer) {
        std::map<std::string, Serialization::ViewerDataSerialization>::const_iterator found = viewers.find(name);
        if (found != viewers.end()) {
            const Serialization::ViewerDataSerialization& viewerData = found->second;
            ViewerTab* tab = gui->getApp()->getGui()->getViewerTabForInstance(viewer);
            tab->setProjection(viewerData.zoomLeft, viewerData.zoomBottom, viewerData.zoomFactor, 1.);
            tab->setChannels(viewerData.channels);
            tab->setDisplayTransform(viewerData.display, viewerData.view, viewerData.look);
            tab->setGain(viewerData.gain);
            tab->setGamma(viewerData.gamma);
            tab->setUserRoIEnabled(viewerData.userRoIEnabled);
            tab->setAutoContrastEnabled(viewerData.autoContrastEnabled);
            tab->setUserRoI(RectD(viewerData.userRoI.x1, viewerData.userRoI.y1, viewerData.userRoI.x2, viewerData.userRoI.y2));
            tab->setClipToProject(viewerData.isClippedToProject);
            tab->setRenderScaleActivated(viewerData.renderScaleActivated);
            tab->setMipmapLevel(viewerData.mipmapLevel);
            tab->setCompositingOperator((ViewerCompositingOperatorEnum)viewerData.wipeCompositingOperator);
            tab->setZoomOrPannedSinceLastFit(viewerData.zoomOrPanSinceLastFit);
            tab->setTopToolbarVisible(viewerData.topToolbarVisible);
            tab->setLeftToolbarVisible(viewerData.leftToolbarVisible);
            tab->setRightToolbarVisible(viewerData.rightToolbarVisible);
            tab->setPlayerVisible(viewerData.playerVisible);
            tab->setInfobarVisible(viewerData.infobarVisible);
            tab->setTimelineVisible(viewerData.timelineVisible);
            tab->setCheckerboardEnabled(viewerData.checkerboardEnabled);
            tab->setFullFrameProcessing(viewerData.isFullFrameProcessEnabled);
            if (!viewerData.layerName.empty()) {
                tab->setCurrentLayers(QString::fromUtf8(viewerData.layerName.c_str()), QString::fromUtf8(viewerData.alphaLayerName.c_str()));
            }

            if (viewerData.isInputAPaused && viewerData.isInputBPaused) {
                tab->setViewerPaused(true, true);
            } else if (viewerData.isInputAPaused && !viewerData.isInputBPaused) {
                tab->setViewerPaused(true, false);
            }
            if (viewerData.activeInputA >= 0) {
                tab->setInputA(viewerData.activeInputA);
            }
            if (viewerData.activeInputB >= 0) {
                tab->setInputB(viewerData.activeInputB);
            }
            tab->setFrameRange(viewerData.leftBound, viewerData.rightBound);
            tab->setFrameRangeEdited(leftBound != viewerData.leftBound || rightBound != viewerData.rightBound);
            if (!viewerData.fpsLocked) {
                tab->setDesiredFps(viewerData.fps);
            }
            tab->setFPSLocked(viewerData.fpsLocked);
        }
    }

    if (serialization.selected) {
        gui->getNodeGraph()->selectNode(nodeGui, true);
    }

    if (dynamic_cast<MissingPluginNode*>(internalNode->getEffectInstance().get())) {
        return;
    }

    for (std::list<Serialization::NodeGuiSerializationPtr>::const_iterator it = serialization.children.begin(); it != serialization.children.end(); ++it) {
        restoreNodeGuiFromSerialization(gui, viewers, settings, leftBound, rightBound, **it);
    }
} // restoreNodeGuiFromSerialization

NATRON_NAMESPACE_ANONYMOUS_EXIT

void
NodeGui::toSerialization(Serialization::NodeGuiSerialization* serialization) const
{
    assert(serialization);
    *serialization = Serialization::NodeGuiSerialization();

    NodePtr node = getNode();

    serialization->nodeName = node->getFullyQualifiedName();
    QPointF position = getPos_mt_safe();
    serialization->positionX = position.x();
    serialization->positionY = position.y();
    getSize(&serialization->width, &serialization->height);
    serialization->previewEnabled = node->isPreviewEnabled();
    QColor color = getCurrentColor();
    serialization->colorR = color.redF();
    serialization->colorG = color.greenF();
    serialization->colorB = color.blueF();
    QColor userColor;
    if (getUserColor(&userColor)) {
        serialization->hasUserColor = true;
        serialization->userColorR = userColor.redF();
        serialization->userColorG = userColor.greenF();
        serialization->userColorB = userColor.blueF();
    }
    serialization->selected = isSelected();

    serialization->hasOverlayColor = getOverlayColor(&serialization->overlayColorR, &serialization->overlayColorG, &serialization->overlayColorB);

    NodeGroup* isGroup = node->isEffectGroup();
    if (isGroup) {
        NodesList nodes;
        isGroup->getActiveNodes(&nodes);

        for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if (!(*it)->isPartOfProject()) {
                continue;
            }
            NodeGuiIPtr childGuiInterface = (*it)->getNodeGui();
            if (!childGuiInterface) {
                continue;
            }
            NodeGui* childGui = dynamic_cast<NodeGui*>(childGuiInterface.get());
            if (!childGui) {
                continue;
            }
            Serialization::NodeGuiSerializationPtr child = std::make_shared<Serialization::NodeGuiSerialization>();
            childGui->toSerialization(child.get());
            serialization->children.push_back(child);
        }
    }
}

void
Gui::toSerialization(Serialization::GuiLayoutSerialization* serialization)
{
    assert(serialization);
    *serialization = Serialization::GuiLayoutSerialization();

    serialization->windows.push_back(Serialization::ApplicationWindowSerialization());
    applicationWindowToSerialization(true, this, &serialization->windows.back());

    std::list<FloatingWidget*> floatingWindows = getFloatingWindows();
    for (std::list<FloatingWidget*>::iterator it = floatingWindows.begin(); it != floatingWindows.end(); ++it) {
        serialization->windows.push_back(Serialization::ApplicationWindowSerialization());
        applicationWindowToSerialization(false, *it, &serialization->windows.back());
    }
}

void
Gui::restoreLayout(bool wipePrevious,
                   const Serialization::GuiLayoutSerialization& layoutSerialization)
{
    if (wipePrevious) {
        wipeLayout();
    }

    std::list<const Serialization::ApplicationWindowSerialization*> floatingDockablePanels;
    QScreen* desktop = QGuiApplication::primaryScreen();
    QRect screen = desktop->availableGeometry();

    for (std::list<Serialization::ApplicationWindowSerialization>::const_iterator it = layoutSerialization.windows.begin();
         it != layoutSerialization.windows.end(); ++it) {
        QWidget* mainWidget = 0;

        if (it->childPane) {
            TabWidget* centralWidget = new TabWidget(this);
            registerPane(centralWidget);
            restorePaneFromSerialization(centralWidget, *it->childPane);
            mainWidget = centralWidget;
        } else if (it->childSplitter) {
            Splitter* centralWidget = new Splitter(this);
            restoreSplitterFromSerialization(this, centralWidget, *it->childSplitter);
            mainWidget = centralWidget;
        } else if (!it->childDockablePanel.empty()) {
            // The settings panel is floated once every window exists.
            assert(!it->isMainWindow);
            floatingDockablePanels.push_back(&(*it));
            continue;
        }

        assert(mainWidget);
        if (!mainWidget) {
            continue;
        }
        QWidget* window;
        if (it->isMainWindow) {
            _imp->_leftRightSplitter->addWidget_mt_safe(mainWidget);
            window = this;
        } else {
            FloatingWidget* floatingWindow = new FloatingWidget(this, this);
            floatingWindow->setWidget(mainWidget);
            registerFloatingWindow(floatingWindow);
            window = floatingWindow;
        }

        int width = std::min(it->width, screen.width());
        int height = std::min(it->height, screen.height());
        window->resize(width, height);
        // If the screen size changed, make sure at least 50x50 pixels of the window are visible
        int positionX = MathUtils::clamp(it->positionX, screen.left(), screen.right() - 50);
        int positionY = MathUtils::clamp(it->positionY, screen.top(), screen.bottom() - 50);
        window->move(QPoint(positionX, positionY));
    }

    for (std::list<const Serialization::ApplicationWindowSerialization*>::iterator it = floatingDockablePanels.begin();
         it != floatingDockablePanels.end(); ++it) {
        assert(!(*it)->childDockablePanel.empty());
        FloatingWidget* floatingWindow = 0;
        if ((*it)->childDockablePanel == NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME) {
            floatingWindow = _imp->_projectGui->getPanel()->floatPanel();
        } else {
            const NodesGuiList& nodes = getNodeGraph()->getAllActiveNodes();
            DockablePanel* panel = 0;
            for (NodesGuiList::const_iterator it2 = nodes.begin(); it2 != nodes.end(); ++it2) {
                if ((*it2)->getNode()->getScriptName() == (*it)->childDockablePanel) {
                    (*it2)->ensurePanelCreated();
                    NodeSettingsPanel* nodeSettings = (*it2)->getSettingPanel();
                    if (nodeSettings) {
                        nodeSettings->floatPanel();
                        panel = nodeSettings;
                    }
                    break;
                }
            }
            if (panel) {
                QWidget* parent = panel->parentWidget();
                while (!floatingWindow && parent) {
                    floatingWindow = dynamic_cast<FloatingWidget*>(parent);
                    parent = parent->parentWidget();
                }
            }
        }
        assert(floatingWindow);
        floatingWindow->move(QPoint((*it)->positionX, (*it)->positionY));
        floatingWindow->resize(std::min((*it)->width, screen.width()), std::min((*it)->height, screen.height()));
    }
} // restoreLayout

void
ProjectGui::toSerialization(Serialization::ProjectGuiSerialization* serialization) const
{
    assert(serialization);
    *serialization = Serialization::ProjectGuiSerialization();

    NodesList activeNodes;
    getInternalProject()->getActiveNodesExpandGroups(&activeNodes);

    for (NodesList::iterator it = activeNodes.begin(); it != activeNodes.end(); ++it) {
        NodeGuiIPtr nodeGuiInterface = (*it)->getNodeGui();
        if (!nodeGuiInterface) {
            continue;
        }
        NodeGuiPtr nodeGui = std::dynamic_pointer_cast<NodeGui>(nodeGuiInterface);

        if (!nodeGui->isVisible()) {
            continue;
        }
        NodeCollectionPtr isInCollection = (*it)->getGroup();
        NodeGroup* isCollectionAGroup = dynamic_cast<NodeGroup*>(isInCollection.get());
        if (!isCollectionAGroup) {
            // A node inside a group is saved with its group.
            serialization->nodes.push_back(Serialization::NodeGuiSerialization());
            nodeGui->toSerialization(&serialization->nodes.back());
        }
        ViewerInstance* viewer = (*it)->isEffectViewer();
        if (!viewer) {
            continue;
        }
        ViewerTab* tab = getGui()->getViewerTabForInstance(viewer);
        assert(tab);
        if (!tab) {
            continue;
        }
        ViewerGL* viewerGL = tab->getViewer();
        if (!viewerGL) {
            continue;
        }
        Serialization::ViewerDataSerialization viewerData;
        double zoomAspectRatio;
        viewerGL->getProjection(&viewerData.zoomLeft, &viewerData.zoomBottom, &viewerData.zoomFactor, &zoomAspectRatio);
        RectD userRoI = viewerGL->getUserRegionOfInterest();
        viewerData.userRoI.x1 = userRoI.x1;
        viewerData.userRoI.y1 = userRoI.y1;
        viewerData.userRoI.x2 = userRoI.x2;
        viewerData.userRoI.y2 = userRoI.y2;
        viewerData.userRoIEnabled = viewerGL->isUserRegionOfInterestEnabled();
        viewerData.isClippedToProject = tab->isClippedToProject();
        viewerData.autoContrastEnabled = tab->isAutoContrastEnabled();
        viewerData.gain = tab->getGain();
        viewerData.gamma = tab->getGamma();
        tab->getDisplayTransform(&viewerData.display, &viewerData.view, &viewerData.look);
        viewerData.channels = tab->getChannelsString();
        viewerData.renderScaleActivated = tab->getRenderScaleActivated();
        viewerData.mipmapLevel = tab->getMipmapLevel();
        viewerData.zoomOrPanSinceLastFit = tab->getZoomOrPannedSinceLastFit();
        viewerData.wipeCompositingOperator = (int)tab->getCompositingOperator();
        viewerData.leftToolbarVisible = tab->isLeftToolbarVisible();
        viewerData.rightToolbarVisible = tab->isRightToolbarVisible();
        viewerData.topToolbarVisible = tab->isTopToolbarVisible();
        viewerData.infobarVisible = tab->isInfobarVisible();
        viewerData.playerVisible = tab->isPlayerVisible();
        viewerData.timelineVisible = tab->isTimelineVisible();
        viewerData.checkerboardEnabled = tab->isCheckerboardEnabled();
        viewerData.isFullFrameProcessEnabled = tab->isFullFrameProcessingEnabled();
        viewerData.fps = tab->getDesiredFps();
        viewerData.fpsLocked = tab->isFPSLocked();
        viewerData.isInputAPaused = tab->isViewerPaused(0);
        viewerData.isInputBPaused = tab->isViewerPaused(1);
        viewerData.layerName = tab->getCurrentLayerName().toStdString();
        viewerData.alphaLayerName = tab->getCurrentAlphaLayerName().toStdString();
        tab->getTimelineBounds(&viewerData.leftBound, &viewerData.rightBound);
        tab->getActiveInputs(&viewerData.activeInputA, &viewerData.activeInputB);
        serialization->viewers.insert(std::make_pair(viewer->getNode()->getScriptName_mt_safe(), viewerData));
    }

    getGui()->toSerialization(&serialization->layout);

    std::list<Histogram*> histograms = getGui()->getHistograms_mt_safe();
    for (std::list<Histogram*>::const_iterator it = histograms.begin(); it != histograms.end(); ++it) {
        serialization->histograms.push_back((*it)->objectName().toStdString());
    }

    std::list<DockablePanel*> panels = getGui()->getVisiblePanels_mt_safe();
    for (std::list<DockablePanel*>::iterator it = panels.begin(); it != panels.end(); ++it) {
        if (!(*it)->isVisible()) {
            continue;
        }
        KnobHolder* holder = (*it)->getHolder();
        assert(holder);

        EffectInstance* isEffect = dynamic_cast<EffectInstance*>(holder);
        Project* isProject = dynamic_cast<Project*>(holder);
        bool isMinimized = (*it)->isMinimized();
        bool hideUnmodified = (*it)->isHideUnmodified();

        if (isProject) {
            serialization->openedPanels.push_back(NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME);
            if (isMinimized) {
                serialization->openedPanelsMinimized.push_back(NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME);
            }
        } else if (isEffect) {
            std::string nodeName = isEffect->getNode()->getFullyQualifiedName();
            serialization->openedPanels.push_back(nodeName);
            if (isMinimized) {
                serialization->openedPanelsMinimized.push_back(nodeName);
            }
            if (hideUnmodified) {
                serialization->openedPanelsHideUnmodified.push_back(nodeName);
            }
        }
    }

    serialization->scriptEditorInput = getGui()->getScriptEditor()->getAutoSavedScript().toStdString();

    std::map<NATRON_PYTHON_NAMESPACE::PyPanel*, std::string> pythonPanels = getGui()->getPythonPanels();
    for (std::map<NATRON_PYTHON_NAMESPACE::PyPanel*, std::string>::iterator it = pythonPanels.begin(); it != pythonPanels.end(); ++it) {
        serialization->pythonPanels.push_back(Serialization::PythonPanelSerialization());
        pythonPanelToSerialization(it->first, it->second, &serialization->pythonPanels.back());
    }
} // ProjectGui::toSerialization

void
ProjectGui::fromSerialization(const Serialization::ProjectGuiSerialization& serialization,
                              bool isAutosave)
{
    double leftBound, rightBound;
    _project.lock()->getFrameRange(&leftBound, &rightBound);

    SettingsPtr settings = appPTR->getCurrentSettings();
    for (std::list<Serialization::NodeGuiSerialization>::const_iterator it = serialization.nodes.begin(); it != serialization.nodes.end(); ++it) {
        restoreNodeGuiFromSerialization(_gui, serialization.viewers, settings, leftBound, rightBound, *it);
    }

    // getVisibleNodes() only covers the root graph's active nodes; walk every node of every
    // sub-graph too, so a Group's own pipes are repainted right after load as well.
    NodesList allNodes;
    _project.lock()->getNodes_recursive(allNodes, true);
    for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
        NodeGuiPtr nodeGui = std::dynamic_pointer_cast<NodeGui>((*it)->getNodeGui());
        if (nodeGui) {
            nodeGui->refreshEdges();
            nodeGui->refreshKnobLinks();
        }
    }

    // we have to give the tr() context explicitly due to a bug in lupdate
    _gui->getApp()->updateProjectLoadStatus(QObject::tr("Restoring settings panels", "ProjectGui"));

    // Panels are opened bottom up so the first saved one ends on top.
    for (std::list<std::string>::const_reverse_iterator it = serialization.openedPanels.rbegin(); it != serialization.openedPanels.rend(); ++it) {
        bool isMinimized = (std::find(serialization.openedPanelsMinimized.begin(), serialization.openedPanelsMinimized.end(), *it) != serialization.openedPanelsMinimized.end());
        bool hideUnmodified = (std::find(serialization.openedPanelsHideUnmodified.begin(), serialization.openedPanelsHideUnmodified.end(), *it) != serialization.openedPanelsHideUnmodified.end());
        if (*it == NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME) {
            _gui->setVisibleProjectSettingsPanel(isMinimized);
        } else {
            NodePtr node = getInternalProject()->getNodeByFullySpecifiedName(*it);
            if (node) {
                NodeGuiIPtr nodeGuiInterface = node->getNodeGui();
                assert(nodeGuiInterface);
                NodeGui* nodeGui = dynamic_cast<NodeGui*>(nodeGuiInterface.get());
                assert(nodeGui);
                if (nodeGui) {
                    nodeGui->setVisibleSettingsPanel(true, isMinimized, hideUnmodified);
                }
            }
        }
    }

    if (!serialization.pythonPanels.empty()) {
        // we have to give the tr() context explicitly due to a bug in lupdate
        _gui->getApp()->updateProjectLoadStatus(QObject::tr("Restoring user panels", "ProjectGui"));

        std::string appID = _gui->getApp()->getAppIDString();
        std::string err;
        std::string script = "app = " + appID + '\n';
        bool ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0);
        assert(ok);
        if (!ok) {
            // we have to give the tr() context explicitly due to a bug in lupdate
            throw std::runtime_error(QObject::tr("ProjectGui::load(): interpretPythonScript(%1) failed!", "ProjectGui").arg(QString::fromUtf8(script.c_str())).toStdString());
        }
    }
    for (std::list<Serialization::PythonPanelSerialization>::const_iterator it = serialization.pythonPanels.begin(); it != serialization.pythonPanels.end(); ++it) {
        std::string script = it->pythonFunction + "()\n";
        std::string err, output;
        if (!NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output)) {
            _gui->getApp()->appendToScriptEditor(err);
        } else {
            if (!output.empty()) {
                _gui->getApp()->appendToScriptEditor(output);
            }
        }
        const RegisteredTabs& registeredTabs = _gui->getRegisteredTabs();
        RegisteredTabs::const_iterator found = registeredTabs.find(it->name);
        if (found != registeredTabs.end()) {
            NATRON_PYTHON_NAMESPACE::PyPanel* panel = dynamic_cast<NATRON_PYTHON_NAMESPACE::PyPanel*>(found->second.first);
            if (panel) {
                panel->restore(QString::fromUtf8(it->userData.c_str()));
                for (std::list<Serialization::KnobSerialization>::const_iterator it2 = it->knobs.begin(); it2 != it->knobs.end(); ++it2) {
                    const std::string serializedName = NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(it2->name);
                    NATRON_PYTHON_NAMESPACE::Param* param = panel->getParam(QString::fromUtf8(serializedName.c_str()));
                    if (param) {
                        KnobIPtr internalKnob = param->getInternalKnob();
                        KnobIPtr serializedKnob = internalKnob ? createDetachedKnobFromSerialization(completeKnobSerializationForKnob(*internalKnob, *it2)) : KnobIPtr();
                        if (serializedKnob) {
                            internalKnob->clone(serializedKnob);
                        }
                        delete param;
                    }
                }
            }
        }
    }

    // we have to give the tr() context explicitly due to a bug in lupdate
    _gui->getApp()->updateProjectLoadStatus(QObject::tr("Restoring layout", "ProjectGui"));

    // An auto-save always restores its workspace.
    bool loadWorkspace = isAutosave || appPTR->getCurrentSettings()->getLoadProjectWorkspce();
    if (loadWorkspace) {
        _gui->restoreLayout(true, serialization.layout);
    }

    for (std::list<std::string>::const_iterator it = serialization.histograms.begin(); it != serialization.histograms.end(); ++it) {
        Histogram* histogram = _gui->addNewHistogram();
        histogram->setObjectName(QString::fromUtf8((*it).c_str()));
        _gui->appendTabToDefaultViewerPane(histogram, histogram);
    }

    _gui->getScriptEditor()->setInputScript(QString::fromUtf8(serialization.scriptEditorInput.c_str()));
    _gui->centerAllNodeGraphsWithTimer();
} // ProjectGui::fromSerialization

NATRON_NAMESPACE_EXIT
