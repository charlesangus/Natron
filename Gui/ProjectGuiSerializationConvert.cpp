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
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QStringList>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/GlobalDefines.h"
#include "Global/MathUtils.h"

#include "Serialization/ProjectGuiSerialization.h"

#include "Engine/Knob.h"
#include "Engine/Node.h"

#include "Gui/DockablePanel.h"
#include "Gui/FloatingWidget.h"
#include "Gui/Gui.h"
#include "Gui/GuiPrivate.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/RegisteredTabs.h"
#include "Gui/Splitter.h"
#include "Gui/TabWidget.h"

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

NATRON_NAMESPACE_ANONYMOUS_EXIT

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

NATRON_NAMESPACE_EXIT
