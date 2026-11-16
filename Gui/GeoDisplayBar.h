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

#ifndef Gui_GeoDisplayBar_h
#define Gui_GeoDisplayBar_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>

#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;

NATRON_NAMESPACE_ENTER

struct GeoDisplayOptions;

/**
 * @brief The viewer toolbar row of geometry display toggles. Each control is named after the
 * viewer knob it shows.
 **/
class GeoDisplayBar
    : public QWidget {
public:
    explicit GeoDisplayBar(QWidget* parent);

    // Shows @p options without calling the edit callback.
    void setOptions(const GeoDisplayOptions& options);

    // Called with the options the controls show after the user changes one.
    void setEditCallback(const std::function<void(const GeoDisplayOptions&)>& callback);

    // Shows the pick mode, a GeoComponentEnum, without calling the pick mode callback.
    void setPickMode(int mode);

    // Called with the pick mode the user chose.
    void setPickModeCallback(const std::function<void(int)>& callback);

private:
    GeoDisplayOptions currentOptions() const;
    void edited();

    QCheckBox* _pointMarkers;
    QCheckBox* _pointNumbers;
    QCheckBox* _primitiveNumbers;
    QCheckBox* _normals;
    QDoubleSpinBox* _normalScale;
    QLineEdit* _highlightGroup;
    QButtonGroup* _pickModes;
    std::function<void(int)> _pickModeCallback;
    std::function<void(const GeoDisplayOptions&)> _callback;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_GeoDisplayBar_h
