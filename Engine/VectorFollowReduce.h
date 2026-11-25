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

#ifndef Engine_VectorFollowReduce_h
#define Engine_VectorFollowReduce_h

#include "Global/Macros.h"

#include <functional>
#include <set>
#include <vector>

#include "Engine/VectorFollow.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Keys over time that evaluate to a set of points, whatever the keys interpolate through.
 * Reduction removes and restores keys on it as it tries each one.
 **/
class VectorFollowKeyedMotion {
public:
    virtual ~VectorFollowKeyedMotion() { }

    virtual std::set<int> keys() const = 0;
    virtual void removeKey(int frame) = 0;
    virtual void restoreKey(int frame) = 0;

    // The same number of points at every frame.
    virtual void evaluate(int frame, std::vector<FollowPoint>* points) const = 0;
};

/**
 * @brief Removes keys greedily, always the one whose removal moves any point the least over any
 * frame between the first and last key, for as long as that stays within tolerance of the motion
 * as it was given. Only keys in removable go, and one key always stays. Returns the removed keys,
 * which are removed from motion too. An abort stops it where it is: what was removed so far stands.
 **/
std::set<int> reduceVectorFollowKeys(VectorFollowKeyedMotion& motion,
                                     const std::set<int>& removable,
                                     double tolerance,
                                     const std::function<bool()>& aborted);

NATRON_NAMESPACE_EXIT

#endif // Engine_VectorFollowReduce_h
