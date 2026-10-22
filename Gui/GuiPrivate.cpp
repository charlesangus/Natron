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

#include "GuiPrivate.h"

#include <cassert>
#include <fstream>
#include <algorithm> // min, max
#include <stdexcept>

#include <QTextStream>
#include <QWaitCondition>
#include <QMutex>
#include <QCoreApplication>
#include <QAction>
#include <QSettings>
#include <QDebug>
#include <QThread>
#include <QCheckBox>
#include <QTimer>
#include <QTextEdit>

#include <QScreen>
#include <QUndoGroup>
GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QCloseEvent>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
#include <QApplication>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QMenu>
#include <QMenuBar>

#include <QFontMetrics>
#include <QImage>
#include <QKeySequence>
#include <QMessageBox>
#include <QProgressDialog>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolBar>
#include <QToolButton>

#include <cairo/cairo.h>

#include "Global/ProcInfo.h"
#include "Global/QtCompat.h"

#include "Engine/Image.h"
#include "Engine/KnobFile.h"
#include "Engine/Lut.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Plugin.h"
#include "Engine/ProcessHandler.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/Utils.h" // convertFromPlainText
#include "Engine/ViewerInstance.h"

#include "Gui/AboutWindow.h"
#include "Gui/ActionShortcuts.h"
#include "Gui/Button.h"
#include "Gui/CurveEditor.h"
#include "Gui/CurveWidget.h"
#include "Gui/DockablePanel.h"
#include "Gui/DopeSheetEditor.h"
#include "Gui/FloatingWidget.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDensity.h"
#include "Gui/Histogram.h"
#include "Gui/Menu.h"
#include "Gui/MessageBox.h"
#include "Gui/MultiInstancePanel.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/PreferencesPanel.h"
#include "Gui/ProgressPanel.h"
#include "Gui/ProjectGui.h"
#include "Gui/PropertiesBinWrapper.h"
#include "Gui/PythonPanels.h"
#include "Gui/QtEnumConvert.h"
#include "Gui/RightClickableWidget.h"
#include "Gui/ScriptEditor.h"
#include "Gui/SequenceFileDialog.h"
#include "Gui/SpinBox.h"
#include "Gui/Splitter.h"
#include "Gui/TabWidget.h"
#include "Gui/ThemedIcon.h"
#include "Gui/ViewerGL.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER
GuiPrivate::GuiPrivate(const GuiAppInstancePtr& app,
                       Gui* gui)
    : _gui(gui)
    , _isInDraftModeMutex()
    , _isInDraftMode(false)
    , _appInstance(app)
    , _uiUsingMainThreadCond()
    , _uiUsingMainThread(false)
    , _uiUsingMainThreadMutex()
    , _lastQuestionDialogAnswer(eStandardButtonNo)
    , _lastStopAskingAnswer(false)
    , _currentUndoAction(0)
    , _currentRedoAction(0)
    , _undoStacksGroup(0)
    , _undoStacksActions()
    , _splittersMutex()
    , _splitters()
    , _pyPanelsMutex()
    , _userPanels()
    , _isTripleSyncEnabled(false)
    , areRenderStatsEnabledMutex()
    , areRenderStatsEnabled(false)
    , actionNew_project(0)
    , actionOpen_project(0)
    , actionClose_project(0)
    , actionReload_project(0)
    , actionSave_project(0)
    , actionSaveAs_project(0)
    , actionExportAsGroup(0)
    , actionSaveAndIncrementVersion(0)
    , actionPreferences(0)
    , actionExit(0)
    , actionProject_settings(0)
    , actionShowErrorLog(0)
    , actionNewViewer(0)
    , actionFullScreen(0)
#ifdef __NATRON_WIN32__
    , actionShowWindowsConsole(0)
