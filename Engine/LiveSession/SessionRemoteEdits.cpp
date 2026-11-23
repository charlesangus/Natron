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

#include "SessionRemoteEdits.h"

#include <optional>
#include <string>
#include <variant>

#include "Engine/LiveSession/SessionShared.h"

NATRON_NAMESPACE_ENTER
namespace LiveSession {

void
remoteEditedNodes(const std::vector<Serialization::GraphOp>& ops,
                  QStringList* deleted,
                  QStringList* edited)
{
    for (const Serialization::GraphOp& op : ops) {
        if (const Serialization::DeleteNodeOp* deletion = std::get_if<Serialization::DeleteNodeOp>(&op)) {
            const QString id = QString::fromStdString(deletion->nodeId);
            edited->removeAll(id);
            if (!deleted->contains(id)) {
                deleted->push_back(id);
            }
            continue;
        }
        if (std::optional<std::string> target = graphOpTargetNode(op)) {
            const QString id = QString::fromStdString(*target);
            if (!deleted->contains(id) && !edited->contains(id)) {
                edited->push_back(id);
            }
        }
    }
}

int RemoteApply::_depth = 0;

RemoteApply::RemoteApply()
{
    ++_depth;
}

RemoteApply::~RemoteApply()
{
    --_depth;
}

bool
RemoteApply::isApplying()
{
    return _depth > 0;
}

} // namespace LiveSession
NATRON_NAMESPACE_EXIT
