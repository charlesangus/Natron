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

#include "Gui.h"
#include "GuiPrivate.h"

#include <algorithm> // min, max
#include <cassert>
#include <cstdio>
#include <exception>
#include <istream>
#include <iterator>
#include <list>
#include <map>
#include <string>

#include <QAction>
#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include "Global/FStreamsSupport.h"

#include "Engine/AppManager.h" // Dialogs
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

#include "Gui/DockablePanel.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/Histogram.h"
#include "Gui/Menu.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/ProjectGui.h"
#include "Gui/SequenceFileDialog.h"
#include "Gui/Splitter.h"
#include "Gui/TabWidget.h"
#include "Gui/ViewerTab.h"

#include "Serialization/ProjectGuiSerialization.h"
#include "Serialization/Yaml/ProjectGuiYaml.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER

namespace {

// Reads the layout file at filename. On failure, error says why in words meant for the user and
// unreadable tells whether the file could not even be opened.
bool
readLayoutFile(const std::string& filename,
               Serialization::GuiLayoutSerialization* layout,
               QString* error,
               bool* unreadable = nullptr)
{
    FStreamsSupport::ifstream ifile;
    FStreamsSupport::open(&ifile, filename);
    if (unreadable) {
        *unreadable = !ifile;
    }
    if (!ifile) {
        *error = Gui::tr("Failed to open file: %1").arg(QString::fromUtf8(filename.c_str()));

        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(ifile)), std::istreambuf_iterator<char>());
    if (Serialization::isXmlDocument(text)) {
        *error = Gui::tr("The layout %1 uses the old XML .nl format, which this version of Natron cannot open (no compatibility, by design).").arg(QString::fromUtf8(filename.c_str()));

        return false;
    }
    try {
        *layout = Serialization::decodeGuiLayoutYaml(text);
    } catch (const std::exception& e) {
        *error = Gui::tr("Cannot read the layout %1: %2").arg(QString::fromUtf8(filename.c_str())).arg(QString::fromUtf8(e.what()));

        return false;
    }
    if (layout->windows.empty()) {
        *error = Gui::tr("The layout %1 holds no window.").arg(QString::fromUtf8(filename.c_str()));

        return false;
    }

    return true;
}

} // anonymous namespace

void
Gui::importLayout()
{
    std::vector<std::string> filters;

    filters.push_back(NATRON_LAYOUT_FILE_EXT);
    SequenceFileDialog dialog( this, filters, false, SequenceFileDialog::eFileDialogModeOpen, _imp->_lastLoadProjectOpenedDir.toStdString(), this, false );
    if ( dialog.exec() ) {
        std::string filename = dialog.selectedFiles();
        Serialization::GuiLayoutSerialization layoutSerialization;
        QString error;
        if (!readLayoutFile(filename, &layoutSerialization, &error)) {
            Dialogs::errorDialog(tr("Error").toStdString(), error.toStdString(), false);

            return;
        }
        restoreLayout(true, layoutSerialization);
    }
}

void
Gui::createDefaultLayoutInternal(bool wipePrevious)
{
    if (wipePrevious) {
        wipeLayout();
    }

    const std::string fileLayout = appPTR->getCurrentSettings()->getDefaultLayoutFile();
    if (fileLayout.empty()) {
        createDefaultLayout1();

        return;
    }
    Serialization::GuiLayoutSerialization layoutSerialization;
    QString error;
    bool unreadable = false;
    if (!readLayoutFile(fileLayout, &layoutSerialization, &error, &unreadable)) {
        createDefaultLayout1();
        // A default layout file that has gone away is not worth a dialog on every launch.
        if (!unreadable) {
            Dialogs::errorDialog(tr("Error").toStdString(), error.toStdString(), false);
        }

        return;
    }
    restoreLayout(false, layoutSerialization);
}

void
Gui::restoreDefaultLayout()
{
    _imp->rebuildingLayout = true;
    try {
        createDefaultLayoutInternal(true);
    } catch (...) {
        _imp->rebuildingLayout = false;
        throw;
    }
    _imp->rebuildingLayout = false;
    // Only the tab each pane ends up showing is shown, so a GL tab passed over on the way builds no
    // context for nothing.
    const std::list<TabWidget*> panes = getPanes();
    for (TabWidget* pane : panes) {
        if (!pane->currentWidget() && (pane->count() > 0)) {
            pane->makeCurrentTab(pane->count() - 1);
        }
    }
    removeSceneGraphPanelsOutsidePanes();
    noteDefaultLayout();
}

