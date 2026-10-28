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

#ifndef Gui_GuiApplicationManager_h
#define Gui_GuiApplicationManager_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"
#include "Global/Enums.h"

#include <list>

#include <QtGlobal> // for Q_OS_*

#include "Engine/AppManager.h"
#include "Engine/Variant.h"
#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"

#if defined(appPTR)
#undef appPTR
#endif
#define appPTR ( static_cast<GuiApplicationManager*>( AppManager::instance() ) )

#define appFont ( appPTR->getAppFont() )
#define appFontSize ( appPTR->getAppFontSize() )

/**
 * @brief Returns true if the given modifiers and symbol should trigger the given action of the given group.
 **/
#define isKeybind(group, action, modifiers, symbol) ( appPTR->matchesKeybind(group, action, modifiers, symbol) )

/**
 * @brief Returns true if the given modifiers and button should trigger the given action of the given group.
 **/
#define isMouseShortcut(group, action, modifiers, button) ( appPTR->matchesMouseShortcut(group, action, modifiers, button) )

/**
 * @brief Returns the QKeySequence object for the given action of the given group.
 **/
#define getKeybind(group, action) ( appPTR->getKeySequenceForAction(group, action) )

#define kNodesMenuObjectName "NodesMenu"

// Set on every action the plug-in tree puts in the nodes menu, plug-in leaves and grouping
// submenus' menu actions alike. Rebuilding the tree only ever removes actions carrying it.
#define kNodesMenuPropPluginOwned "natronPluginOwned"

// On plug-in actions.
#define kNodesMenuPropPluginID "natronPluginID"
#define kNodesMenuPropPluginMajor "natronPluginMajor"
#define kNodesMenuPropCategoryColor "natronCategoryColor"
#define kNodesMenuPropShortcut "natronShortcut"

// On grouping submenus' menu actions: the grouping's name, unescaped, and the icon the
// toolbar draws for it.
#define kNodesMenuPropGrouping "natronGrouping"
#define kNodesMenuPropToolButtonIcon "natronToolButtonIcon"

NATRON_NAMESPACE_ENTER

struct PythonUserCommand
{
    QString grouping;
    Qt::Key key;
    Qt::KeyboardModifiers modifiers;
    std::string pythonFunction;
};

struct GuiApplicationManagerPrivate;
class GuiApplicationManager
    : public AppManager
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    GuiApplicationManager();

    virtual ~GuiApplicationManager();

    const std::list<PluginGroupNodePtr> & getTopLevelPluginsToolButtons() const;
    PluginGroupNodePtr  findPluginToolButtonOrCreate(const QStringList & grouping,
                                                                     const QString & name,
                                                                     const QStringList& groupIconPath,
                                                                     const QString & iconPath,
                                                                     int major,
                                                                     int minor,
                                                                     bool isUserCreatable);

    /**
     * @brief The process-wide menu of every user-creatable plug-in, one submenu per grouping.
     * Null until the plug-ins have loaded. Every window's toolbar and node graph show it.
     **/
    QMenu* getNodesMenu() const;

    /**
     * @brief Replaces the plug-in-owned actions of the nodes menu with the current plug-in tree,
     * keeping any other action, and emits nodesMenuRebuilt().
     **/
    void rebuildNodesMenu();

    /**
     * @brief Adds tree entries for plug-ins that became user-creatable since they were loaded,
     * then rebuilds the nodes menu.
     **/
    void onPluginsActivationChanged();

    /**
     * @brief The window whose toolbar or graph is about to show the nodes menu in popup: until
     * popup hides, nodes created from the menu go into that window's last selected node
     * collection. With no popup open, they go into the active window's.
     **/
    void setNodesMenuTarget(Gui* gui, QMenu* popup);

    /**
     * @brief Drops the target set by setNodesMenuTarget() if its popup has not been shown since,
     * for callers that cannot tell beforehand whether an event opens the popup.
     **/
    void releaseNodesMenuTargetIfNotShown();

    virtual bool isBackground() const OVERRIDE FINAL
    {
        return false;
    }

    void getIcon(Natron::PixmapEnum e, QPixmap* pix) const;
    void getIcon(Natron::PixmapEnum e, int size, QPixmap* pix) const;

    // For an SVG-backed id, builds the full themed Normal/On/Disabled/Active QIcon
    // via ThemedIcon::icon() instead of a single flat pixmap; a PNG id falls back
    // to the plain pixmap above, wrapped in a QIcon with only its Normal/Off state.
    void getIcon(Natron::PixmapEnum e, int size, QIcon* icon) const;

    // A toggle whose On state is a different glyph (play/stop, lock/unlock). Ids that
    // map to the same SVG give the single-glyph icon, with On in the selection colour.
    void getIcon(Natron::PixmapEnum off, Natron::PixmapEnum on, int size, QIcon* icon) const;

    void setKnobClipBoard(Natron::KnobClipBoardType type,
                          const KnobIPtr& serialization,
                          int dimension);


    void getKnobClipBoard(Natron::KnobClipBoardType *type,
                          KnobIPtr* serialization,
                          int *dimension) const;


    void updateAllRecentFileMenus();

    bool isSplashcreenVisible() const;

    /**
     * @brief True when the GUI refused to start because the OpenGL context was not good enough.
     **/
    bool hasRefusedGLContext() const;

    virtual void hideSplashScreen() OVERRIDE FINAL;
    const QCursor & getColorPickerCursor() const;
    const QCursor & getLinkToCursor() const;
    const QCursor & getLinkToMultCursor() const;
    virtual void setLoadingStatus(const QString & str) OVERRIDE FINAL;
    KnobGui* createGuiForKnob(KnobIPtr knob, KnobGuiContainerI *container) const;
    virtual void setUndoRedoStackLimit(int limit) OVERRIDE FINAL;
    virtual void debugImage( const Image* image, const RectI& roi, const QString & filename = QString() ) const OVERRIDE FINAL;

    void setFileToOpen(const QString & str);

    /**
     * @brief Returns true if the given keyboard symbol and modifiers match the given action.
     * The symbol parameter is to be casted to the Qt::Key enum
     **/
    bool matchesKeybind(const std::string & group,
                        const std::string & actionID,
                        const Qt::KeyboardModifiers & modifiers,
                        int symbol) const;

    /**
     * @brief Returns true if the given keyboard modifiers and the given mouse button match the given action.
     * The button parameter is to be casted to the Qt::MouseButton enum
     **/
    bool matchesMouseShortcut(const std::string & group, const std::string & actionID, const Qt::KeyboardModifiers & modifiers, int button) const;

    std::list<QKeySequence> getKeySequenceForAction(const QString & group, const QString & actionID) const;

    /**
     * @brief Save shortcuts to QSettings
     **/
    void saveShortcuts() const;

    void restoreDefaultShortcuts();

    const std::map<QString, std::map<QString, BoundAction*> > & getAllShortcuts() const;

    /**
     * @brief Register an action to the shortcut manager indicating it is using a shortcut.
     * This is used to update the action's shortcut when it gets modified by the user.
     **/
    void addShortcutAction(const QString & group, const QString & actionID, ActionWithShortcut* action);
    void removeShortcutAction(const QString & group, const QString & actionID, QAction* action);

    void notifyShortcutChanged(KeyBoundAction* action);

    bool isShorcutVersionUpToDate() const;

    virtual void showErrorLog() OVERRIDE FINAL;
    virtual QString getAppFont() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getAppFontSize() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    ///Closes the application, asking the user to save each opened project that has unsaved changes
    virtual void exitApp(bool warnUserForSave) OVERRIDE FINAL;

    bool isNodeClipBoardEmpty() const;

    NodeClipBoard& getNodeClipBoard();
    virtual void reloadStylesheets() OVERRIDE FINAL;
    virtual void reloadScriptEditorFonts() OVERRIDE FINAL;

    void clearNodeClipBoard();

    virtual void addCommand(const QString& grouping, const std::string& pythonFunction, Qt::Key key, const Qt::KeyboardModifiers& modifiers) OVERRIDE;
    const std::list<PythonUserCommand>& getUserPythonCommands() const;

    bool handleImageFileOpenRequest(const std::string& imageFile);

    void appendTaskToPreviewThread(const NodeGuiPtr& node, double time);

    int getDocumentationServerPort();

