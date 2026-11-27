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

#include "SceneTopBar.h"

#include <QHBoxLayout>
#include <QMenu>
#include <QToolButton>

#include "Engine/AppManager.h"
#include "Engine/Utils.h" // convertFromPlainText
#include "Engine/ViewerInstance.h"
#include "Gui/Button.h"
#include "Gui/GuiDensity.h"
#include "Gui/Label.h"
#include "Gui/SceneBarCommon.h"
#include "Gui/SceneViewport.h"
#include "Gui/SpinBox.h"

NATRON_NAMESPACE_ENTER

SceneTopBar::SceneTopBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QString::fromUtf8("sceneTopBar"));
    // At least the buttons' height whatever is shown, so hiding controls never moves the view.
    setMinimumHeight(TO_DPIY(SceneBar::kBarSize + 2));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setSpacing(TO_DPIX(NATRON_DENSITY_TOOLBAR_SPACING));
    layout->setContentsMargins(TO_DPIX(NATRON_DENSITY_TOOLBAR_SPACING * 2), TO_DPIY(1), TO_DPIX(NATRON_DENSITY_TOOLBAR_SPACING * 2), TO_DPIY(1));

    typedef void (SceneViewport::*Populate)(QMenu*);
    const struct
    {
        const char* name;
        const char* icon;
        QString tip;
        Populate populate;
    } menus[] = {
        { "sceneRenderer", "natron/renderer.svg", tr("Renderer: the render delegate that draws the 3D view."), &SceneViewport::populateRendererMenu },
        { "sceneLookThrough", "natron/look-through.svg", tr("Look Through: the camera or light the 3D view looks through, or the viewer's own camera."),
          &SceneViewport::populateLookThroughMenu },
        { "sceneView", "natron/view-perspective.svg", tr("View: perspective or an orthographic view along an axis. The button shows which."), &SceneViewport::populateViewMenu },
        { "sceneClipping", "natron/clipping.svg", tr("Clipping: the near and far clipping planes, or ones fitted to the scene."),
          &SceneViewport::populateClippingMenu },
    };
    for (int i = 0; i < 4; ++i) {
        QToolButton* button = SceneBar::makeIconButton(this, menus[i].name, menus[i].icon, false);
        button->setToolTip(menus[i].tip);
        button->setPopupMode(QToolButton::InstantPopup);
        QMenu* menu = new QMenu(button);
        button->setMenu(menu);
        const Populate populate = menus[i].populate;
        QObject::connect(menu, &QMenu::aboutToShow, this, [this, menu, populate]() {
            if (_viewport) {
                (_viewport->*populate)(menu);
            }
        });
        _menus[i] = button;
        layout->addWidget(button);
    }

    layout->addWidget(SceneBar::makeSeparator(this, false));

    const QString maxSamplesTt = NATRON_NAMESPACE::convertFromPlainText(tr("The samples per pixel the renderer refines the 3D view to; "
                                                                           "0 uses the renderer's default. A change restarts the render."),
                                                                        NATRON_NAMESPACE::WhiteSpaceNormal);
    _maxSamplesLabel = new Label(tr("Max Samples"), this);
    _maxSamplesLabel->setToolTip(maxSamplesTt);
    layout->addWidget(_maxSamplesLabel);
    _maxSamples = new SpinBox(this, SpinBox::eSpinBoxTypeInt);
    _maxSamples->setObjectName(QString::fromUtf8(kViewerParamMaxSamples));
    _maxSamples->setMinimum(0);
    _maxSamples->setValue(0);
    _maxSamples->setMinimumWidth(_maxSamples->fontMetrics().horizontalAdvance(QString::fromUtf8("1048576")) + TO_DPIX(24));
    _maxSamples->setToolTip(maxSamplesTt);
    layout->addWidget(_maxSamples);

    _refinementPaused = new Button(tr("Pause"), this);
    _refinementPaused->setObjectName(QString::fromUtf8(kViewerParamRefinementPaused));
    _refinementPaused->setFocusPolicy(Qt::NoFocus);
    _refinementPaused->setCheckable(true);
    _refinementPaused->setChecked(false);
    _refinementPaused->setDown(false);
    _refinementPaused->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("Holds the refinement at its current samples; "
                                                                            "releasing it continues from them. Edits wait until it is released."),
                                                                         NATRON_NAMESPACE::WhiteSpaceNormal));
    layout->addWidget(_refinementPaused);

    _renderEverything = new Button(tr("Render Everything"), this);
    _renderEverything->setObjectName(QString::fromUtf8(kViewerParamRenderEverything));
    _renderEverything->setFocusPolicy(Qt::NoFocus);
    _renderEverything->setCheckable(true);
    _renderEverything->setChecked(false);
    _renderEverything->setDown(false);
    _renderEverything->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("Renders the whole scene, ignoring the draw states, and loads every payload. "
                                                                            "Turning it off brings the draw states back and unloads nothing."),
                                                                         NATRON_NAMESPACE::WhiteSpaceNormal));
    layout->addWidget(_renderEverything);

    _refineSamples = new Label(QString(), this);
    _refineSamples->setObjectName(QString::fromUtf8("refineSamples"));
    _refineSamples->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("The samples per pixel of the frame shown, against the cap."),
                                                                      NATRON_NAMESPACE::WhiteSpaceNormal));
    layout->addWidget(_refineSamples);

    _toolOptions = new QWidget(this);
    _toolOptions->setObjectName(QString::fromUtf8("sceneToolOptions"));
    _toolOptionsLayout = new QHBoxLayout(_toolOptions);
    _toolOptionsLayout->setContentsMargins(TO_DPIX(NATRON_DENSITY_TOOLBAR_SPACING * 2), 0, 0, 0);
    _toolOptionsLayout->setSpacing(TO_DPIX(NATRON_DENSITY_TOOLBAR_SPACING));
    layout->addWidget(_toolOptions);
    layout->addStretch();
    _toolOptions->hide();

    setActive(false, false);
}

