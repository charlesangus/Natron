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

#include "SceneDisplayBar.h"

#include <QAction>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include "Engine/AppManager.h"
#include "Engine/ViewerInstance.h"
#include "Gui/ActionShortcuts.h"
#include "Gui/GeoDisplay.h"
#include "Gui/GuiDensity.h"
#include "Gui/SceneBarCommon.h"
#include "Gui/SceneViewport.h"

NATRON_NAMESPACE_ENTER

namespace {

// A button that opens menu, which the view's shared actions fill each time it opens.
QToolButton*
makeMenuButton(QWidget* parent,
               const char* name,
               const char* icon,
               const QString& tip,
               QMenu** menu)
{
    QToolButton* button = SceneBar::makeIconButton(parent, name, icon, false);
    button->setToolTip(tip);
    *menu = new QMenu(button);
    button->setMenu(*menu);
    button->setPopupMode(QToolButton::InstantPopup);

    return button;
}

// The purposes shown, as the one tile of the only purpose drawn besides the default prims, the
// default tile when none is, or the stacked tile when several are.
const char*
purposesIcon(const QList<QAction*>& actions)
{
    const char* const icons[][2] = {
        { "proxy", "natron/purpose-proxy.svg" },
        { "render", "natron/purpose-render.svg" },
        { "guide", "natron/purpose-guide.svg" },
    };
    const char* icon = "natron/purpose-default.svg";
    int shown = 0;

    for (const QAction* action : actions) {
        if (!action->isChecked()) {
            continue;
        }
        ++shown;
        for (const auto& entry : icons) {
            if (action->objectName() == QLatin1String(entry[0])) {
                icon = entry[1];
            }
        }
    }

    return (shown > 1) ? "natron/purpose-several.svg" : icon;
}

/**
 * @brief Shows on button the icon of the checked one of actions, looked up by action name in
 * icons (pairs of action name and icon path, ending with a null name); fallback when none matches.
 **/
void
showCheckedIcon(QToolButton* button,
                const QList<QAction*>& actions,
                const char* const icons[][2],
                const char* fallback)
{
    const char* icon = fallback;

    for (const QAction* action : actions) {
        if (!action->isChecked()) {
            continue;
        }
        for (int i = 0; icons[i][0]; ++i) {
            if (action->objectName() == QLatin1String(icons[i][0])) {
                icon = icons[i][1];
            }
        }
    }
    SceneBar::setIcon(button, icon);
}

// The view's draw mode and lighting actions, by script name, and the icons showing them.
const char* const kDrawModeIcons[][2] = {
    { "unshaded", "natron/draw-unshaded.svg" },
    { "shaded", "natron/draw-shaded.svg" },
    { "wireframe", "natron/draw-wireframe.svg" },
    { nullptr, nullptr },
};
const char* const kLightingIcons[][2] = {
    { "auto", "natron/lighting-auto.svg" },
    { "scene", "natron/lighting-scene.svg" },
    { "headlight", "natron/lighting-headlight.svg" },
    { nullptr, nullptr },
};

void
fillMenu(QMenu* menu,
         const QList<QAction*>& actions)
{
    menu->clear();
    menu->addActions(actions);
}

} // namespace

