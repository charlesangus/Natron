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

#include "SceneToolBar.h"

#include <QAction>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QFrame>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include "Engine/AppManager.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoViewerLayer.h"
#include "Gui/ActionShortcuts.h"
#include "Gui/GuiDensity.h"
#include "Gui/NodeViewerContext.h"
#include "Gui/SceneBarCommon.h"
#include "Gui/SceneViewport.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kNodeHandleIcon = "natron/handle-transform.svg";
// The script name of the view's node handle tool action.
const char* const kNodeHandleActionName = "nodeHandle";

QString
noNodeHandleTip()
{
    return QCoreApplication::translate("SceneToolBar", "Node Handle: draws the handle of the node being edited. "
                                                       "No node with its settings panel open offers one.");
}

} // namespace

SceneToolBar::SceneToolBar(QWidget* parent)
    : QWidget(parent)
    , _pickModes(new QButtonGroup(this))
{
    setObjectName(QString::fromUtf8("sceneToolBar"));
    setFixedWidth(TO_DPIX(SceneBar::kBarSize));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(TO_DPIX(2), TO_DPIY(2), TO_DPIX(2), TO_DPIY(2));
    layout->setSpacing(TO_DPIY(NATRON_DENSITY_TOOLBAR_SPACING));

    // The names match the view's transform tool actions, whose order they keep.
    const struct
    {
        const char* name;
        const char* icon;
        const char* shortcut;
        QString tip;
    } tools[] = {
        { "sceneToolSelect", "lucide/mouse-pointer-2.svg", kShortcutIDActionViewer3DSelectTool,
          tr("Select: a click or marquee picks, with no transform handle drawn.") },
        { "sceneToolTranslate", "lucide/move.svg", kShortcutIDActionViewer3DTranslateTool,
          tr("Translate: draws the move handle on the selection.") },
        { "sceneToolRotate", "lucide/rotate-ccw.svg", kShortcutIDActionViewer3DRotateTool,
          tr("Rotate: draws the rotate handle on the selection.") },
        { "sceneToolScale", "lucide/expand.svg", kShortcutIDActionViewer3DScaleTool,
          tr("Scale: draws the scale handle on the selection.") },
    };
    for (const auto& tool : tools) {
        QToolButton* button = SceneBar::makeIconButton(this, tool.name, tool.icon, true);
        setToolTipWithShortcut(kShortcutGroupViewer3D, tool.shortcut, SceneBar::tipWithShortcut(tool.tip), button);
        const int index = int(_toolButtons.size());
        QObject::connect(button, &QToolButton::clicked, this, [this, index]() {
            if (_viewport && (index < _viewport->getTransformToolActions().size())) {
                _viewport->getTransformToolActions()[index]->trigger();
            }
            refreshTools();
        });
        _toolButtons.push_back(button);
        layout->addWidget(button, 0, Qt::AlignHCenter);
    }
    _nodeHandle = SceneBar::makeIconButton(this, "sceneToolNodeHandle", kNodeHandleIcon, true);
    setToolTipWithShortcut(kShortcutGroupViewer3D, kShortcutIDActionViewer3DNodeHandleTool, SceneBar::tipWithShortcut(noNodeHandleTip()), _nodeHandle);
    QObject::connect(_nodeHandle, &QToolButton::clicked, this, [this]() {
        if (_viewport) {
            for (QAction* action : _viewport->getTransformToolActions()) {
                if (action->objectName() == QLatin1String(kNodeHandleActionName)) {
                    action->trigger();
                }
            }
        }
        refreshTools();
    });
    layout->addWidget(_nodeHandle, 0, Qt::AlignHCenter);

    layout->addWidget(SceneBar::makeSeparator(this, true));

    const QString pickTip = tr("What a click or marquee in the 3D view selects of the geometry shown.");
    _pickModes->setExclusive(true);
    const struct
    {
        GeoComponentEnum mode;
        const char* name;
        const char* icon;
        const char* shortcut;
        QString tip;
    } picks[] = {
        { eGeoComponentObjects, "geoPickObjects", "lucide/box.svg", kShortcutIDActionViewer3DPickObjects, tr("Objects: selects whole prims.") },
        { eGeoComponentPoints, "geoPickPoints", "natron/pick-points.svg", kShortcutIDActionViewer3DPickPoints,
          tr("Points: selects points, marking every point to pick from.") },
        { eGeoComponentEdges, "geoPickEdges", "natron/pick-edges.svg", kShortcutIDActionViewer3DPickEdges,
          tr("Edges: selects the edges of polygons, as pairs of points.") },
        { eGeoComponentPrimitives, "geoPickPrimitives", "natron/pick-primitives.svg", kShortcutIDActionViewer3DPickPrimitives,
          tr("Primitives: selects primitives; a packed primitive is picked whole.") },
    };
    for (const auto& pick : picks) {
        QToolButton* button = SceneBar::makeIconButton(this, pick.name, pick.icon, true);
        button->setChecked(pick.mode == eGeoComponentObjects);
        setToolTipWithShortcut(kShortcutGroupViewer3D, pick.shortcut, SceneBar::tipWithShortcut(pick.tip + QString::fromUtf8(" ") + pickTip), button);
        _pickModes->addButton(button, int(pick.mode));
        layout->addWidget(button, 0, Qt::AlignHCenter);
    }
    QObject::connect(_pickModes, &QButtonGroup::idClicked, this, [this](int mode) {
        if (_pickModeCallback) {
            _pickModeCallback(mode);
        }
    });

    QWidget* snapSlot = new QWidget(this);
    snapSlot->setObjectName(QString::fromUtf8("snapSlot"));
    snapSlot->hide();
    layout->addWidget(snapSlot, 0, Qt::AlignHCenter);
    layout->addStretch();

    setActive(false, false);
}