#endif
    , actionClearDiskCache(0)
    , actionClearPlayBackCache(0)
    , actionClearNodeCache(0)
    , actionClearPluginsLoadingCache(0)
    , actionClearAllCaches(0)
    , actionShowAboutWindow(0)
    , actionsOpenRecentFile()
    , renderAllWriters(0)
    , renderSelectedNode(0)
    , enableRenderStats(0)
    , actionConnectInput()
    , actionImportLayout(0)
    , actionExportLayout(0)
    , actionRestoreDefaultLayout(0)
    , actionNextTab(0)
    , actionPrevTab(0)
    , actionCloseTab(0)
    , actionColorTrackball(0)
    , actionHelpWebsite(0)
    , actionHelpForum(0)
    , actionHelpIssues(0)
    , actionHelpDocumentation(0)
    , _centralWidget(0)
    , _mainLayout(0)
    , _lastLoadSequenceOpenedDir()
    , _lastLoadProjectOpenedDir()
    , _lastSaveSequenceOpenedDir()
    , _lastSaveProjectOpenedDir()
    , _lastPluginDir()
    , _nextViewerTabPlace(0)
    , _leftRightSplitter(0)
    , _viewerTabsMutex()
    , _viewerTabs()
    , _masterSyncViewer(0)
    , _activeViewer(0)
    , _histogramsMutex()
    , _histograms()
    , _nextHistogramIndex(1)
    , _nodeGraphArea(0)
    , _lastFocusedGraph(0)
    , _groups()
    , _curveEditor(0)
    , _progressPanel(0)
    , _dopeSheetEditor(0)
    , _toolBox(0)
    , _propertiesBin(0)
    , _propertiesScrollArea(0)
    , _propertiesContainer(0)
    , _layoutPropertiesBin(0)
    , _clearAllPanelsButton(0)
    , _minimizeAllPanelsButtons(0)
    , _maxPanelsOpenedSpinBox(0)
    , _isGUIFrozenMutex()
    , _isGUIFrozen(false)
    , menubar(0)
    , menuFile(0)
    , menuRecentFiles(0)
    , menuEdit(0)
    , menuLayout(0)
    , menuDisplay(0)
    , menuRender(0)
    , viewersMenu(0)
    , viewerInputsMenu(0)
    , viewerInputsBMenu(0)
    , viewersViewMenu(0)
    , cacheMenu(0)
    , menuHelp(0)
    , _panesMutex()
    , _panes()
    , _floatingWindowMutex()
    , _floatingWindows()
    , _settingsGui(0)
    , _projectGui(0)
    , _errorLog(0)
    , _currentlyDraggedPanel(0)
    , _currentlyDraggedPanelInitialSize()
    , _aboutWindow(0)
    , openedPanelsMutex()
    , openedPanels()
    , _toolButtonMenuOpened(NULL)
    , aboutToCloseMutex()
    , _aboutToClose(false)
    , leftToolBarDisplayedOnHoverOnly(false)
    , _scriptEditor(0)
    , _lastEnteredTabWidget(0)
    , pythonCommands()
    , statsDialog(0)
    , currentPanelFocus(0)
    , currentPanelFocusEventRecursion(0)
    , keyPressEventHasVisitedFocusWidget(false)
    , keyUpEventHasVisitedFocusWidget(false)
    , applicationConsoleVisible(true)
{
}

void
GuiPrivate::refreshLeftToolBarVisibility(const QPoint & p)
{
    int toolbarW = _toolBox->sizeHint().width();

    if (p.x() <= toolbarW) {
        _toolBox->show();
    } else {
        _toolBox->hide();
    }
}

void
GuiPrivate::notifyGuiClosing()
{
    ///This is to workaround an issue that when destroying a widget it calls the focusOut() handler hence can
    ///cause bad pointer dereference to the Gui object since we're destroying it.
    std::list<TabWidget*> tabs;
    {
        QMutexLocker k(&_panesMutex);
        tabs = _panes;
    }

    for (std::list<TabWidget*>::iterator it = tabs.begin(); it != tabs.end(); ++it) {
        (*it)->discardGuiPointer();
        for (int i = 0; i < (*it)->count(); ++i) {
            (*it)->tabAt(i)->notifyGuiClosingPublic();
        }
    }

    const NodesGuiList allNodes = _nodeGraphArea->getAllActiveNodes();

    {
        // we do not need this list anymore, avoid using it
        QMutexLocker k(&this->openedPanelsMutex);
        this->openedPanels.clear();
    }

    for (NodesGuiList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
        DockablePanel* panel = (*it)->getSettingPanel();
        if (panel) {
            panel->onGuiClosing();
        }
    }
    _lastFocusedGraph = 0;
}

