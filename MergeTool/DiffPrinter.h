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

#ifndef MergeTool_DiffPrinter_h
#define MergeTool_DiffPrinter_h

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Serialization/Ops/GraphOp.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER
namespace MergeTool {

/**
 * @brief The operations that turn before into after, as text grouped per node: a "+" line for a
 * node after adds, "-" for one it removes and "~" for one it changes, then one indented line per
 * change, such as "  white: 1 → 1.2" or "  input Source: Read1 → Blur2". Nodes are named by their
 * path of script names; project knobs and settings are listed under "~ Project". Empty when ops is.
 **/
std::string printDiff(const Serialization::ProjectSerialization& before,
                      const Serialization::ProjectSerialization& after,
                      const std::vector<Serialization::GraphOp>& ops);

/**
 * @brief A knob's state in a few words: its value, "(x, y)" for dimensions that differ, an
 * expression, a link or "animated (N keys)". A null knob is at its default, which other, the same
 * knob on another side, tells when it has one.
 **/
std::string knobText(const Serialization::KnobSerialization* knob,
                     const Serialization::KnobSerialization* other = nullptr);

} // namespace MergeTool
NATRON_NAMESPACE_EXIT

#endif // MergeTool_DiffPrinter_h
