/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef Engine_RotoKeyframeReduce_h
#define Engine_RotoKeyframeReduce_h

#include "Global/Macros.h"

#include <list>
#include <set>
#include <vector>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

constexpr const char* kRotoReduceKeyframesActionName = "Reduce Keyframes";

struct RotoReducedItem {
    RotoDrawableItemPtr item;
    std::set<int> removed;
};

/**
 * @brief Removes the keys of each item, on whole frames from first to last, that the shape can do
 * without: one at a time, always the one whose removal moves the shape least where it shows, for as
 * long as no point of it moves further than tolerance from where it showed on any frame between its
 * first and last key. The keys of its points and of its own transform both count, and a frame's key
 * goes from all of them at once. Main thread only, and it needs no vectors.
 *
 * Returns, by item, the frames whose keys went; items that lost none are left out. One undo command
 * takes them all back: handed over through undo when given, otherwise pushed on the context's node.
 * With nothing removed there is no command.
 **/
std::vector<RotoReducedItem> reduceRotoKeyframes(const RotoCurveContextPtr& context,
                                                 const std::list<RotoDrawableItemPtr>& items,
                                                 int first,
                                                 int last,
                                                 double tolerance,
                                                 UndoCommandPtr* undo = 0);

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoKeyframeReduce_h