void
GuiPrivate::createPropertiesBinGui()
{
    _propertiesBin = new PropertiesBinWrapper(_gui);
    _propertiesBin->setScriptName(kPropertiesBinName);
    _propertiesBin->setLabel( tr("Properties").toStdString() );

    QVBoxLayout* mainPropertiesLayout = new QVBoxLayout(_propertiesBin);
    mainPropertiesLayout->setContentsMargins(0, 0, 0, 0);
    mainPropertiesLayout->setSpacing(0);

    _propertiesScrollArea = new QScrollArea(_propertiesBin);

    ///Remove wheel autofocus
    _propertiesScrollArea->setFocusPolicy(Qt::StrongFocus);
    QObject::connect( _propertiesScrollArea->verticalScrollBar(), SIGNAL(valueChanged(int)), _gui, SLOT(onPropertiesScrolled()) );
    _propertiesScrollArea->setObjectName( QString::fromUtf8("Properties") );
    assert(_nodeGraphArea);

    _propertiesContainer = new QWidget(_propertiesScrollArea);
    _propertiesContainer->setObjectName( QString::fromUtf8("_propertiesContainer") );
    _layoutPropertiesBin = new QVBoxLayout(_propertiesContainer);
    _layoutPropertiesBin->setSpacing(0);
    _layoutPropertiesBin->setContentsMargins(0, 0, 0, 0);
    _propertiesContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    _propertiesScrollArea->setWidget(_propertiesContainer);
    _propertiesScrollArea->setWidgetResizable(true);

    QWidget* propertiesAreaButtonsContainer = new QWidget(_propertiesBin);
    QHBoxLayout* propertiesAreaButtonsLayout = new QHBoxLayout(propertiesAreaButtonsContainer);
    propertiesAreaButtonsLayout->setContentsMargins(0, 0, 0, 0);
    propertiesAreaButtonsLayout->setSpacing(TO_DPIX(NATRON_DENSITY_TOOLBAR_SPACING));
    const int mediumSizeIcon = TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE);
    QIcon closePanelIcon;
    appPTR->getIcon(NATRON_PIXMAP_CLOSE_PANEL, mediumSizeIcon, &closePanelIcon);
    _clearAllPanelsButton = new Button(closePanelIcon, QString(), propertiesAreaButtonsContainer);

    const QSize mediumButtonSize(TO_DPIX(NATRON_MEDIUM_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE));
    const QSize mediumButtonIconSize(mediumSizeIcon, TO_DPIY(NATRON_MEDIUM_BUTTON_ICON_SIZE));

    _clearAllPanelsButton->setObjectName(QString::fromUtf8("PropertiesBinClearAllButton"));
    _clearAllPanelsButton->setFixedSize(mediumButtonSize);
    _clearAllPanelsButton->setIconSize(mediumButtonIconSize);
    _clearAllPanelsButton->setToolTip( NATRON_NAMESPACE::convertFromPlainText(tr("Clears all the panels in the properties bin pane."),
                                                                      NATRON_NAMESPACE::WhiteSpaceNormal) );
    _clearAllPanelsButton->setFocusPolicy(Qt::NoFocus);
    QObject::connect( _clearAllPanelsButton, SIGNAL(clicked(bool)), _gui, SLOT(clearAllVisiblePanels()) );
    const QIcon mIc = ThemedIcon::icon(QString::fromUtf8(NATRON_ICONS_PATH "lucide/chevrons-down-up.svg"), mediumSizeIcon);
    _minimizeAllPanelsButtons = new Button(mIc, QString(), propertiesAreaButtonsContainer);
    _minimizeAllPanelsButtons->setCheckable(true);
    _minimizeAllPanelsButtons->setChecked(false);
    _minimizeAllPanelsButtons->setObjectName(QString::fromUtf8("PropertiesBinMinimizeAllButton"));
    _minimizeAllPanelsButtons->setFixedSize(mediumButtonSize);
    _minimizeAllPanelsButtons->setIconSize(mediumButtonIconSize);
    _minimizeAllPanelsButtons->setToolTip( NATRON_NAMESPACE::convertFromPlainText(tr("Minimize / Maximize all panels."), NATRON_NAMESPACE::WhiteSpaceNormal) );
    _minimizeAllPanelsButtons->setFocusPolicy(Qt::NoFocus);
    QObject::connect( _minimizeAllPanelsButtons, SIGNAL(clicked(bool)), _gui, SLOT(minimizeMaximizeAllPanels(bool)) );

    _maxPanelsOpenedSpinBox = new SpinBox(propertiesAreaButtonsContainer);
    // Sized from the font rather than the 15 px button height, which is shorter than one
    // line of text plus the line edit's frame and padding.
    {
        const QFontMetrics fm(_maxPanelsOpenedSpinBox->font());
        const int frame = TO_DPIX(4);
        _maxPanelsOpenedSpinBox->setFixedSize(fm.horizontalAdvance(QString::fromUtf8("99")) + 2 * frame,
                                              std::max<int>(TO_DPIY(NATRON_SMALL_BUTTON_SIZE), fm.height() + frame));
    }
    _maxPanelsOpenedSpinBox->setAlignment(Qt::AlignCenter);
    _maxPanelsOpenedSpinBox->setMinimum(0);
    _maxPanelsOpenedSpinBox->setMaximum(99);
    _maxPanelsOpenedSpinBox->setToolTip( NATRON_NAMESPACE::convertFromPlainText(tr("Set the maximum of panels that can be opened at the same time "
                                                                           "in the properties bin pane. The special value of 0 indicates "
                                                                           "that an unlimited number of panels can be opened."),
                                                                        NATRON_NAMESPACE::WhiteSpaceNormal) );
    _maxPanelsOpenedSpinBox->setValue( appPTR->getCurrentSettings()->getMaxPanelsOpened() );
    QObject::connect( _maxPanelsOpenedSpinBox, SIGNAL(valueChanged(double)), _gui, SLOT(onMaxPanelsSpinBoxValueChanged(double)) );

    propertiesAreaButtonsLayout->addWidget(_maxPanelsOpenedSpinBox, 0, Qt::AlignVCenter);
    propertiesAreaButtonsLayout->addWidget(_clearAllPanelsButton, 0, Qt::AlignVCenter);
    propertiesAreaButtonsLayout->addWidget(_minimizeAllPanelsButtons, 0, Qt::AlignVCenter);
    propertiesAreaButtonsLayout->addStretch();

    mainPropertiesLayout->addWidget(propertiesAreaButtonsContainer);
    mainPropertiesLayout->addWidget(_propertiesScrollArea);

    _propertiesBin->setVisible(false);
    _gui->registerTab(_propertiesBin, _propertiesBin);
} // createPropertiesBinGui

