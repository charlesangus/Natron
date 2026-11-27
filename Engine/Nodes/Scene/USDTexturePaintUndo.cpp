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

#include "USDTexturePaintUndo.h"

#include <QCoreApplication>

#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

USDTexturePaintBakeOutputUndoCommand::USDTexturePaintBakeOutputUndoCommand(const NodePtr& node,
                                                                           const USDTexturePaint::BakeOutputState& state,
                                                                           const std::shared_ptr<USDTexturePaint::BakeOutputFiles>& files)
    : UndoCommand(QCoreApplication::translate("USDTexturePaint", "Bake Output").toStdString())
    , _node(node)
    , _state(state)
    , _files(files)
    , _applied(std::make_shared<bool>(true))
    , _firstRedo(true)
{
}

USDTexturePaintBakeOutputUndoCommand::~USDTexturePaintBakeOutputUndoCommand()
{
    USDTexturePaint::enqueueBakeOutputDiscard(_files);
}

std::shared_ptr<USDTexturePaint>
USDTexturePaintBakeOutputUndoCommand::getPaint() const
{
    NodePtr node = _node.lock();

    return node ? std::dynamic_pointer_cast<USDTexturePaint>(node->getEffectInstance()) : std::shared_ptr<USDTexturePaint>();
}

void
USDTexturePaintBakeOutputUndoCommand::undo()
{
    std::shared_ptr<USDTexturePaint> paint = getPaint();

    if (paint) {
        (void)paint->enqueueBakeOutputSwap(_files, _state, true, _applied);
    }
}

void
USDTexturePaintBakeOutputUndoCommand::redo()
{
    // The bake was done before the command was pushed.
    if (_firstRedo) {
        _firstRedo = false;

        return;
    }
    std::shared_ptr<USDTexturePaint> paint = getPaint();
    if (paint) {
        (void)paint->enqueueBakeOutputSwap(_files, _state, false, _applied);
    }
}

NATRON_NAMESPACE_EXIT
