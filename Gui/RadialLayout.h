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

#ifndef NATRON_GUI_RADIALLAYOUT_H
#define NATRON_GUI_RADIALLAYOUT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/EngineFwd.h"

#include "Gui/ConnectorLayout.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The classic layout: every connector is anchored at the centre of the node and its edge is
 * clipped against the node's outline. Unconnected inputs fan out over the top half at evenly
 * spaced angles; side inputs and the first two masks point sideways, and inspectors show only
 * their first inputs on top. The output leaves downwards.
 **/
class RadialLayout
    : public ConnectorLayout {
public:
    explicit RadialLayout(NodeGui* node);

    virtual ~RadialLayout() OVERRIDE;

    /**
     * @brief Inspectors, such as Merge and Switch, show two inputs and unfold the rest one at a
     * time on the left. A native node with a run of numbered inputs is laid out the same way,
     * however long the run, so that its visible inputs are not spread as if every input of the run
     * were shown.
     **/
    static bool unfoldsInputs(const NodePtr& node);

    virtual void resetInputs(std::size_t inputCount) OVERRIDE FINAL;

    virtual void layoutInputs() OVERRIDE FINAL;

    virtual ConnectorAnchor inputAnchor(int inputNb, const QGraphicsItem* space) const OVERRIDE FINAL;

    virtual ConnectorAnchor outputAnchor(const std::string& outputName, const QGraphicsItem* space) const OVERRIDE FINAL;

    virtual ConnectorHit hitTest(const QPointF& scenePos,
                                 double grabRadius,
                                 double unconnectedInputGrabRadius) const OVERRIDE FINAL;

    /**
     * @brief Counter-clockwise from the positive x axis, in radians; 0 for an input never placed.
     **/
    double getInputAngle(int inputNb) const;

private:
    void layoutInputsForInspector();

    void setInputAngle(std::size_t inputNb, double angle);

    ConnectorAnchor anchorAtCenter(const QGraphicsItem* space) const;

    std::vector<double> _inputAngles;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_RADIALLAYOUT_H
