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

#ifndef Gui_NodeGraphDeleteLinks_h
#define Gui_NodeGraphDeleteLinks_h

#include "Global/Macros.h"

#include <cstddef>
#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QString>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

NATRON_NAMESPACE_ENTER

// A link or expression on a node that stays, reading a knob of a node being deleted.
struct BrokenLink {
    std::string masterNode; // fully qualified name of the node being deleted
    std::string masterKnob; // script name of the knob read
    std::string slaveNode; // fully qualified name of the node that stays
    std::string slaveKnob; // script name of the knob that reads it
};

// Number of distinct links, ignoring repeats of the same master and slave knob pair.
std::size_t countDistinctBrokenLinks(const std::vector<BrokenLink>& links);

// The text of the single confirmation shown for a whole delete: links grouped under the deleted
// node that owns them, at most maxShown link lines, then a line counting the rest. Empty when
// there are no links, which means no prompt.
QString brokenLinksPrompt(const std::vector<BrokenLink>& links,
                          std::size_t maxShown = 12);

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphDeleteLinks_h
