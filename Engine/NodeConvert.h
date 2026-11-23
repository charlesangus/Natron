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

#ifndef Engine_NodeConvert_h
#define Engine_NodeConvert_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/UndoCommand.h"

#define kNodeConvertLoopIterations 8

NATRON_NAMESPACE_ENTER

struct IVariant;

/**
 * @brief The registry entry whose base node is node's plug-in, when that entry's I-variant is
 * installed; else null.
 **/
const IVariant* exactIVariantOf(const NodePtr& node) WARN_UNUSED_RETURN;

/**
 * @brief The registry entry whose I-variant is node's plug-in, when that entry's base node is
 * installed; else null.
 **/
const IVariant* baseVariantOf(const NodePtr& node) WARN_UNUSED_RETURN;

/**
 * @brief Whether node has an input labelled label with something connected to it.
 **/
bool hasConnectedInput(const NodePtr& node, const std::string& label) WARN_UNUSED_RETURN;

/**
 * @brief Replaces a node by the node of another plug-in, keeping its name, label, position,
 * connections, and the values, animation, expressions, drivers and links of its knobs, through
 * the registry's knob map and otherwise by name. Expressions and links that read the node read
 * its replacement. Inputs the replacement has no input of the same label for are dropped, as are
 * knobs it has no knob of the same type for.
 *
 * Nothing changes until the first redo(), which creates the replacement; later redo() and undo()
 * swap the two nodes, the one out of the graph kept deactivated for the other direction. Main
 * thread only.
 **/
class NodeConvertCommand
    : public UndoCommand {
public:
    /**
     * @brief Converts node to its exact I-variant, or null with error set when it has none.
     **/
    static std::shared_ptr<NodeConvertCommand> toIVariant(const NodePtr& node, std::string* error) WARN_UNUSED_RETURN;

    /**
     * @brief Converts an I-variant back to its base node, or null with error set when node is no
     * I-variant. The Map input, which the base node lacks, is dropped.
     **/
    static std::shared_ptr<NodeConvertCommand> toBaseNode(const NodePtr& node, std::string* error) WARN_UNUSED_RETURN;

    virtual ~NodeConvertCommand();

    virtual void redo() OVERRIDE FINAL;

    virtual void undo() OVERRIDE FINAL;

    NodePtr getOriginal() const WARN_UNUSED_RETURN;

    /**
     * @brief The replacement, null before the first redo() or if it could not be created.
     **/
    NodePtr getConverted() const WARN_UNUSED_RETURN;

    NodeConvertCommand(const NodePtr& node,
                       const IVariant& variant,
                       bool toIVariant);

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

/**
 * @brief Wraps nodes in a loop block: a LoopBegin on each node outside the selection that feeds
 * it, and a LoopEnd after the selection's single output, taking over that output's connections.
 * The LoopEnd is in Map Blend mode with kNodeConvertLoopIterations iterations and drives nothing.
 * Nothing changes until the first redo(), which creates the loop nodes; undo() takes them out
 * and puts the connections back. Main thread only.
 **/
class LoopWrapCommand
    : public UndoCommand {
public:
    /**
     * @brief Whether nodes can be wrapped; reason, when given, receives why not.
     **/
    static bool canWrap(const NodesList& nodes, std::string* reason) WARN_UNUSED_RETURN;

    /**
     * @brief The wrap of nodes, or null with error set when canWrap() refuses them.
     **/
    static std::shared_ptr<LoopWrapCommand> create(const NodesList& nodes, std::string* error) WARN_UNUSED_RETURN;

    /**
     * @brief A warning for nodes a loop renders poorly: those that pass a transform downstream for
     * concatenation, which a loop breaks, and Defocus, whose bokeh blends badly. Names their exact
     * I-variant when one exists. Empty when none applies.
     **/
    static std::string warningFor(const NodesList& nodes) WARN_UNUSED_RETURN;

    virtual ~LoopWrapCommand();

    virtual void redo() OVERRIDE FINAL;

    virtual void undo() OVERRIDE FINAL;

    /**
     * @brief The loop nodes, null and empty before the first redo().
     **/
    NodePtr getLoopEnd() const WARN_UNUSED_RETURN;
    NodesList getLoopBegins() const WARN_UNUSED_RETURN;

    explicit LoopWrapCommand(const NodesList& nodes);

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_NodeConvert_h