void
GuiPrivate::createNodeGraphGui()
{
    QGraphicsScene* scene = new QGraphicsScene(_gui);

    scene->setItemIndexMethod(QGraphicsScene::NoIndex);
    _nodeGraphArea = new NodeGraph(_gui, _appInstance.lock()->getProject(), scene, _gui);
    _nodeGraphArea->setScriptName(kNodeGraphObjectName);
    _nodeGraphArea->setLabel( tr("Node Graph").toStdString() );
    _nodeGraphArea->setVisible(false);
    _gui->registerTab(_nodeGraphArea, _nodeGraphArea);
}

void
GuiPrivate::createCurveEditorGui()
{
    _curveEditor = new CurveEditor(_gui, _appInstance.lock()->getTimeLine(), _gui);
    _curveEditor->setScriptName(kCurveEditorObjectName);
    _curveEditor->setLabel( tr("Curve Editor").toStdString() );
    _curveEditor->setVisible(false);
    _gui->registerTab(_curveEditor, _curveEditor);
}

void
GuiPrivate::createDopeSheetGui()
{
    _dopeSheetEditor = new DopeSheetEditor(_gui, _appInstance.lock()->getTimeLine(), _gui);
    _dopeSheetEditor->setScriptName(kDopeSheetEditorObjectName);
    _dopeSheetEditor->setLabel( tr("Dope Sheet").toStdString() );
    _dopeSheetEditor->setVisible(false);
    _gui->registerTab(_dopeSheetEditor, _dopeSheetEditor);
}

