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

#ifndef GUIAPPWRAPPER_H
#define GUIAPPWRAPPER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/PyAppInstance.h"
#include "Engine/PyParameter.h" // ColorTuple
#include "Engine/RectD.h"
#include "Engine/ViewIdx.h"

#include "Gui/GuiAppInstance.h"
#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER;
NATRON_PYTHON_NAMESPACE_ENTER;

class PyViewer
{
    NodeWPtr _node;
    ViewerTab* _viewer;

public:

    PyViewer(const NodePtr& node);

    ~PyViewer();


    NodePtr getInternalNode() const
    {
        return _node.lock();
    }

    void seek(int frame);

    int getCurrentFrame();

    void startForward();

    void startBackward();

    void pause();

    void redraw();

    void renderCurrentFrame(bool useCache = true);

    void setFrameRange(int firstFrame, int lastFrame);

    void getFrameRange(int* firstFrame, int* lastFrame) const;

    void setPlaybackMode(Natron::PlaybackModeEnum mode);

    Natron::PlaybackModeEnum getPlaybackMode() const;

    Natron::ViewerCompositingOperatorEnum getCompositingOperator() const;

    void setCompositingOperator(Natron::ViewerCompositingOperatorEnum op);

    int getAInput() const;

    void setAInput(int index);

    int getBInput() const;

    void setBInput(int index);

    void setChannels(Natron::DisplayChannelsEnum channels);

    Natron::DisplayChannelsEnum getChannels() const;

    void setProxyModeEnabled(bool enabled);

    bool isProxyModeEnabled() const;

    void setProxyIndex(int index);

    int getProxyIndex() const;

    /* Python API: do not use ViewIdx */
    void setCurrentView(int index);

    /* Python API: do not use ViewIdx */
    int getCurrentView() const;
};

class GuiApp
    : public App
{
    Q_DECLARE_TR_FUNCTIONS(GuiApp)

private:
    GuiAppInstanceWPtr _app;

public:

    GuiApp(const GuiAppInstancePtr& app);

    virtual ~GuiApp();

    GuiAppInstancePtr getInternalGuiApp() const
    {
        return _app.lock();
    }

    Gui* getGui() const;
    PyModalDialog* createModalDialog();
    PyTabWidget* getTabWidget(const QString& name) const;
    PyTabWidget* getActiveTabWidget() const;

    bool moveTab(const QString& scriptName, PyTabWidget* pane);

    void registerPythonPanel(PyPanel* panel, const QString& pythonFunction);
    void unregisterPythonPanel(PyPanel* panel);

    QString getFilenameDialog( const QStringList& filters, const QString& location = QString() ) const;

    QString getSequenceDialog( const QStringList& filters, const QString& location = QString() ) const;

    QString getDirectoryDialog( const QString& location = QString() ) const;

    QString saveFilenameDialog( const QStringList& filters, const QString& location = QString() ) const;

    QString saveSequenceDialog( const QStringList& filters, const QString& location = QString() ) const;

    ColorTuple getRGBColorDialog() const;

    /**
     * @brief Creates a node as the node graph's own creation does: it connects to the selected node
     * following the connection preferences, is placed below it or at the creation context's position,
     * opens its panel and is one undo step. Without a group, the node goes in the graph the creation
     * context names, else in the last selected graph. createNode() stays the raw script creation.
     **/
    Effect* createNodeInGraph(const QString& pluginID,
                              int majorVersion = -1,
                              Group* group = 0,
                              const NodeCreationPropertyMap& props = NodeCreationPropertyMap()) const;

    std::list<Effect*> getSelectedNodes(Group* group = 0) const;

    void selectNode(Effect* effect, bool clearPreviousSelection);
    void setSelection(const std::list<Effect*>& nodes);
    void selectAllNodes(Group* group = 0);
    void copySelectedNodes(Group* group = 0);
    void pasteNodes(Group* group = 0);
    void deselectNode(Effect* effect);
    void clearSelection(Group* group = 0);

    PyViewer* getViewer(const QString& scriptName) const;
    PyViewer* getActiveViewer() const;
    PyPanel* getUserPanel(const QString& scriptName) const;

    void renderBlocking(Effect* writeNode, int firstFrame, int lastFrame, int frameStep = 1);
    void renderBlocking(const std::list<Effect*>& effects, const std::list<int>& firstFrames, const std::list<int>& lastFrames, const std::list<int>& frameSteps);

    /**
     * @brief Triggers an action chosen in the tab menu. Nodes it creates with no group go into the graph
     * the menu was opened over, and the first one with no position lands at the cursor. None, sent
     * when the menu closes with no choice, ends a filtered launch without creating anything.
     **/
    void triggerTabMenuAction(QAction* action);

    /**
     * @brief Imports the template file at filePath into the graph last worked in, as one undo step,
     * the way File > Import Template does. Returns the new import's key, or an empty string when
     * nothing was imported; the reason, or the problems of an import that was only partly made, go
     * to the script editor. App.importTemplate() is the scripting variant, which returns the
     * created nodes.
     **/
    QString importTemplateAsUndoStep(const QString& filePath);

    /**
     * @brief Merges a version of the template import templateKey into the project as one undo
     * step: the file at filePath, or the one the import recorded when filePath is empty. Returns
     * the status: "updated", "upToDate", "lowMatch" when too few of the base's nodes are in the
     * file, or "templateMismatch" when the file's template ID is not the import's and
     * allowMismatch is not set; nothing is merged for the last two. Raises RuntimeError when the
     * merge failed.
     **/
    QString updateFromTemplate(const QString& templateKey, const QString& filePath = QString(), bool allowMismatch = false);

    // updateFromTemplate() reporting a failure in error, with an empty status, instead of raising it.
    QString updateFromTemplateChecked(const QString& templateKey, const QString& filePath, bool allowMismatch, QString* error);

    // Whether effect's node shows the badge of a node with unresolved merge conflicts.
    bool isMergeConflictBadgeShown(Effect* effect) const;

    /**
     * @brief Shows over the node graphs how the project differs from the project file at filePath,
     * or from the stored base of the template import templateKey. False when it could not be read,
     * with the reason in the script editor.
     **/
    bool compareWithFile(const QString& filePath);
    bool compareWithTemplateBase(const QString& templateKey);

    void clearGraphComparison();

    // "added", "changed", "moved", or an empty string when the comparison does not mark effect's node.
    QString getDiffTint(Effect* effect) const;

    // The removed nodes the comparison draws in group's node graph, the top level's by default.
    int getDiffGhostCount(Group* group = 0) const;

    // The ghost at index in group's node graph, in the coordinates of node positions, or an empty
    // rectangle past the last.
    RectD getDiffGhostRect(int index, Group* group = 0) const;

    // The rows of the Node Diff panel, headings included.
    QStringList getNodeDiffRows() const;
};

NATRON_PYTHON_NAMESPACE_EXIT;
NATRON_NAMESPACE_EXIT;

#endif // GUIAPPWRAPPER_H
