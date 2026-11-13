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

#ifndef Engine_Nodes_Scene_TexturePaintUndo_h
#define Engine_Nodes_Scene_TexturePaintUndo_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/TexturePaint.h"
#include "Engine/UndoCommand.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The undo step of a TexturePaint Bake Output, which the bake has done already. Undoing it
 * puts back the output files an earlier bake had written, from copies the command owns, unfreezes
 * the sessions it froze and sets the bake record back, so those sessions render from their
 * strokes again. Redoing it bakes again over the same sessions.
 **/
class TexturePaintBakeOutputUndoCommand
    : public UndoCommand {
public:
    TexturePaintBakeOutputUndoCommand(const NodePtr& node,
                                      double time,
                                      ViewIdx view,
                                      const TexturePaint::BakeOutputState& state,
                                      const std::vector<std::pair<std::string, std::string>>& backups,
                                      const std::string& backupDirectory);

    virtual ~TexturePaintBakeOutputUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    std::shared_ptr<TexturePaint> getPaint() const;

    NodeWPtr _node;
    double _time;
    ViewIdx _view;
    TexturePaint::BakeOutputState _state;
    // (output file, copy of it before the bake) pairs.
    std::vector<std::pair<std::string, std::string>> _backups;
    std::string _backupDirectory;
    bool _firstRedo;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TexturePaintUndo_h