bool
Gui::isRebuildingLayout() const
{
    return _imp->rebuildingLayout;
}

namespace {

void
appendPaneStructure(const RegisteredTabs& registered,
                    const Serialization::PaneLayoutSerialization& pane,
                    std::string* out)
{
    *out += "pane " + pane.name + (pane.isAnchor ? " anchor [" : " [");
    for (const std::string& tab : pane.tabs) {
        // A viewer belongs to the project the reset closes, and the default layout puts the next
        // project's viewers in the anchor pane anyway.
        const RegisteredTabs::const_iterator found = registered.find(tab);
        if ((found == registered.end()) || dynamic_cast<ViewerTab*>(found->second.first)) {
            continue;
        }
        *out += tab + ",";
    }
    *out += "]";
}

void
appendSplitterStructure(const RegisteredTabs& registered,
                        const Serialization::SplitterSerialization& splitter,
                        std::string* out)
{
    *out += "split " + std::to_string(splitter.orientation) + " (";
    for (const Serialization::SplitterChildSerialization& child : splitter.children) {
        if (child.splitter) {
            appendSplitterStructure(registered, *child.splitter, out);
        } else if (child.pane) {
            appendPaneStructure(registered, *child.pane, out);
        }
        *out += ";";
    }
    *out += ")";
}

// The windows, splits and the tabs of each pane, without the sizes, positions and current tabs a
// user changes by looking around.
std::string
layoutStructure(const RegisteredTabs& registered,
                const Serialization::GuiLayoutSerialization& layout)
{
    std::string out;
    for (const Serialization::ApplicationWindowSerialization& window : layout.windows) {
        out += window.isMainWindow ? "main " : "floating ";
        if (window.childSplitter) {
            appendSplitterStructure(registered, *window.childSplitter, &out);
        } else if (window.childPane) {
            appendPaneStructure(registered, *window.childPane, &out);
        } else {
            out += "panel " + window.childDockablePanel;
        }
        out += "\n";
    }

    return out;
}

bool
layoutDiagnosticsWanted()
{
    return qgetenv("NATRON_MAIN_HEARTBEAT") == "1";
}

void
collectPaneTabs(const Serialization::SplitterSerialization& splitter,
                std::map<std::string, std::list<std::string>>* out)
{
    for (const Serialization::SplitterChildSerialization& child : splitter.children) {
        if (child.splitter) {
            collectPaneTabs(*child.splitter, out);
        } else if (child.pane) {
            (*out)[child.pane->name] = child.pane->tabs;
        }
    }
}

std::map<std::string, std::list<std::string>>
paneTabs(const Serialization::GuiLayoutSerialization& layout)
{
    std::map<std::string, std::list<std::string>> out;
    for (const Serialization::ApplicationWindowSerialization& window : layout.windows) {
        if (window.childSplitter) {
            collectPaneTabs(*window.childSplitter, &out);
        } else if (window.childPane) {
            out[window.childPane->name] = window.childPane->tabs;
        }
    }

    return out;
}

// Closes, in the main window, the panes the default layout has not got, each tab going back to the
// pane the default gives it. Only the split a closed pane shared is rearranged: its neighbour moves
// up into the parent split, and no other widget changes parent. Returns false, having changed
// nothing, when the layout differs in a way this cannot undo.
bool
closePanesAddedToDefault(Gui* gui,
                         const std::map<std::string, std::list<std::string>>& defaultPaneTabs)
{
    if (defaultPaneTabs.empty() || !gui->getFloatingWindows().empty()) {
        return false;
    }
    const std::list<TabWidget*> panes = gui->getPanes();
    std::map<std::string, TabWidget*> defaultPanes;
    std::vector<TabWidget*> added;
    TabWidget* anchor = 0;
    for (TabWidget* pane : panes) {
        const std::string name = pane->objectName_mt_safe().toStdString();
        if (defaultPaneTabs.count(name)) {
            defaultPanes[name] = pane;
            if (pane->isAnchor()) {
                anchor = pane;
            }
        } else if (pane->isAnchor() || !dynamic_cast<Splitter*>(pane->parentWidget())) {
            return false;
        } else {
            added.push_back(pane);
        }
    }
    if (!anchor || added.empty() || (defaultPanes.size() != defaultPaneTabs.size())) {
        return false;
    }
    std::map<std::string, TabWidget*> defaultPaneOfTab;
    for (const std::pair<const std::string, std::list<std::string>>& pane : defaultPaneTabs) {
        for (const std::string& tab : pane.second) {
            defaultPaneOfTab[tab] = defaultPanes[pane.first];
        }
    }
    for (TabWidget* pane : added) {
        while (pane->count() > 0) {
            PanelWidget* widget = 0;
            ScriptObject* object = 0;
            pane->tabAt(0, &widget, &object);
            if (!widget || !object) {
                pane->removeTab(0, false);
                continue;
            }
            TabWidget* target = 0;
            if (dynamic_cast<ViewerTab*>(widget) || dynamic_cast<Histogram*>(widget)) {
                target = anchor;
            } else {
                const std::map<std::string, TabWidget*>::const_iterator found = defaultPaneOfTab.find(object->getScriptName());
                if (found != defaultPaneOfTab.end()) {
                    target = found->second;
                }
            }
            if (target) {
                TabWidget::moveTab(widget, object, target);
            } else {
                pane->removeTab(0, false);
            }
        }
        pane->closePane();
    }

    return true;
}

} // anonymous namespace

