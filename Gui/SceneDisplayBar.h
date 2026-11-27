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

#ifndef Gui_SceneDisplayBar_h
#define Gui_SceneDisplayBar_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>

#include <QList>
#include <QPointer>
#include <QWidget>

class QAction;
class QDoubleSpinBox;
class QLineEdit;
class QMenu;
class QToolButton;

NATRON_NAMESPACE_ENTER

class SceneViewport;
struct GeoDisplayOptions;

/**
 * @brief The 3D view's right bar: draw mode, purposes, lighting and grid, then the geometry
 * display toggles and a popup for the geometry display options. Each geometry control is named
 * after the viewer knob it shows. The view's display actions and the bar's geometry toggle
 * actions are the ones the view's context menu holds, so both always agree.
 **/
class SceneDisplayBar
    : public QWidget {
public:
    explicit SceneDisplayBar(QWidget* parent);

    // The view whose display state the buttons set and show; set once it exists.
    void setViewport(SceneViewport* viewport);

    /**
     * @brief Whether the buttons act: the scene display while the 3D view is shown, the geometry
     * display while it shows geometry. A control that cannot act is disabled, never hidden.
     **/
    void setActive(bool shown, bool geometry);

    // Shows @p options without calling the edit callback.
    void setOptions(const GeoDisplayOptions& options);

    // Called with the options the controls show after the user changes one.
    void setEditCallback(const std::function<void(const GeoDisplayOptions&)>& callback);

    // The checkable point marker, point number, primitive number and normal toggles, in that order.
    const QList<QAction*>& getGeometryToggleActions() const;

private:
    void refreshDisplay();
    GeoDisplayOptions currentOptions() const;
    void edited();

    QPointer<SceneViewport> _viewport;
    QToolButton* _drawMode;
    QToolButton* _purposes;
    QToolButton* _lighting;
    QToolButton* _showGrid;
    QMenu* _drawModeMenu;
    QMenu* _purposesMenu;
    QMenu* _lightingMenu;
    QList<QAction*> _geometryToggles;
    QToolButton* _displayOptions;
    QDoubleSpinBox* _normalScale;
    QLineEdit* _highlightGroup;
    std::function<void(const GeoDisplayOptions&)> _callback;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneDisplayBar_h
