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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpUndo_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpUndo_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>

#include <QCoreApplication>

#include "Engine/EngineFwd.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/UndoCommand.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One edit of a SplineWarpContext's warp model (links, boundaries, correspondences) as an
 * undo step. Undo restores the model from before the edit and redo the model it left.
 **/
class SplineWarpStateUndoCommand
    : public UndoCommand {
public:
    // The first redo() runs edit, between the two snapshots.
    SplineWarpStateUndoCommand(const RotoCurveInteractPtr& roto,
                               const std::string& text,
                               const std::function<void()>& edit);

    // An edit already made, as an interactive drag is: redo() restores after.
    SplineWarpStateUndoCommand(const RotoCurveInteractPtr& roto,
                               const std::string& text,
                               const RotoCurveContextHostStatePtr& before,
                               const RotoCurveContextHostStatePtr& after);

    virtual ~SplineWarpStateUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    RotoCurveInteractWPtr _roto;
    std::function<void()> _edit;
    RotoCurveContextHostStatePtr _before;
    RotoCurveContextHostStatePtr _after;
};

/**
 * @brief Duplicate and Link: a copy of a curve, inserted just above it in its layer and selected,
 * becomes the destination of a link from the curve, paired as the copy was made so nothing moves.
 **/
class SplineWarpDuplicateAndLinkUndoCommand
    : public UndoCommand {
    Q_DECLARE_TR_FUNCTIONS(SplineWarpDuplicateAndLinkUndoCommand)

public:
    SplineWarpDuplicateAndLinkUndoCommand(const RotoCurveInteractPtr& roto,
                                          const BezierPtr& original);

    virtual ~SplineWarpDuplicateAndLinkUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

    BezierPtr getCopy() const
    {
        return _copy;
    }

private:
    RotoCurveInteractWPtr _roto;
    BezierPtr _original;
    BezierPtr _copy;
    RotoLayerPtr _layer;
    int _indexInLayer;
    RotoCurveContextHostStatePtr _before;
    RotoCurveContextHostStatePtr _after;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpUndo_h
