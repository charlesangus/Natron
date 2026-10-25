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

#ifndef Gui_NodeClipBoard_h
#define Gui_NodeClipBoard_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

class NodeClipBoard
{
public:
    std::list<Serialization::NodeSerializationPtr> nodes;

    // The node each item of nodes was copied from, or null. It is not put on the system
    // clipboard: a serialization does not reference its node.
    std::list<NodePtr> sourceNodes;

    // The Project::getSessionId() of the project the nodes were copied from, or empty if unknown.
    std::string sourceProject;

    NodeClipBoard()
        : nodes()
        , sourceNodes()
        , sourceProject()
    {
    }

    bool isEmpty() const
    {
        return nodes.empty();
    }
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeClipBoard_h