void
SceneToolBar::setViewport(SceneViewport* viewport)
{
    if (_viewport == viewport) {
        return;
    }
    if (_viewport) {
        QObject::disconnect(_viewport, nullptr, this, nullptr);
    }
    _viewport = viewport;
    if (_viewport) {
        QObject::connect(_viewport, &SceneViewport::displayStateChanged, this, [this]() { refreshTools(); });
    }
    refreshTools();
}

void
SceneToolBar::setActive(bool shown,
                        bool geometry)
{
    _shown = shown;
    for (QToolButton* button : _toolButtons) {
        button->setEnabled(shown && _viewport);
    }
    for (QAbstractButton* button : _pickModes->buttons()) {
        button->setEnabled(shown && geometry && _viewport);
    }
    refreshTools();
}

void
SceneToolBar::setPickMode(int mode)
{
    const QSignalBlocker blocker(_pickModes);

    if (QAbstractButton* button = _pickModes->button(mode)) {
        button->setChecked(true);
    }
}

void
SceneToolBar::setPickModeCallback(const std::function<void(int)>& callback)
{
    _pickModeCallback = callback;
}

void
SceneToolBar::refreshTools()
{
    const QList<QAction*> actions = _viewport ? _viewport->getTransformToolActions() : QList<QAction*>();
    const QAction* nodeHandleAction = nullptr;
    for (const QAction* action : actions) {
        if (action->objectName() == QLatin1String(kNodeHandleActionName)) {
            nodeHandleAction = action;
        }
    }
    for (std::size_t i = 0; i < _toolButtons.size(); ++i) {
        SceneBar::showActionChecked(_toolButtons[i], (int(i) < actions.size()) ? actions[int(i)] : nullptr);
    }
    SceneBar::showActionChecked(_nodeHandle, nodeHandleAction);
    refreshNodeHandle();
}

void
SceneToolBar::refreshNodeHandle()
{
    const NodePtr node = _viewport ? _viewport->getHandleNode() : NodePtr();
    const NodeHandleTool* tool = node ? NodeViewerContext::findHandleTool(node->getPluginID()) : nullptr;

    _nodeHandle->setEnabled(_shown && tool);
    SceneBar::setIcon(_nodeHandle, tool ? tool->icon.toUtf8().constData() : kNodeHandleIcon);
    const QString tip = QString::fromStdString(SceneBar::tipWithShortcut(tool ? (tool->label + QString::fromUtf8(": ") + tool->tip) : noNodeHandleTip()));
    if (tip != _nodeHandleTip) {
        _nodeHandleTip = tip;
        _nodeHandle->setToolTip(tip);
    }
}

NATRON_NAMESPACE_EXIT
