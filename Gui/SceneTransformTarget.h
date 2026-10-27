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

#ifndef NATRON_GUI_SCENETRANSFORMTARGET_H
#define NATRON_GUI_SCENETRANSFORMTARGET_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>
#include <vector>

#include <QtCore/QString>

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"

// Where a drag of the 3D view's transform widget lands. Matrices are Gf (row-vector) matrices
// stored as 16 doubles row by row, so that this header needs no USD.

NATRON_NAMESPACE_ENTER

namespace SceneTransform {
enum ToolEnum {
    eToolSelect = 0,
    eToolTranslate,
    eToolRotate,
    eToolScale
};

// The frame the handles point along: the world's axes, or the current prim's.
enum SpaceEnum {
    eSpaceWorld = 0,
    eSpaceLocal
};

// Where the handles sit: the centre of the selection's bounds, or the current prim's origin.
enum PivotEnum {
    ePivotSelectionCentre = 0,
    ePivotCurrentPrim
};
} // namespace SceneTransform

// An op of a prim's op order, and the matrix that places the op's result in the world.
struct SceneTransformOp {
    std::string name;
    double space[16];
};

// A prim a drag moves, as it was when the drag began.
struct SceneTransformPrim {
    std::string path;
    double world[16];
    double parentToWorld[16];
    bool resetsXformStack = false;
    std::vector<SceneTransformOp> ops;
};

/**
 * @brief A drag of the transform widget: every prim's world transform W becomes W * delta. The
 * prims are the selection's xformable prims without those under another of them, the current
 * one last; pivot is the world point the widget sat at.
 **/
struct SceneTransformDrag {
    std::vector<SceneTransformPrim> prims;
    double delta[16];
    SceneTransform::ToolEnum tool = SceneTransform::eToolSelect;
    double frame = 0.;
    double pivot[3] = { 0., 0., 0. };
};

class SceneTransformTarget {
public:
    virtual ~SceneTransformTarget() { }

    /**
     * @brief Writes drag where it belongs, as one undo step. Returns whether anything was
     * written, so that the scene will change to show it.
     **/
    virtual bool commit(const SceneTransformDrag& drag) = 0;
};

// Keeps the drags it is given and writes nothing, for tests.
class SceneTransformRecorder
    : public SceneTransformTarget {
public:
    virtual bool commit(const SceneTransformDrag& drag) OVERRIDE FINAL;

    int getCount() const
    {
        return _count;
    }

    // The last drag committed; meaningless while getCount() is 0.
    const SceneTransformDrag& getLast() const
    {
        return _last;
    }

private:
    int _count = 0;
    SceneTransformDrag _last;
};

/**
 * @brief A change to one dimension of a double knob, at frame when the dimension is animated
 * (which keys it there). hadKey tells whether the animated dimension had a key at frame before.
 **/
struct SceneKnobEdit {
    KnobDoubleWPtr knob;
    int dimension = 0;
    double oldValue = 0.;
    double newValue = 0.;
    bool animated = false;
    bool hadKey = false;
};

// The edit of knob's dimension to value at frame.
SceneKnobEdit makeSceneKnobEdit(const KnobDoublePtr& knob, int dimension, double frame, double value);

/**
 * @brief Writes edits, which belong to node, at frame, then records them in history as one
 * command labelled text, whose undo restores the old values and removes keys it added. Without a
 * history the edits are only written.
 **/
void applySceneKnobEdits(const NodePtr& node,
                         double frame,
                         const std::vector<SceneKnobEdit>& edits,
                         UndoHistory* history,
                         const QString& text);

/**
 * @brief The policy the Preferences choose: the knobs of each prim's owner node, else an
 * USDEditTransform; the USDEditTransform whose panel is open; or one at the end of the viewed
 * branch. A USDEditTransform it creates goes after the viewed node, its pattern naming the
 * prims, in the same undo step as the edit.
 **/
class SceneTransformPolicyTarget
    : public SceneTransformTarget {
public:
    struct Context {
        // The node the 3D view shows.
        std::function<NodePtr()> viewedNode;
        // The USDEditTransform whose settings panel is open, if any.
        std::function<NodePtr()> openEditNode;
        std::function<UndoHistory*()> undoHistory;
    };

    explicit SceneTransformPolicyTarget(const Context& context);

    virtual bool commit(const SceneTransformDrag& drag) OVERRIDE FINAL;

private:
    Context _context;
};

/**
 * @brief The node upstream of viewed whose transform knobs place the prim at primPath in viewed's
 * scene: a node with translate, rotate, scale, uniform scale and rotate order knobs whose Prim
 * Path is the prim's, reached through re-parenting nodes (which graft their inputs under their
 * own Prim Path) and not shadowed by a Replace-mode USDEditTransform matching the prim on the
 * way. Null for a prim no such node makes, such as one read from a file.
 **/
NodePtr findSceneTransformOwner(const NodePtr& viewed, const std::string& primPath);

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENETRANSFORMTARGET_H