void
Gui::noteDefaultLayout()
{
    _imp->defaultLayoutStructure.clear();
    _imp->defaultPaneTabs.clear();
    const unsigned int generation = ++_imp->layoutGeneration;
    // Queued behind the layout requests the panes just posted, so the splitters have their sizes.
    QTimer::singleShot(0, this, [this, generation]() {
        if (generation != _imp->layoutGeneration) {
            return;
        }
        try {
            Serialization::GuiLayoutSerialization layoutSerialization;
            toSerialization(&layoutSerialization);
            _imp->defaultLayoutStructure = layoutStructure(getRegisteredTabs(), layoutSerialization);
            _imp->defaultPaneTabs = paneTabs(layoutSerialization);
        } catch (const std::exception&) {
            _imp->defaultLayoutStructure.clear();
            _imp->defaultPaneTabs.clear();
        }
    });
}

void
Gui::restoreDefaultLayoutUnlessKept()
{
    // Rebuilding moves every pane's widgets to new parents, and each restyles on the move (and a GL
    // widget taken out of the window loses its context): a project reset that kept the default
    // layout's panes and tabs keeps the layout as it is, and one that only added panes closes them.
    if (_imp->defaultLayoutStructure.empty()) {
        restoreDefaultLayout();

        return;
    }
    const std::string defaultStructure = _imp->defaultLayoutStructure;
    std::string structure;
    const char* outcome = "rebuilds";
    bool rebuild = true;
    try {
        Serialization::GuiLayoutSerialization layoutSerialization;
        toSerialization(&layoutSerialization);
        structure = layoutStructure(getRegisteredTabs(), layoutSerialization);
        if (structure == defaultStructure) {
            outcome = "keeps";
            rebuild = false;
        } else if (closePanesAddedToDefault(this, _imp->defaultPaneTabs)) {
            Serialization::GuiLayoutSerialization closedSerialization;
            toSerialization(&closedSerialization);
            if (layoutStructure(getRegisteredTabs(), closedSerialization) == defaultStructure) {
                outcome = "closes the panes added to";
                rebuild = false;
            }
        }
    } catch (const std::exception&) {
        outcome = "rebuilds";
        rebuild = true;
    }
    if (layoutDiagnosticsWanted()) {
        std::fprintf(stderr, "natron layout: reset %s the default layout\nnatron layout: default structure:\n%snatron layout: structure at reset:\n%s",
                     outcome, defaultStructure.c_str(), structure.c_str());
        std::fflush(stderr);
    }
    if (!rebuild) {
        removeSceneGraphPanelsOutsidePanes();

        return;
    }
    restoreDefaultLayout();
}

void
Gui::initProjectGuiKnobs()
{
    assert(_imp->_projectGui);
    _imp->_projectGui->initializeKnobsGui();
}