void
GuiPrivate::createScriptEditorGui()
{
    _scriptEditor = new ScriptEditor(_gui);
    _scriptEditor->setScriptName("scriptEditor");
    _scriptEditor->setLabel( tr("Script Editor").toStdString() );
    _scriptEditor->setVisible(false);
    _gui->registerTab(_scriptEditor, _scriptEditor);
}

void
GuiPrivate::createProgressPanelGui()
{
    _progressPanel = new ProgressPanel(_gui);
    _progressPanel->setScriptName("progress");
    _progressPanel->setLabel( tr("Progress").toStdString() );
    _progressPanel->setVisible(false);
    _gui->registerTab(_progressPanel, _progressPanel);
}

TabWidget*
GuiPrivate::getOnly1NonFloatingPane(int & count) const
{
    assert( !_panesMutex.tryLock() );
    count = 0;
    if ( _panes.empty() ) {
        return NULL;
    }
    TabWidget* firstNonFloating = 0;
    for (std::list<TabWidget*>::const_iterator it = _panes.begin(); it != _panes.end(); ++it) {
        if ( !(*it)->isFloatingWindowChild() ) {
            if (!firstNonFloating) {
                firstNonFloating = *it;
            }
            ++count;
        }
    }
    ///there should always be at least 1 non floating window
    assert(firstNonFloating);

    return firstNonFloating;
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

class AutoRaiseToolButton
    : public QToolButton
{
    Gui* _gui;
    bool _menuOpened;

public:

    AutoRaiseToolButton(Gui* gui,
                        QWidget* parent)
        : QToolButton(parent)
        , _gui(gui)
        , _menuOpened(false)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }

private:

    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL
    {
        appPTR->setNodesMenuTarget(_gui, menu());
        _menuOpened = !_menuOpened;
        if (_menuOpened) {
            setFocus();
            _gui->setToolButtonMenuOpened(this);
        } else {
            _gui->setToolButtonMenuOpened(NULL);
        }
        QToolButton::mousePressEvent(e);
        appPTR->releaseNodesMenuTargetIfNotShown();
    }

    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL
    {
        _gui->setToolButtonMenuOpened(NULL);
        QToolButton::mouseReleaseEvent(e);
    }

    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL
    {
        // Space and Select press the button, which pops an InstantPopup button's menu up.
        const bool mayOpenMenu = (e->key() == Qt::Key_Right) || (e->key() == Qt::Key_Space) || (e->key() == Qt::Key_Select);
        if (mayOpenMenu) {
            appPTR->setNodesMenuTarget(_gui, menu());
        }
        if (e->key() == Qt::Key_Right) {
            QMenu* m = menu();
            if (m) {
                QList<QAction*> actions = m->actions();
                if ( !actions.isEmpty() ) {
                    m->setActiveAction(actions[0]);
                }
            }
            showMenu();
        } else if (e->key() == Qt::Key_Left) {
            //This code won't work because the menu is active and modal
            //But at least it deactivate the focus tabbing when pressing the left key
            QMenu* m = menu();
            if ( m && m->isVisible() ) {
                m->hide();
            }
        } else {
            QToolButton::keyPressEvent(e);
        }
        if (mayOpenMenu) {
            appPTR->releaseNodesMenuTargetIfNotShown();
        }
    }

    virtual void enterEvent(QEnterEvent* e) OVERRIDE FINAL
    {
        AutoRaiseToolButton* btn = dynamic_cast<AutoRaiseToolButton*>( _gui->getToolButtonMenuOpened() );

        if ( btn && (btn != this) && btn->menu()->isActiveWindow() ) {
            btn->menu()->close();
            btn->_menuOpened = false;
            setFocus();
            _gui->setToolButtonMenuOpened(this);
            _menuOpened = true;
            appPTR->setNodesMenuTarget(_gui, menu());
            showMenu();
            appPTR->releaseNodesMenuTargetIfNotShown();
        }
        QToolButton::enterEvent(e);
    }
};

