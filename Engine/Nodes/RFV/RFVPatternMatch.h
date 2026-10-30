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

#ifndef Engine_Nodes_RFV_RFVPatternMatch_h
#define Engine_Nodes_RFV_RFVPatternMatch_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace RFV {

/**
 * @brief The prefix that makes a pattern line a regular expression rather than a glob.
 **/
extern const char* const kPatternRegexPrefix;

/**
 * @brief The lines of a multi-line pattern text, split at '\n', each stripped of one trailing
 * '\r'. A text ending in '\n' ends with an empty line; an empty text is one empty line.
 **/
std::vector<std::string> splitPatternLines(const std::string& text);

/**
 * @brief Whether the glob matches the whole value: '*' matches any run of characters, '?' any
 * one, "[abc]" one of those, "[!abc]" one not among them, and "a-z" inside brackets a range.
 * A ']' first in a class is a member, and a '[' that no ']' closes is a literal. Characters are
 * UTF-8 code points; there is no escape character.
 **/
bool matchesGlob(const std::string& value, const std::string& glob);

/**
 * @brief The index of the first line matching the whole value, or -1. A line starting with
 * kPatternRegexPrefix is an ECMAScript regular expression after the prefix; any other line is a
 * glob (see matchesGlob()). An empty line never matches but keeps its index. A regular
 * expression that does not compile never matches; error, when given, receives the first such
 * line's input, as "Input <index>: ", and why, or is cleared when every regular expression compiles. Every
 * line is checked for errors, those after the matching one included.
 **/
int firstMatch(const std::string& value, const std::vector<std::string>& lines, std::string* error = nullptr);

} // namespace RFV

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_RFV_RFVPatternMatch_h
