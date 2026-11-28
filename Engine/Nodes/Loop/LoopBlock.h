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

#ifndef Engine_Nodes_Loop_LoopBlock_h
#define Engine_Nodes_Loop_LoopBlock_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Loop/LoopEndCooks.h"

#define kLoopBlockDefaultName "loop1"
#define kLoopBlockParamDrivenKnobs "drivenKnobs"

NATRON_NAMESPACE_ENTER

/**
 * @brief The end node of a loop block: an image loop, a geometry for-each or a geometry solver.
 * The node graph draws a hull around each block, knobs of its body can be driven from its
 * variables, and its driven-knob table lists them, whatever the kind of block.
 **/
class LoopBlockEnd {
public:
    virtual ~LoopBlockEnd();

    /** @brief The block of node's effect, or null when node does not end one. */
    static LoopBlockEnd* of(const NodePtr& node) WARN_UNUSED_RETURN;

    /** @brief The name of the loops.<name>.* variables the block sets; empty when it sets none. */
    virtual std::string getBlockLoopName() const = 0;

    /** @brief The begin nodes the block pairs with, by script name. */
    virtual std::vector<NodePtr> getBlockBegins() const = 0;

    /** @brief The knob holding the block's name. */
    virtual KnobStringPtr getBlockNameKnob() const = 0;

    /** @brief The variables each iteration sets as loop.* and loops.<name>.*. */
    virtual const std::vector<std::string>& getBlockVariables() const = 0;

    /**
     * @brief base with the variables of the iteration the block previews at time, as knobs of
     * its body show outside a render. Never cooks anything.
     **/
    virtual RenderContextPtr getBlockPreviewContext(const RenderContextPtr& base,
                                                    double time) const
        = 0;
};

namespace LoopBlocks {
/** @brief Every kind of loop block, begin and end. */
const std::vector<LoopEndKind>& kinds() WARN_UNUSED_RETURN;

bool isBegin(const NodePtr& node) WARN_UNUSED_RETURN;

bool isEnd(const NodePtr& node) WARN_UNUSED_RETURN;

/**
 * @brief The block ends whose iterations cook node, of every kind: those its outputs reach
 * without passing a nested block, nearest first, then those enclosing them in turn. Empty
 * outside any block.
 **/
std::vector<NodePtr> enclosingEnds(const NodePtr& node) WARN_UNUSED_RETURN;

/** @brief The nodes of end's group whose iterations end cooks, nested blocks' included. */
std::vector<NodePtr> bodyNodes(const NodePtr& end) WARN_UNUSED_RETURN;

/**
 * @brief The context node's knobs are previewed under at time: the preview iteration of every
 * block enclosing it, outermost first. Null outside any block.
 **/
RenderContextPtr previewContextOf(const NodePtr& node,
                                  double time) WARN_UNUSED_RETURN;

/**
 * @brief The loop variables any kind of block sets: what loop, or loops.<name> used whole,
 * may read.
 **/
const std::vector<std::string>& allVariables() WARN_UNUSED_RETURN;

/**
 * @brief current when no other block end in end's group is named so, else the first free
 * loop<N>. Block names share the loops.<name>.* variables, so they are unique across kinds.
 **/
std::string freeName(const NodePtr& end,
                     const std::string& current) WARN_UNUSED_RETURN;

/**
 * @brief Gives a new or pasted block end a name no other block end of its group has; a loaded
 * one keeps its saved name.
 **/
void assignFreeName(const EffectInstance* end);
} // namespace LoopBlocks

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_LoopBlock_h
