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

#include <filesystem>
#include <system_error>

#include <QCoreApplication>
#include <QString>

#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

TexturePaintBakeOutputUndoCommand::TexturePaintBakeOutputUndoCommand(const NodePtr& node,
                                                                     const TexturePaint::BakeOutputState& state,
                                                                     const std::vector<std::string>& files,
                                                                     const std::vector<std::pair<std::string, std::string>>& backups,
                                                                     const std::string& backupDirectory)
    : UndoCommand(QCoreApplication::translate("TexturePaint", "Bake Output").toStdString())
    , _node(node)
    , _state(state)
    , _files(files)
    , _redoable(true)
    , _backups(backups)
    , _backupDirectory(backupDirectory)
    , _firstRedo(true)
{
}

TexturePaintBakeOutputUndoCommand::~TexturePaintBakeOutputUndoCommand()
{
    std::error_code ec;
    std::filesystem::remove_all(_backupDirectory, ec);
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
    if (!paint) {
        return;
    }
    // What the bake wrote is about to be overwritten with the files it replaced.
    _stashed.clear();
    _redoable = true;
    const std::string stashDirectory = _backupDirectory + "/redo";
    std::error_code ec;
    std::filesystem::create_directories(stashDirectory, ec);
    for (const std::string& file : _files) {
        const std::string copy = stashDirectory + "/" + std::to_string(_stashed.size()) + "_" + std::filesystem::path(file).filename().string();
        if (std::filesystem::copy_file(file, copy, std::filesystem::copy_options::overwrite_existing, ec)) {
            _stashed.push_back(std::make_pair(file, copy));
        } else {
            _redoable = false;
        }
    }
    for (const std::pair<std::string, std::string>& backup : _backups) {
        std::filesystem::copy_file(backup.second, backup.first, std::filesystem::copy_options::overwrite_existing, ec);
    }
    paint->restoreBakeOutputState(_state);
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
    if (!paint) {
        return;
    }
    if (!_redoable) {
        paint->setPersistentMessage(eMessageTypeError, QCoreApplication::translate("TexturePaint", "The files of this bake could not be kept when it was undone, so it cannot be redone: bake the output again.").toStdString());

        return;
    }
    for (const std::pair<std::string, std::string>& stashed : _stashed) {
        std::error_code ec;
        if (!std::filesystem::copy_file(stashed.second, stashed.first, std::filesystem::copy_options::overwrite_existing, ec)) {
            paint->setPersistentMessage(eMessageTypeError, QCoreApplication::translate("TexturePaint", "%1 cannot be put back: %2").arg(QString::fromStdString(stashed.first)).arg(QString::fromStdString(ec.message())).toStdString());

            return;
        }
    }
    paint->reapplyBakeOutputState(_state);
}

NATRON_NAMESPACE_EXIT
