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

#ifndef Engine_Nodes_Raw_RawDevelop_h
#define Engine_Nodes_Raw_RawDevelop_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <vector>

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"

#define kRawDevelopBackdropTitle "Develop RAW"

NATRON_NAMESPACE_ENTER

class CreateNodeArgs;

/**
 * @brief The Develop RAW chain: RawPrepare, RawTemperature, RawHighlights, RawDemosaic,
 * RawInputColor and RawColorCalibration connected one below the other under a Read of a camera raw
 * file, at their defaults, which are darktable's under its default scene-referred workflow
 * (RawTemperature as shot to reference, RawColorCalibration adapting from the file's illuminant),
 * as loose nodes inside a backdrop titled "Develop RAW".
 *
 * The native Read asks for it when it is created in the interface -- from the node menu, by a
 * file dropped on the node graph or a file opened with Natron -- on a camera raw file, with the
 * "Develop RAW files on read" preference on. Loading a project, pasting, Python's createNode() and
 * setting the filename of a Read that already exists never build one.
 **/
namespace RawDevelop {
struct Chain {
    NodePtr prepare;
    NodePtr temperature;
    NodePtr highlights;
    NodePtr demosaic;
    // DenoiseProfiled, only with the developRawDenoise preference on.
    NodePtr denoise;
    NodePtr inputColor;
    NodePtr colorCalibration;
    NodePtr backdrop;

    bool complete() const;
};

struct NodeSize {
    double width = 0.;
    double height = 0.;
};

/**
 * @brief A rectangle of the node graph: its top-left corner, y growing downwards, and its size.
 **/
struct GraphRect {
    double x = 0.;
    double y = 0.;
    double width = 0.;
    double height = 0.;
};

struct ChainLayout {
    std::vector<GraphRect> nodes;
    GraphRect backdrop;
};

/**
 * @brief Where buildChain() puts nodes of the given sizes under a Read at (readX, readY) of the
 * given size: one below the other in order, each centred under the Read, inside a backdrop whose
 * title bar sits above the first.
 **/
ChainLayout layoutChain(double readX, double readY, double readWidth, double readHeight, const std::vector<NodeSize>& sizes);

/**
 * @brief Creates the chain below read, in read's group, and returns it; a chain whose nodes could
 * not all be created or connected is removed again and returned empty. In the interface the chain
 * joins the undo command that created read (NodeGraphI::addNodesToCreationOf()), so undoing the
 * Read's creation removes the chain and its backdrop with it.
 **/
Chain buildChain(const NodePtr& read);

/**
 * @brief Whether args are those of a node the user creates in the interface: not silent (as
 * Python's createNode() is), not from a serialization (a project load, a paste), inside the
 * project and with an interface.
 **/
bool isInteractiveArgs(const CreateNodeArgs& args);

/**
 * @brief isInteractiveArgs(), in an application with an interface, outside a project load and
 * the creation of a PyPlug or a node tree.
 **/
bool isInteractiveCreation(const AppInstancePtr& app,
                           const CreateNodeArgs& args);

/**
 * @brief Called by the native Read once it is created with its filename: builds the chain under
 * it on the next turn of the event loop, once the interface has placed the Read, when the
 * creation is interactive, the file is a camera raw file and the preference is on.
 **/
void onReadCreated(const NodePtr& read,
                   const CreateNodeArgs& args);
} // namespace RawDevelop

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawDevelop_h