void
SceneTopBar::setViewport(SceneViewport* viewport)
{
    if (_viewport == viewport) {
        return;
    }
    if (_viewport) {
        QObject::disconnect(_viewport, nullptr, this, nullptr);
    }
    _viewport = viewport;
    if (_viewport) {
        QObject::connect(_viewport, &SceneViewport::displayStateChanged, this, [this]() { refreshView(); });
    }
    refreshView();
}

void
SceneTopBar::refreshView()
{
    const bool perspective = !_viewport || (_viewport->getRenderer().getViewMode() == SceneViewportRenderer::eViewModePerspective);

    SceneBar::setIcon(_menus[2], perspective ? "natron/view-perspective.svg" : "natron/view-orthographic.svg");
}

void
SceneTopBar::setActive(bool shown,
                       bool progressive)
{
    for (QToolButton* button : _menus) {
        button->setEnabled(shown && _viewport);
    }
    _maxSamplesLabel->setVisible(progressive);
    _maxSamples->setVisible(progressive);
    _refinementPaused->setVisible(progressive);
    _refineSamples->setVisible(progressive);
}

SpinBox*
SceneTopBar::getMaxSamplesBox() const
{
    return _maxSamples;
}

Button*
SceneTopBar::getRefinementPausedButton() const
{
    return _refinementPaused;
}

Button*
SceneTopBar::getRenderEverythingButton() const
{
    return _renderEverything;
}

Label*
SceneTopBar::getRefineSamplesLabel() const
{
    return _refineSamples;
}

void
SceneTopBar::addToolOptions(QWidget* container)
{
    if (!container || hasToolOptions(container)) {
        return;
    }
    _toolOptionsLayout->addWidget(container);
    refreshToolOptions();
}

bool
SceneTopBar::hasToolOptions(QWidget* container) const
{
    return container && (_toolOptionsLayout->indexOf(container) >= 0);
}

void
SceneTopBar::removeToolOptions(QWidget* container)
{
    if (!hasToolOptions(container)) {
        return;
    }
    _toolOptionsLayout->removeWidget(container);
    refreshToolOptions();
}

void
SceneTopBar::refreshToolOptions()
{
    _toolOptions->setVisible(_toolOptionsLayout->count() > 0);
}

NATRON_NAMESPACE_EXIT