QKeySequence
Gui::keySequenceForView(ViewIdx v)
{
    switch ( static_cast<int>(v) ) {
    case 0:

        return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_1);
        break;
    case 1:

        return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_2);
        break;
    case 2:

        return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_3);
        break;
    case 3:

        return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_4);
        break;
    case 4:

        return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_5);
        break;
    case 5:

        return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_6);
        break;
    case 6:

        return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_7);
        break;
    case 7:

        return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_8);
        break;
    case 8:

        return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_9);
        break;
    case 9:

        return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_0);
        break;
    default:

        return QKeySequence();
    }
}

static const char*
slotForView(int view)
{
    switch (view) {
    case 0:

        return SLOT(showView0());
        break;
    case 1:

        return SLOT(showView1());
        break;
    case 2:

        return SLOT(showView2());
        break;
    case 3:

        return SLOT(showView3());
        break;
    case 4:

        return SLOT(showView4());
        break;
    case 5:

        return SLOT(showView5());
        break;
    case 6:

        return SLOT(showView6());
        break;
    case 7:

        return SLOT(showView7());
        break;
    case 8:

        return SLOT(showView7());
        break;
    case 9:

        return SLOT(showView8());
        break;
    default:

        return NULL;
    }
}

void
Gui::updateViewsActions(int viewsCount)
{
    _imp->viewersViewMenu->clear();
    //if viewsCount == 1 we don't add a menu entry
    _imp->viewersMenu->removeAction( _imp->viewersViewMenu->menuAction() );
    if (viewsCount == 2) {
        QAction* left = new QAction(this);
        left->setCheckable(false);
        left->setShortcut( QKeySequence(Qt::CTRL | Qt::Key_1) );
        _imp->viewersViewMenu->addAction(left);
        left->setText( tr("Display Left View") );
        QObject::connect( left, SIGNAL(triggered()), this, SLOT(showView0()) );
        QAction* right = new QAction(this);
        right->setCheckable(false);
        right->setShortcut( QKeySequence(Qt::CTRL | Qt::Key_2) );
        _imp->viewersViewMenu->addAction(right);
        right->setText( tr("Display Right View") );
        QObject::connect( right, SIGNAL(triggered()), this, SLOT(showView1()) );

        _imp->viewersMenu->addAction( _imp->viewersViewMenu->menuAction() );
    } else if (viewsCount > 2) {
        for (int i = 0; i < viewsCount; ++i) {
            if (i > 9) {
                break;
            }
            QAction* viewI = new QAction(this);
            viewI->setCheckable(false);
            QKeySequence seq = keySequenceForView( ViewIdx(i) );
            if ( !seq.isEmpty() ) {
                viewI->setShortcut(seq);
            }
            _imp->viewersViewMenu->addAction(viewI);
            const char* slot = slotForView(i);
            viewI->setText( QString( tr("Display View ") ) + QString::number(i + 1) );
            if (slot) {
                QObject::connect(viewI, SIGNAL(triggered()), this, slot);
            }
        }

        _imp->viewersMenu->addAction( _imp->viewersViewMenu->menuAction() );
    }
}

void
Gui::showNodeSettingsPanel(const NodeGuiPtr& node)
{
    if (!node) {
        return;
    }
    node->ensurePanelCreated();
    const NodeGuiPtr multiInstance = node->getParentMultiInstance();
    DockablePanel* panel = multiInstance ? multiInstance->getSettingPanel() : node->getSettingPanel();
    if (!panel || !node->isVisible()) {
        return;
    }
    if (!node->isSettingsPanelVisible()) {
        node->setVisibleSettingsPanel(true);
    }
    if (!node->wasBeginEditCalled()) {
        node->beginEditKnobs();
    }
    putSettingsPanelFirst(node->getSettingPanel());
    getApp()->redrawAllViewers();
}

void
Gui::putSettingsPanelFirst(DockablePanel* panel)
{
    if (!panel) {
        return;
    }
    {
        QMutexLocker k(&_imp->openedPanelsMutex);
        std::list<DockablePanel*>::iterator it = std::find(_imp->openedPanels.begin(), _imp->openedPanels.end(), panel);
        if ( it != _imp->openedPanels.end() ) {
            if ( !(*it)->isFloating() ) {
                _imp->openedPanels.erase(it);
                _imp->openedPanels.push_front(panel);

                panel->setParent( _imp->_layoutPropertiesBin->parentWidget() );
                _imp->_layoutPropertiesBin->removeWidget(panel);
                _imp->_layoutPropertiesBin->insertWidget(0, panel);
                _imp->_propertiesScrollArea->verticalScrollBar()->setValue(0);
                if ( !panel->isVisible() ) {
                    panel->setVisible(true);
                }
                buildTabFocusOrderPropertiesBin();
            } else {
                (*it)->activateWindow();
            }
        } else {
            return;
        }
    }
}

