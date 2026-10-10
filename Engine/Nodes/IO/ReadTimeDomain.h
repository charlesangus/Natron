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

#ifndef Engine_Nodes_IO_ReadTimeDomain_h
#define Engine_Nodes_IO_ReadTimeDomain_h

#include <set>
#include <string>

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

namespace ReadTimeDomain {
// Integer values equal the option indices of the matching choice knobs.
enum BeforeAfter {
    eBeforeAfterHold = 0,
    eBeforeAfterLoop,
    eBeforeAfterBounce,
    eBeforeAfterBlack,
    eBeforeAfterError,
};

enum MissingFrame {
    eMissingPrevious = 0,
    eMissingNext,
    eMissingNearest,
    eMissingError,
    eMissingBlack,
};

enum FrameMode {
    eFrameModeStartingTime = 0,
    eFrameModeTimeOffset,
};

// How far from the requested frame a missing-frame search looks, in each direction.
constexpr int kMissingFrameSearchRange = 100;

struct Settings {
    int firstFrame = 1;
    int lastFrame = 1;
    BeforeAfter before = eBeforeAfterHold;
    BeforeAfter after = eBeforeAfterHold;
    MissingFrame onMissingFrame = eMissingError;
    FrameMode frameMode = eFrameModeStartingTime;
    int startingTime = 1;
    int timeOffset = 0;
};

struct Result {
    enum Kind {
        eFile,
        eBlack,
        eError,
    };

    Kind kind = eBlack;
    int frame = 0; // valid when kind == eFile
    std::string message; // set when kind == eError
};

struct FrameRange {
    int min = 0;
    int max = 0;
};

/**
 * @brief The output frame at which firstFrame appears, minus firstFrame. Starting-time mode
 * derives it from startingTime; time-offset mode uses timeOffset as is.
 **/
int effectiveTimeOffset(const Settings& settings);

/**
 * @brief The output frames the node produces: [firstFrame, lastFrame] shifted to start at
 * the effective starting time.
 **/
FrameRange outputRange(const Settings& settings);

/**
 * @brief What to load at output `time`. `framesOnDisk` holds the file frame numbers that exist.
 **/
Result resolve(double time, const Settings& settings, const std::set<int>& framesOnDisk);
} // namespace ReadTimeDomain

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_ReadTimeDomain_h