SceneDisplayBar::SceneDisplayBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QString::fromUtf8("sceneDisplayBar"));
    setFixedWidth(TO_DPIX(SceneBar::kBarSize));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(TO_DPIX(2), TO_DPIY(2), TO_DPIX(2), TO_DPIY(2));
    layout->setSpacing(TO_DPIY(NATRON_DENSITY_TOOLBAR_SPACING));

    _drawMode = makeMenuButton(this, "sceneDrawMode", "natron/draw-shaded.svg",
                               tr("Draw Mode: unshaded, shaded or wireframe."), &_drawModeMenu);
    _purposes = makeMenuButton(this, "scenePurposes", "natron/purpose-default.svg",
                               tr("Purposes: which of the proxy, render and guide prims the view draws besides the default ones. "
                                  "The button shows the one drawn, D for the default prims alone, or stacked tiles for several."),
                               &_purposesMenu);
    _lighting = makeMenuButton(this, "sceneLighting", "natron/lighting-scene.svg",
                               tr("Lighting: the scene's lights, a headlight, or the scene's lights when it has any."), &_lightingMenu);
    _showGrid = SceneBar::makeIconButton(this, "sceneShowGrid", "natron/show-grid.svg", true);
    setToolTipWithShortcut(kShortcutGroupViewer3D, kShortcutIDActionViewer3DShowGrid,
                           SceneBar::tipWithShortcut(tr("Show Grid: draws the ground grid.")), _showGrid);
    QObject::connect(_drawModeMenu, &QMenu::aboutToShow, this, [this]() {
        fillMenu(_drawModeMenu, _viewport ? _viewport->getDrawModeActions() : QList<QAction*>());
    });
    QObject::connect(_purposesMenu, &QMenu::aboutToShow, this, [this]() {
        fillMenu(_purposesMenu, _viewport ? _viewport->getPurposeActions() : QList<QAction*>());
    });
    QObject::connect(_lightingMenu, &QMenu::aboutToShow, this, [this]() {
        fillMenu(_lightingMenu, _viewport ? _viewport->getLightingActions() : QList<QAction*>());
    });
    QObject::connect(_showGrid, &QToolButton::clicked, this, [this]() {
        if (_viewport) {
            _viewport->getShowGridAction()->trigger();
        }
        refreshDisplay();
    });
    for (QToolButton* button : { _drawMode, _purposes, _lighting, _showGrid }) {
        layout->addWidget(button, 0, Qt::AlignHCenter);
    }

    layout->addWidget(SceneBar::makeSeparator(this, true));

    // Without shortcuts, the buttons take the actions' text, tooltip and checked state directly.
    const struct
    {
        const char* name;
        const char* icon;
        QString text;
        QString tip;
    } toggles[] = {
        { kViewerParamPointMarkers, "natron/point-markers.svg", tr("Point Markers"), tr("Point Markers: marks every point.") },
        { kViewerParamPointNumbers, "natron/point-numbers.svg", tr("Point Numbers"),
          tr("Point Numbers: numbers the points in view, up to a limit per frame.") },
        { kViewerParamPrimitiveNumbers, "natron/primitive-numbers.svg", tr("Primitive Numbers"),
          tr("Primitive Numbers: numbers the primitives in view at their centres, up to a limit per frame.") },
        { kViewerParamNormals, "natron/normals.svg", tr("Normals"), tr("Normals: draws the N point attribute as a line from each point.") },
    };
    for (const auto& toggle : toggles) {
        QToolButton* button = SceneBar::makeIconButton(this, toggle.name, toggle.icon, true);
        QAction* action = new QAction(button->icon(), toggle.text, this);
        action->setObjectName(QString::fromUtf8(toggle.name));
        action->setCheckable(true);
        action->setToolTip(toggle.tip);
        button->setDefaultAction(action);
        QObject::connect(action, &QAction::triggered, this, [this]() { edited(); });
        _geometryToggles.push_back(action);
        _geometryButtons.push_back(button);
        layout->addWidget(button, 0, Qt::AlignHCenter);
    }

    _displayOptions = SceneBar::makeIconButton(this, "sceneDisplayOptions", "lucide/settings.svg", false);
    _displayOptions->setToolTip(tr("Display Options: the normal scale and the group to highlight."));
    _displayOptions->setPopupMode(QToolButton::InstantPopup);
    QMenu* optionsMenu = new QMenu(_displayOptions);
    optionsMenu->setObjectName(QString::fromUtf8("sceneDisplayOptionsMenu"));
    QWidget* options = new QWidget(optionsMenu);
    QFormLayout* form = new QFormLayout(options);
    form->setContentsMargins(TO_DPIX(6), TO_DPIY(6), TO_DPIX(6), TO_DPIY(6));

    _normalScale = new QDoubleSpinBox(options);
    _normalScale->setObjectName(QString::fromUtf8(kViewerParamNormalScale));
    _normalScale->setDecimals(4);
    _normalScale->setRange(0., 1e6);
    _normalScale->setSingleStep(0.01);
    _normalScale->setValue(0.1);
    _normalScale->setKeyboardTracking(false);
    _normalScale->setToolTip(tr("The length, in scene units, a normal of length 1 is drawn at."));
    _normalScale->setMaximumWidth(TO_DPIX(160));
    form->addRow(tr("Normal Scale"), _normalScale);

    _highlightGroup = new QLineEdit(options);
    _highlightGroup->setObjectName(QString::fromUtf8(kViewerParamHighlightGroup));
    _highlightGroup->setToolTip(tr("The point group, else the primitive group, to highlight. Empty highlights nothing."));
    _highlightGroup->setMaximumWidth(TO_DPIX(160));
    form->addRow(tr("Highlight Group"), _highlightGroup);

    QWidgetAction* optionsAction = new QWidgetAction(optionsMenu);
    optionsAction->setDefaultWidget(options);
    optionsMenu->addAction(optionsAction);
    _displayOptions->setMenu(optionsMenu);
    layout->addWidget(_displayOptions, 0, Qt::AlignHCenter);
    layout->addStretch();

    QObject::connect(_normalScale, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() { edited(); });
    QObject::connect(_highlightGroup, &QLineEdit::editingFinished, this, [this]() { edited(); });

    setActive(false, false);
}

