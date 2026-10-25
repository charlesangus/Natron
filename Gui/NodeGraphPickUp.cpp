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

#include "NodeGraphPickUp.h"

NATRON_NAMESPACE_ENTER

bool
isDragDistance(double pressToReleaseDistance,
               double dragDistance)
{
    return pressToReleaseDistance >= dragDistance;
}

bool
shouldStartPickUp(PickUpButtonEnum releasedButton,
                  double pressToReleaseDistance,
                  double dragDistance,
                  bool isOutputEdge)
{
    return (releasedButton == ePickUpButtonLeft) && !isDragDistance(pressToReleaseDistance, dragDistance) && !isOutputEdge;
}

PickUpEndEnum
pickedUpEnd(const QPointF& click,
            const QPointF& destination,
            const QPointF& source)
{
    const QPointF toDestination = click - destination;
    const QPointF toSource = click - source;
    const double destinationDist2 = QPointF::dotProduct(toDestination, toDestination);
    const double sourceDist2 = QPointF::dotProduct(toSource, toSource);

    return (destinationDist2 < sourceDist2) ? ePickUpEndDestination : ePickUpEndSource;
}

PickUpPressActionEnum
decidePickUpPress(PickUpButtonEnum button,
                  PickUpClickTargetEnum target)
{
    switch (button) {
    case ePickUpButtonLeft:
        switch (target) {
        case ePickUpClickTargetNode:
            return ePickUpPressConnect;
        case ePickUpClickTargetNavigator:
            return ePickUpPressPassThrough;
        case ePickUpClickTargetOther:
            return ePickUpPressDisconnect;
        case ePickUpClickTargetHoldingNode:
        default:
            return ePickUpPressCancel;
        }
    case ePickUpButtonRight:
        return ePickUpPressCancel;
    case ePickUpButtonMiddle:
        return ePickUpPressPassThrough;
    case ePickUpButtonOther:
    default:
        return ePickUpPressIgnore;
    }
}

NATRON_NAMESPACE_EXIT