NATRON_NAMESPACE_ANONYMOUS_EXIT

void
GuiPrivate::addToolButton(QAction* groupingMenuAction)
{
    QToolButton* button = new AutoRaiseToolButton(_gui, _toolBox);

    //button->setArrowType(Qt::NoArrow); // has no effect (arrow is still displayed)
    //button->setToolButtonStyle(Qt::ToolButtonIconOnly); // has no effect (arrow is still displayed)
    button->setIcon(groupingMenuAction->property(kNodesMenuPropToolButtonIcon).value<QIcon>());
    button->setMenu(groupingMenuAction->menu());

    const QSize toolButtonSize( TO_DPIX(NATRON_TOOL_BUTTON_SIZE), TO_DPIY(NATRON_TOOL_BUTTON_SIZE) );
    button->setFixedSize(toolButtonSize);
    button->setPopupMode(QToolButton::InstantPopup);
    const QString label = groupingMenuAction->property(kNodesMenuPropGrouping).toString();
    button->setToolTip(NATRON_NAMESPACE::convertFromPlainText(label.trimmed(), NATRON_NAMESPACE::WhiteSpaceNormal));
    _nodesToolBoxActions.push_back(_toolBox->addWidget(button));
}

void
GuiPrivate::rebuildNodesToolBox()
{
    if (!_toolBox) {
        return;
    }
    _toolButtonMenuOpened = 0;
    // The toolbar releases a removed action's widget, and the action it made for that widget
    // deletes the widget with it. A rebuild can be requested from inside a button's own popup,
    // hence the deferred deletion.
    for (std::size_t i = 0; i < _nodesToolBoxActions.size(); ++i) {
        _toolBox->removeAction(_nodesToolBoxActions[i]);
        _nodesToolBoxActions[i]->deleteLater();
    }
    _nodesToolBoxActions.clear();

    QMenu* nodesMenu = appPTR->getNodesMenu();
    if (!nodesMenu) {
        return;
    }
    QList<QAction*> actions = nodesMenu->actions();
    for (int i = 0; i < actions.size(); ++i) {
        if (actions[i]->menu() && actions[i]->property(kNodesMenuPropPluginOwned).toBool()) {
            addToolButton(actions[i]);
        }
    }
}

void
GuiPrivate::setUndoRedoActions(QAction* undoAction,
                               QAction* redoAction)
{
    if (_currentUndoAction) {
        menuEdit->removeAction(_currentUndoAction);
    }
    if (_currentRedoAction) {
        menuEdit->removeAction(_currentRedoAction);
    }
    _currentUndoAction = undoAction;
    _currentRedoAction = redoAction;
    if (undoAction) {
        menuEdit->addAction(undoAction);
    }
    if (redoAction) {
        menuEdit->addAction(redoAction);
    }
}