void
Gui::addVisibleDockablePanel(DockablePanel* panel)
{
    notifyLocalPresenceChanged();
    DockablePanel* found = 0;
    int nbDockedPanels = 0;
    {
        QMutexLocker k(&_imp->openedPanelsMutex);

        for (std::list<DockablePanel*>::iterator it = _imp->openedPanels.begin(); it != _imp->openedPanels.end(); ++it) {
            if (*it == panel) {
                found = *it;
            }
            if ( !(*it)->isFloating() ) {
                ++nbDockedPanels;
            }
        }
    }

    assert(panel);
    int maxPanels = appPTR->getCurrentSettings()->getMaxPanelsOpened();
    while ( (nbDockedPanels >= maxPanels) && (maxPanels != 0) ) {
        DockablePanel* first = 0;
        {
            QMutexLocker k(&_imp->openedPanelsMutex);
            for (std::list<DockablePanel*>::reverse_iterator it = _imp->openedPanels.rbegin(); it != _imp->openedPanels.rend(); ++it) {
                if ( !(*it)->isFloating() ) {
                    if ( !first && (*it != panel) ) {
                        first = *it;
                        break;
                    }
                }
            }
        }
        if (first) {
            first->closePanel();
        } else {
            break;
        }

        nbDockedPanels = 0;

        {
            QMutexLocker k(&_imp->openedPanelsMutex);
            for (std::list<DockablePanel*>::reverse_iterator it = _imp->openedPanels.rbegin(); it != _imp->openedPanels.rend(); ++it) {
                if ( !(*it)->isFloating() ) {
                    ++nbDockedPanels;
                }
            }
        }
    }


    if (found) {
        putSettingsPanelFirst(panel);
    } else {
        if (!panel->isFloating()) {
            panel->setParent( _imp->_layoutPropertiesBin->parentWidget() );
            _imp->_layoutPropertiesBin->insertWidget(0, panel);
            _imp->_propertiesScrollArea->verticalScrollBar()->setValue(0);
        }
        if ( !panel->isVisible() ) {
            panel->setVisible(true);
        }

        QMutexLocker k(&_imp->openedPanelsMutex);
        _imp->openedPanels.push_front(panel);
    }
} // Gui::addVisibleDockablePanel

void
Gui::removeVisibleDockablePanel(DockablePanel* panel)
{
    notifyLocalPresenceChanged();
    QMutexLocker k(&_imp->openedPanelsMutex);
    std::list<DockablePanel*>::iterator it = std::find(_imp->openedPanels.begin(), _imp->openedPanels.end(), panel);

    if ( it != _imp->openedPanels.end() ) {
        _imp->openedPanels.erase(it);
    }
}

const std::list<DockablePanel*>&
Gui::getVisiblePanels() const
{
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->openedPanels;
}

std::list<DockablePanel*>
Gui::getVisiblePanels_mt_safe() const
{
    QMutexLocker k(&_imp->openedPanelsMutex);

    return _imp->openedPanels;
}

void
Gui::buildTabFocusOrderPropertiesBin()
{
    int next = 1;

    for (int i = 0; i < _imp->_layoutPropertiesBin->count(); ++i, ++next) {
        QLayoutItem* item = _imp->_layoutPropertiesBin->itemAt(i);
        QWidget* w = item->widget();
        QWidget* nextWidget = _imp->_layoutPropertiesBin->itemAt(next < _imp->_layoutPropertiesBin->count() ? next : 0)->widget();

        if (w && nextWidget) {
            setTabOrder(w, nextWidget);
        }
    }
}

void
Gui::setVisibleProjectSettingsPanel(bool minimized)
{
    if ( _imp->_projectGui->getPanel()->isClosed() ) {
        _imp->_projectGui->getPanel()->setClosed(false);
    } else {
        addVisibleDockablePanel( _imp->_projectGui->getPanel() );
    }
    if (minimized) {
        _imp->_projectGui->getPanel()->restoreMinimizedState(true);
    }
    if ( !_imp->_projectGui->isVisible() ) {
        _imp->_projectGui->setVisible(true);
    }
}

NATRON_NAMESPACE_EXIT