#ifdef Q_OS_DARWIN
    void onClickOnDock();
#endif

    virtual void setCurrentLogicalDPI(double dpiX, double dpiY) OVERRIDE FINAL;
    virtual double getLogicalDPIXRATIO() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual double getLogicalDPIYRATIO() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual double getScreenDevicePixelRatio() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void updateAboutWindowLibrariesVersion() OVERRIDE FINAL;

public Q_SLOTS:

    void onFontconfigCacheUpdateFinished();
    void onFontconfigTimerTriggered();
    void onThemeReloadTimerTriggered();

Q_SIGNALS:

    // Emitted once the stylesheet has been reapplied after a theme colour change. Widgets
    // that hold a rendered QPixmap (labels, cursors, GL textures) re-render on it; QIcons
    // built by ThemedIcon recolour on their own when repainted.
    void themeChanged();

    void nodesMenuRebuilt();

public:
#ifdef Q_OS_DARWIN
Q_SIGNALS:
    void dockClicked();
#endif
    
private:

    virtual void initBuiltinPythonModules() OVERRIDE FINAL;

    void onPluginLoaded(Plugin* plugin) OVERRIDE;
    virtual void ignorePlugin(Plugin* plugin) OVERRIDE FINAL;
    virtual void onAllPluginsLoaded() OVERRIDE FINAL;
    virtual void loadBuiltinNodePlugins() OVERRIDE;
    virtual bool initGui(const CLArgs& args) OVERRIDE FINAL;
    virtual AppInstancePtr makeNewInstance(int appID) const OVERRIDE FINAL;
    virtual void registerGuiMetaTypes() const OVERRIDE FINAL;
    virtual void initializeQApp(int &argc, char **argv)  OVERRIDE FINAL;
    virtual bool checkGLContextRequirements() OVERRIDE FINAL;

    void handleOpenFileRequest();

    virtual void onLoadCompleted() OVERRIDE FINAL;
    virtual void clearLastRenderedTextures() OVERRIDE FINAL;
    virtual void releaseSceneViewEngines() OVERRIDE FINAL;
    /**
     * @brief Load shortcuts from QSettings
     **/
    void loadShortcuts();

    void populateShortcuts();

    /**
     * @brief Re-reads the category colour and shortcut properties of the nodes menu's actions for
     * pluginID, or of every plug-in's actions when pluginID is empty.
     **/
    void refreshNodesMenuPluginProperties(const QString& pluginID);

    std::unique_ptr<GuiApplicationManagerPrivate> _imp;
};

NATRON_NAMESPACE_EXIT


#endif // Gui_GuiApplicationManager_h
