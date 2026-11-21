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

#ifndef Serialization_Merge_MergeScope_h
#define Serialization_Merge_MergeScope_h

#include "Global/Macros.h"

#include <set>
#include <string>

#include "Serialization/Merge/NodeMatcher.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The IDs of the shot nodes a merge of one template import may touch: the nodes that stand
 * for a base node, the nodes that lie on a path from one of those to another, and every node inside
 * an in-scope group. baseToMine comes from matchNodes for that import.
 **/
std::set<std::string> mergeScope(const MergeGraph& mine,
                                 const NodeCorrespondence& baseToMine);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_MergeScope_h
