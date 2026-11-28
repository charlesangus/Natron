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

#ifndef Engine_Nodes_Loop_LoopPair_h
#define Engine_Nodes_Loop_LoopPair_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <span>
#include <string>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A kind of loop whose begin and end nodes can be created together, as a working empty
 * loop. name is how the nodes menu and the Python API name the pair.
 **/
struct LoopPairKind {
    const char* name;
    const char* beginPluginID;
    const char* endPluginID;
};

struct LoopPairNodes {
    NodePtr begin;
    NodePtr end;
};

/**
 * @brief Every kind of loop pair, in the order the nodes menu lists them.
 **/
std::span<const LoopPairKind> loopPairKinds();

/**
 * @brief The kind named name, null for none.
 **/
const LoopPairKind* findLoopPairKind(const std::string& name);

/**
 * @brief Creates kind's begin node, then its end node fed by the begin, as one undo step named
 * after the kind. makeArgs fills the creation arguments of each node, as for a single node; the
 * plug-in ID is then set to the begin's or the end's. Created with auto-connection, the begin
 * connects to the selection as any created node does and the end goes between the begin and the
 * begin's outputs. Otherwise the end is placed below the begin. Both end up selected in their node
 * graph. When the end cannot be created the begin is removed and both are returned null.
 **/
LoopPairNodes createLoopPair(const AppInstancePtr& app,
                             const LoopPairKind& kind,
                             const std::function<void(CreateNodeArgs& args)>& makeArgs);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_LoopPair_h