bool
GuiPrivate::checkProjectLockAndWarn(const QString& projectPath,
                                    const QString& projectName)
{
    ProjectPtr project = _appInstance.lock()->getProject();
    QString author;
    QString lockCreationDate;
    QString lockHost;
    qint64 lockPID;

    if ( project->getLockFileInfos(projectPath, projectName, &author, &lockCreationDate, &lockHost, &lockPID) ) {
        qint64 curPid = (qint64)QCoreApplication::applicationPid();
        if (lockPID != curPid) {
            QString appFilePath = QCoreApplication::applicationFilePath();
            if ( ProcInfo::checkIfProcessIsRunning(appFilePath.toStdString().c_str(), lockPID) ) {
                StandardButtonEnum rep = Dialogs::questionDialog( tr("Project").toStdString(),
                                                                  tr("This project may be open in another instance of Natron "
                                                                     "running on %1 as process ID %2, "
                                                                     "and was opened by %3 on %4.\nContinue anyway?").arg(lockHost,
                                                                                                                          QString::number(lockPID),
                                                                                                                          author,
                                                                                                                          lockCreationDate).toStdString(),
                                                                  false,
                                                                  StandardButtons(eStandardButtonYes | eStandardButtonNo) );
                if (rep == eStandardButtonYes) {
                    return true;
                } else {
                    return false;
                }
            }
        }
    }

    return true;
}

void
GuiPrivate::restoreGuiGeometry()
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );

    settings.beginGroup( QString::fromUtf8("MainWindow") );

    if ( settings.contains( QString::fromUtf8("pos") ) ) {
        QPoint pos = settings.value( QString::fromUtf8("pos") ).toPoint();
        _gui->move(pos);
    }
    if ( settings.contains( QString::fromUtf8("size") ) ) {
        QSize size = settings.value( QString::fromUtf8("size") ).toSize();
        _gui->resize(size);
    } else {
        ///No window size serialized, give some appropriate default value according to the screen size
        QScreen* desktop = QGuiApplication::primaryScreen();
        QRect screen = desktop->availableGeometry();
        _gui->resize( (int)( 0.93 * screen.width() ), (int)( 0.93 * screen.height() ) ); // leave some space
    }
    if ( settings.contains( QString::fromUtf8("maximized")) ) {
        bool maximized = settings.value( QString::fromUtf8("maximized") ).toBool();
        if (maximized) {
            _gui->showMaximized();
        }
    }
    if ( settings.contains( QString::fromUtf8("fullScreen") ) ) {
        bool fs = settings.value( QString::fromUtf8("fullScreen") ).toBool();
        if (fs) {
            _gui->toggleFullScreen();
        }
    }

    if ( settings.contains( QString::fromUtf8("ToolbarHidden") ) ) {
        leftToolBarDisplayedOnHoverOnly = settings.value( QString::fromUtf8("ToolbarHidden") ).toBool();
    }

    settings.endGroup();

    if ( settings.contains( QString::fromUtf8("LastOpenProjectDialogPath") ) ) {
        _lastLoadProjectOpenedDir = settings.value( QString::fromUtf8("LastOpenProjectDialogPath") ).toString();
        QDir d(_lastLoadProjectOpenedDir);
        if ( !d.exists() ) {
            _lastLoadProjectOpenedDir.clear();
        }
    }
    if ( settings.contains( QString::fromUtf8("LastSaveProjectDialogPath") ) ) {
        _lastSaveProjectOpenedDir = settings.value( QString::fromUtf8("LastSaveProjectDialogPath") ).toString();
        QDir d(_lastSaveProjectOpenedDir);
        if ( !d.exists() ) {
            _lastSaveProjectOpenedDir.clear();
        }
    }
    if ( settings.contains( QString::fromUtf8("LastLoadSequenceDialogPath") ) ) {
        _lastLoadSequenceOpenedDir = settings.value( QString::fromUtf8("LastLoadSequenceDialogPath") ).toString();
        QDir d(_lastLoadSequenceOpenedDir);
        if ( !d.exists() ) {
            _lastLoadSequenceOpenedDir.clear();
        }
    }
    if ( settings.contains( QString::fromUtf8("LastSaveSequenceDialogPath") ) ) {
        _lastSaveSequenceOpenedDir = settings.value( QString::fromUtf8("LastSaveSequenceDialogPath") ).toString();
        QDir d(_lastSaveSequenceOpenedDir);
        if ( !d.exists() ) {
            _lastSaveSequenceOpenedDir.clear();
        }
    }
    if ( settings.contains( QString::fromUtf8("LastPluginDir") ) ) {
        _lastPluginDir = settings.value( QString::fromUtf8("LastPluginDir") ).toString();
    }

