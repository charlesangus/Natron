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

#include "Serialization/Ops/GraphOp.h"

#include <type_traits>

NATRON_NAMESPACE_ENTER
namespace Serialization {

bool
isWellFormedNodeId(std::string_view id)
{
    if (id.size() != 32) {
        return false;
    }
    for (char c : id) {
        if (!(((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')))) {
            return false;
        }
    }

    return true;
}

std::string_view
graphOpName(const GraphOp& op)
{
    return std::visit([](const auto& value) -> std::string_view {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, CreateNodeOp>) {
            return "createNode";
        } else if constexpr (std::is_same_v<T, DeleteNodeOp>) {
            return "deleteNode";
        } else if constexpr (std::is_same_v<T, SetKnobOp>) {
            return "setKnob";
        } else if constexpr (std::is_same_v<T, SetInputOp>) {
            return "setInput";
        } else if constexpr (std::is_same_v<T, RenameOp>) {
            return "rename";
        } else if constexpr (std::is_same_v<T, SetNodeUiOp>) {
            return "setNodeUi";
        } else if constexpr (std::is_same_v<T, SetUserKnobsOp>) {
            return "setUserKnobs";
        } else if constexpr (std::is_same_v<T, SetRotoDataOp>) {
            return "setRotoData";
        } else if constexpr (std::is_same_v<T, SetTrackerDataOp>) {
            return "setTrackerData";
        } else {
            static_assert(std::is_same_v<T, SetProjectDataOp>);

            return "setProjectData";
        }
    },
                      op);
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
