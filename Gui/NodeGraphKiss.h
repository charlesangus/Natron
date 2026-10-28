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

#ifndef Gui_NodeGraphKiss_h
#define Gui_NodeGraphKiss_h

#include "Global/Macros.h"

#include <cstdint>
#include <functional>
#include <vector>

NATRON_NAMESPACE_ENTER

typedef std::intptr_t KissNodeId;

// All positions are in scene coordinates.

// The tip of an input arrow of a dragged node. Only unconnected inputs can kiss.
struct KissInputTip {
    KissNodeId nodeId;
    int inputNb;
    double x, y;
    bool connected;

    KissInputTip(KissNodeId nodeId_,
                 int inputNb_,
                 double x_,
                 double y_,
                 bool connected_ = false)
        : nodeId(nodeId_)
        , inputNb(inputNb_)
        , x(x_)
        , y(y_)
        , connected(connected_)
    {
    }
};

// The output of a dragged node that feeds no other dragged node.
struct KissOutput {
    KissNodeId nodeId;
    double x, y;

    KissOutput(KissNodeId nodeId_,
               double x_,
               double y_)
        : nodeId(nodeId_)
        , x(x_)
        , y(y_)
    {
    }
};

// A node outside the dragged set, by its body rectangle. freeInput is the input an output kiss
// connects (the node's preferred input for a connection), or -1 when it has none free.
struct KissTarget {
    KissNodeId id;
    double left, top, right, bottom;
    int freeInput;

    KissTarget(KissNodeId id_,
               double left_,
               double top_,
               double right_,
               double bottom_,
               int freeInput_)
        : id(id_)
        , left(left_)
        , top(top_)
        , right(right_)
        , bottom(bottom_)
        , freeInput(freeInput_)
    {
    }
};

enum KissEndEnum {
    eKissEndNone,
    eKissEndInput, // input inputNb of draggedId takes targetId as its source
    eKissEndOutput, // input inputNb of targetId takes draggedId as its source
};

struct KissPlan {
    KissEndEnum end;
    KissNodeId draggedId;
    KissNodeId targetId;
    int inputNb;
    double distance;

    KissPlan()
        : end(eKissEndNone)
        , draggedId(-1)
        , targetId(-1)
        , inputNb(-1)
        , distance(0.)
    {
    }

    bool isValid() const
    {
        return end != eKissEndNone;
    }
};

// Returns false for a kiss that can't be made (a cycle, a connection the target refuses).
typedef std::function<bool(const KissPlan&)> KissValidator;

// Distance from (x, y) to the target's rectangle, 0 inside it.
double kissDistanceToTarget(double x,
                            double y,
                            const KissTarget& target);

// The nearest kiss within reach that the validator accepts, from an input tip or an output to a
// target's rectangle. Ties go to the lower input number (the dragged node's for an input kiss,
// the target's for an output kiss), then to an input kiss, then to the earlier tip or output, then
// to the earlier target. A target is never kissed by itself. Returns an invalid plan when none fits.
KissPlan planKiss(const std::vector<KissInputTip>& inputs,
                  const std::vector<KissOutput>& outputs,
                  const std::vector<KissTarget>& targets,
                  double reach,
                  const KissValidator& isValid = KissValidator());

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphKiss_h
