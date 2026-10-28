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

#include "SplineWarpUndo.h"

#include <sstream>

#include "Engine/Bezier.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/RotoLayer.h"

NATRON_NAMESPACE_ENTER

SplineWarpStateUndoCommand::SplineWarpStateUndoCommand(const RotoCurveInteractPtr& roto,
                                                       const std::string& text,
                                                       const std::function<void()>& edit)
    : UndoCommand(text)
    , _roto(roto)
    , _edit(edit)
    , _before()
    , _after()
{
}

SplineWarpStateUndoCommand::SplineWarpStateUndoCommand(const RotoCurveInteractPtr& roto,
                                                       const std::string& text,
                                                       const RotoCurveContextHostStatePtr& before,
                                                       const RotoCurveContextHostStatePtr& after)
    : UndoCommand(text)
    , _roto(roto)
    , _edit()
    , _before(before)
    , _after(after)
{
}

SplineWarpStateUndoCommand::~SplineWarpStateUndoCommand()
{
}

void
SplineWarpStateUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto || !_before) {
        return;
    }
    roto->getCurveContext()->restoreHostState(_before);
    roto->redrawOverlays();
}

void
SplineWarpStateUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    RotoCurveContextPtr context = roto->getCurveContext();
    if (_edit) {
        _before = context->saveHostState();
        std::function<void()> edit;
        std::swap(edit, _edit);
        edit();
        _after = context->saveHostState();
    } else if (_after) {
        context->restoreHostState(_after);
    }
    roto->redrawOverlays();
}

namespace {
std::string
copyNameOf(const RotoCurveContextPtr& context,
           const RotoItemPtr& original)
{
    std::string name = original->getScriptName() + "_copy";
    int i = 1;

    while (context->getItemByName(name)) {
        std::stringstream ss;
        ss << original->getScriptName() << "_copy" << i;
        name = ss.str();
        ++i;
    }

    return name;
}
} // anonymous namespace

SplineWarpDuplicateAndLinkUndoCommand::SplineWarpDuplicateAndLinkUndoCommand(const RotoCurveInteractPtr& roto,
                                                                             const BezierPtr& original)
    : UndoCommand()
    , _roto(roto)
    , _original(original)
    , _copy()
    , _layer()
    , _indexInLayer(0)
    , _before()
    , _after()
{
    setText(tr("Duplicate and Link").toStdString());
}

SplineWarpDuplicateAndLinkUndoCommand::~SplineWarpDuplicateAndLinkUndoCommand()
{
}

void
SplineWarpDuplicateAndLinkUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto || !_copy) {
        return;
    }
    // Removing the copy drops its link; the restore puts back what the link replaced.
    roto->removeCurve(_copy);
    roto->getCurveContext()->restoreHostState(_before);
    roto->setSelection(_original, std::make_pair(BezierCPPtr(), BezierCPPtr()));
    roto->evaluate(true);
}

void
SplineWarpDuplicateAndLinkUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto || !_original) {
        return;
    }
    RotoCurveContextPtr context = roto->getCurveContext();
    SplineWarpContextPtr warp = std::dynamic_pointer_cast<SplineWarpContext>(context);
    if (!warp) {
        return;
    }
    if (_copy) {
        context->addItem(_layer, _indexInLayer, _copy, RotoItem::eSelectionReasonOverlayInteract);
        context->restoreHostState(_after);
    } else {
        _before = context->saveHostState();
        _copy = std::dynamic_pointer_cast<Bezier>(context->makeDrawableCopy(_original, copyNameOf(context, _original)));
        if (!_copy) {
            return;
        }
        _layer = _original->getParentLayer();
        _indexInLayer = _layer ? _layer->getChildIndex(_original) : 0;
        context->addItem(_layer, _indexInLayer, _copy, RotoItem::eSelectionReasonOverlayInteract);
        warp->link(_original, _copy, true);
        _after = context->saveHostState();
    }
    roto->setSelection(_copy, std::make_pair(BezierCPPtr(), BezierCPPtr()));
    roto->evaluate(true);
}

NATRON_NAMESPACE_EXIT