void
SceneDisplayBar::setViewport(SceneViewport* viewport)
{
    if (_viewport == viewport) {
        return;
    }
    if (_viewport) {
        QObject::disconnect(_viewport, nullptr, this, nullptr);
    }
    _viewport = viewport;
    if (_viewport) {
        QObject::connect(_viewport, &SceneViewport::displayStateChanged, this, [this]() { refreshDisplay(); });
    }
    refreshDisplay();
}

void
SceneDisplayBar::setActive(bool shown,
                           bool geometry)
{
    const bool scene = shown && _viewport;
    for (QToolButton* button : { _drawMode, _purposes, _lighting, _showGrid }) {
        button->setEnabled(scene);
    }
    const bool geo = scene && geometry;
    for (QAction* action : _geometryToggles) {
        action->setEnabled(geo);
    }
    _displayOptions->setEnabled(geo);
    refreshDisplay();
}

void
SceneDisplayBar::setOptions(const GeoDisplayOptions& options)
{
    const bool checked[] = { options.pointMarkers, options.pointNumbers, options.primitiveNumbers, options.normals };
    for (int i = 0; i < _geometryToggles.size(); ++i) {
        const QSignalBlocker blocker(_geometryToggles[i]);
        _geometryToggles[i]->setChecked(checked[i]);
    }
    const QSignalBlocker b0(_normalScale), b1(_highlightGroup);
    if (_normalScale->value() != options.normalScale) {
        _normalScale->setValue(options.normalScale);
    }
    const QString group = QString::fromStdString(options.highlightGroup);
    if (_highlightGroup->text() != group) {
        _highlightGroup->setText(group);
    }
}

void
SceneDisplayBar::setEditCallback(const std::function<void(const GeoDisplayOptions&)>& callback)
{
    _callback = callback;
}

const QList<QAction*>&
SceneDisplayBar::getGeometryToggleActions() const
{
    return _geometryToggles;
}

void
SceneDisplayBar::refreshDisplay()
{
    SceneBar::showActionChecked(_showGrid, _viewport ? _viewport->getShowGridAction() : nullptr);
    showCheckedIcon(_drawMode, _viewport ? _viewport->getDrawModeActions() : QList<QAction*>(), kDrawModeIcons, "natron/draw-shaded.svg");
    SceneBar::setIcon(_purposes, purposesIcon(_viewport ? _viewport->getPurposeActions() : QList<QAction*>()));
    showCheckedIcon(_lighting, _viewport ? _viewport->getLightingActions() : QList<QAction*>(), kLightingIcons, "natron/lighting-scene.svg");
}

GeoDisplayOptions
SceneDisplayBar::currentOptions() const
{
    GeoDisplayOptions options;

    options.pointMarkers = _geometryToggles[0]->isChecked();
    options.pointNumbers = _geometryToggles[1]->isChecked();
    options.primitiveNumbers = _geometryToggles[2]->isChecked();
    options.normals = _geometryToggles[3]->isChecked();
    options.normalScale = _normalScale->value();
    options.highlightGroup = _highlightGroup->text().trimmed().toStdString();

    return options;
}

void
SceneDisplayBar::edited()
{
    if (_callback) {
        _callback(currentOptions());
    }
}

NATRON_NAMESPACE_EXIT
