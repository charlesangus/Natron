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

#include "TexturePaintUndo.h"

#include <thread>

#include <QCoreApplication>

#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

TexturePaintBakeOutputUndoCommand::TexturePaintBakeOutputUndoCommand(const NodePtr& node,
                                                                     const TexturePaint::BakeOutputState& state,
                                                                     const std::shared_ptr<TexturePaint::BakeOutputFiles>& files)
    : UndoCommand(QCoreApplication::translate("TexturePaint", "Bake Output").toStdString())
    , _node(node)
    , _state(state)
    , _files(files)
    , _firstRedo(true)
{
}

TexturePaintBakeOutputUndoCommand::~TexturePaintBakeOutputUndoCommand()
{
    if (!_files) {
        return;
    }
    std::shared_ptr<TexturePaint> paint = getPaint();
    if (paint) {
        paint->enqueueBakeOutputDiscard(_files);

        return;
    }
    const std::shared_ptr<TexturePaint::BakeOutputFiles> files = _files;
    std::thread([files]() {
        TexturePaint::discardBakeOutputFiles(*files);
    }).detach();
}

std::shared_ptr<TexturePaint>
TexturePaintBakeOutputUndoCommand::getPaint() const
{
    NodePtr node = _node.lock();

    return node ? std::dynamic_pointer_cast<TexturePaint>(node->getEffectInstance()) : std::shared_ptr<TexturePaint>();
}

void
TexturePaintBakeOutputUndoCommand::undo()
{
    std::shared_ptr<TexturePaint> paint = getPaint();

    if (paint) {
        (void)paint->enqueueBakeOutputSwap(_files, _state, true);
    }
}

void
TexturePaintBakeOutputUndoCommand::redo()
{
    // The bake was done before the command was pushed.
    if (_firstRedo) {
        _firstRedo = false;

        return;
    }
    std::shared_ptr<TexturePaint> paint = getPaint();
    if (paint) {
        (void)paint->enqueueBakeOutputSwap(_files, _state, false);
    }
}

NATRON_NAMESPACE_EXIT
