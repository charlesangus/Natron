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

#ifndef NODEVIEWERCONTEXT_H
#define NODEVIEWERCONTEXT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

CLANG_DIAG_OFF(deprecated)
#include <QObject>
#include <QString>
CLANG_DIAG_ON(deprecated)

#include "Engine/ViewIdx.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGuiContainerI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The handle a plug-in's nodes offer the 3D view's node handle tool: the button after
 * Scale in the view's left tool bar, the Node Handle Tool shortcut and the Transform entry of the
 * view's context menu. Choosing the tool sets SceneTransform::eToolNodeHandle; while it is set,
 * the view draws the handle of the node being edited (SceneViewport::getHandleNode(), the node
 * with its settings panel open whose plug-in registered a handle tool) in place of the transform
 * widget on the selection. The code that draws a node's handle checks both. Without such a node
 * the button is disabled and its tooltip says so.
 **/
struct NodeHandleTool {
    // The tool's name, as the button's tooltip and the context menu show it: "Transform Handle".
    QString label;
    // The button's icon, under the icon resources: "natron/handle-transform.svg".
    QString icon;
    // What the handle does, after the label in the tooltip.
    QString tip;
};

struct NodeViewerContextPrivate;
class NodeViewerContext
    : public QObject
    , public KnobGuiContainerI
    , public std::enable_shared_from_this<NodeViewerContext>
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

private:
    // constructors should be privatized in any class that derives from std::enable_shared_from_this<>

    NodeViewerContext(const NodeGuiPtr& node,
                      ViewerTab* viewer);

public:
    static NodeViewerContextPtr create(const NodeGuiPtr& node,
                                                       ViewerTab* viewer)
    {
        return NodeViewerContextPtr( new NodeViewerContext(node, viewer) );
    }

    void createGui();

    virtual ~NodeViewerContext();

    /**
     * @brief If this node's viewer context has a toolbar, this will return it
     **/
    QToolBar* getToolBar() const;

    /**
     * @brief The selected role ID
     **/
    const QString& getCurrentRole() const;

    /**
     * @brief The selected tool ID
     **/
    const QString& getCurrentTool() const;
    virtual Gui* getGui() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const QUndoCommand* getLastUndoCommand() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void pushUndoCommand(QUndoCommand* cmd) OVERRIDE FINAL;
    virtual KnobGuiPtr getKnobGui(const KnobIPtr& knob) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual int getItemsSpacingOnSameLine() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    void setCurrentTool(const QString& toolID, bool notifyNode);

    void notifyGuiClosing();

    virtual bool isInViewerUIKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    void onToolButtonShortcutPressed(const QString& roleID);

    /**
     * @brief Makes the nodes of pluginID offer tool to the 3D view's node handle tool, replacing
     * what they offered. Call on the main thread, before the node's panel opens to see it at once.
     * USDEditTransform offers its translate, rotate and scale handles together.
     **/
    static void registerHandleTool(const std::string& pluginID, const NodeHandleTool& tool);

    // The handle tool the nodes of pluginID offer, null for none.
    static const NodeHandleTool* findHandleTool(const std::string& pluginID);

Q_SIGNALS:

    /**
     * @brief Emitted when the selected role changes
     **/
    void roleChanged(int previousRole, int newRole);

public Q_SLOTS:

    void onNodeColorChanged(const QColor& color);

    /**
     * @brief Received when the selection rectangle has changed on the viewer.
     * @param onRelease When true, this signal is emitted on the mouse release event
     * which means this is the last selection desired by the user.
     * Receivers can either update the selection always or just on mouse release.
     **/
    void updateSelectionFromViewerSelectionRectangle(bool onRelease);

    void onViewerSelectionCleared();

    void onToolActionTriggered();

    void onToolActionTriggered(QAction* act);

    void onToolGroupValueChanged(ViewSpec view,
                                 int dimension,
                                 int reason);

    void onToolActionValueChanged(ViewSpec view,
                                  int dimension,
                                  int reason);

    void onToolActionStateChanged();

    void onNodeSettingsPanelClosed(bool closed);

    void onNodeSettingsPanelMinimized();

    void onNodeSettingsPanelMaximized();

private:

    std::unique_ptr<NodeViewerContextPrivate> _imp;
};


NATRON_NAMESPACE_EXIT

#endif // NODEVIEWERCONTEXT_H
