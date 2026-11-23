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

#ifndef Engine_LiveSession_SessionRemoteEdits_h
#define Engine_LiveSession_SessionRemoteEdits_h

#include "Global/Macros.h"

#include <vector>

#include <QtCore/QStringList>

#include "Serialization/Ops/GraphOp.h"

NATRON_NAMESPACE_ENTER
namespace LiveSession {

/**
 * @brief Sorts the nodes another user's operations reached into those they deleted and those they
 * changed otherwise, each listed once.
 **/
void remoteEditedNodes(const std::vector<Serialization::GraphOp>& ops,
                       QStringList* deleted,
                       QStringList* edited);

/**
 * @brief Marks the time another user's edits, or the host's project, are applied here. Nothing
 * done then is this user's own edit, so nothing may reach this user's undo history.
 *
 * Main thread only.
 **/
class RemoteApply {
public:
    RemoteApply();
    ~RemoteApply();

    RemoteApply(const RemoteApply&) = delete;
    RemoteApply& operator=(const RemoteApply&) = delete;

    static bool isApplying();

private:
    static int _depth;
};

} // namespace LiveSession
NATRON_NAMESPACE_EXIT

#endif // Engine_LiveSession_SessionRemoteEdits_h