#ifdef __NATRON_WIN32__
    if ( settings.contains( QString::fromUtf8("ApplicationConsoleVisible") ) ) {
        bool visible = settings.value( QString::fromUtf8("ApplicationConsoleVisible") ).toBool();
        _gui->setApplicationConsoleActionVisible(visible);
    } else {
        _gui->setApplicationConsoleActionVisible(false);
    }
#endif
} // GuiPrivate::restoreGuiGeometry

void
GuiPrivate::saveGuiGeometry()
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );

    settings.beginGroup( QString::fromUtf8("MainWindow") );
    settings.setValue( QString::fromUtf8("pos"), _gui->pos() );
    settings.setValue( QString::fromUtf8("size"), _gui->size() );
    settings.setValue( QString::fromUtf8("fullScreen"), _gui->isFullScreen() );
    settings.setValue( QString::fromUtf8("maximized"), _gui->isMaximized() );
    settings.setValue( QString::fromUtf8("ToolbarHidden"), leftToolBarDisplayedOnHoverOnly);
    settings.endGroup();

    settings.setValue(QString::fromUtf8("LastOpenProjectDialogPath"), _lastLoadProjectOpenedDir);
    settings.setValue(QString::fromUtf8("LastSaveProjectDialogPath"), _lastSaveProjectOpenedDir);
    settings.setValue(QString::fromUtf8("LastLoadSequenceDialogPath"), _lastLoadSequenceOpenedDir);
    settings.setValue(QString::fromUtf8("LastSaveSequenceDialogPath"), _lastSaveSequenceOpenedDir);
    settings.setValue(QString::fromUtf8("LastPluginDir"), _lastPluginDir);
#ifdef __NATRON_WIN32__
    settings.setValue(QString::fromUtf8("ApplicationConsoleVisible"), applicationConsoleVisible);
#endif
}

QAction*
GuiPrivate::findActionRecursive(int i,
                                QWidget* widget,
                                const QStringList & grouping)
{
    assert( i < grouping.size() );
    QList<QAction*> actions = widget->actions();
    for (QList<QAction*>::iterator it = actions.begin(); it != actions.end(); ++it) {
        if ( (*it)->text() == grouping[i] ) {
            if (i == grouping.size() - 1) {
                return *it;
            } else {
                QMenu* menu = (*it)->menu();
                if (menu) {
                    return findActionRecursive(i + 1, menu, grouping);
                } else {
                    ///Error: specified the name of an already existing action
                    return 0;
                }
            }
        }
    }
    ///Create the entry
    if (i < grouping.size() - 1) {
        Menu* menu = new Menu(widget);
        menu->setTitle(grouping[i]);
        QMenu* isMenu = dynamic_cast<QMenu*>(widget);
        QMenuBar* isMenuBar = dynamic_cast<QMenuBar*>(widget);
        if (isMenu) {
            isMenu->addAction( menu->menuAction() );
        } else if (isMenuBar) {
            isMenuBar->addAction( menu->menuAction() );
        }

        return findActionRecursive(i + 1, menu, grouping);
    } else {
        ActionWithShortcut* action = new ActionWithShortcut(kShortcutGroupGlobal, grouping[i].toStdString(), grouping[i].toStdString(), widget);
        QObject::connect( action, SIGNAL(triggered()), _gui, SLOT(onUserCommandTriggered()) );
        QMenu* isMenu = dynamic_cast<QMenu*>(widget);
        if (isMenu) {
            isMenu->addAction(action);
        }

        return action;
    }

    return 0;
}

NATRON_NAMESPACE_EXIT
