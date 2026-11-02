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

#ifndef Gui_NodeGraphSplice_h
#define Gui_NodeGraphSplice_h

#include "Global/Macros.h"

#include <cstdint>
#include <vector>

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

typedef std::intptr_t SpliceNodeId;

// Free (unconnected) input and output slots are -1; any other value is the
// SpliceNodeId of the node occupying that slot, whether or not that node is
// part of the selection being planned. inputIsMask runs parallel to inputIds;
// an input past its end is not a mask.
struct SpliceNodeInfo {
    SpliceNodeId id;
    std::vector<SpliceNodeId> inputIds;
    std::vector<bool> inputIsMask;
    int preferredInput;
    std::vector<SpliceNodeId> outputIds;
    bool isBackdrop;
    bool hasNoInputs;
    bool isOutputNode;
    bool isInputNode;

    SpliceNodeInfo()
        : id(-1)
        , inputIds()
        , inputIsMask()
        , preferredInput(-1)
        , outputIds()
        , isBackdrop(false)
        , hasNoInputs(false)
        , isOutputNode(false)
        , isInputNode(false)
    {
    }
};

// entryInputs lists the entry's inputs the pipe's source may go into, best first; the caller
// takes the first one the source can connect to. entryInput is the first of them.
struct SplicePlan {
    bool valid;
    SpliceNodeId entryId;
    int entryInput;
    std::vector<int> entryInputs;
    SpliceNodeId exitId;

    SplicePlan()
        : valid(false)
        , entryId(-1)
        , entryInput(-1)
        , entryInputs()
        , exitId(-1)
    {
    }
};

// The entry is the one node whose preferred input is free, entered through that input. When no
// node has a free preferred input, the entry is the one node whose preferred input is fed from
// outside the selection, entered through any of its free inputs, masks last, so that a node
// already connected keeps its connections.
SplicePlan planSplice(const std::vector<SpliceNodeInfo>& selection);

SplicePlan planSpliceForSelection(const NodesGuiList& selection);

struct PasteSplicePlan {
    bool valid;
    SpliceNodeId targetId;
    SplicePlan splice;

    PasteSplicePlan()
        : valid(false)
        , targetId(-1)
        , splice()
    {
    }
};

// The target is the one non-backdrop node of the selection; the pasted set is
// spliced below it, so the target must be able to feed it and the entry must
// accept an input rather than feed the target.
PasteSplicePlan planPasteSplice(const std::vector<SpliceNodeInfo>& selection,
                                const std::vector<SpliceNodeInfo>& pasted);

PasteSplicePlan planPasteSpliceForSelection(const NodesGuiList& selection,
                                            const NodesGuiList& pasted);

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphSplice_h
