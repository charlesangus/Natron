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

#include "ReadTimeDomain.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

NATRON_NAMESPACE_ENTER

namespace ReadTimeDomain {
namespace {
    enum class Placement {
        eWithin,
        eBlack,
        eError,
    };

    struct Mapped {
        Placement placement;
        double sequenceTime;
    };

    int64_t positiveModulo(int64_t value, int64_t period)
    {
        return (value % period + period) % period;
    }

    // The fractional part of `sequenceTime` is dropped by truncation here, before the later
    // round-to-nearest, so a time just outside the range wraps from its truncated integer.
    double wrapIntoRange(BeforeAfter mode, const Settings& s, double sequenceTime)
    {
        const int64_t seqFrames = static_cast<int64_t>(s.lastFrame) - s.firstFrame + 1;
        if (seqFrames <= 1) {
            return s.firstFrame;
        }
        int64_t offsetFromStart = static_cast<int64_t>(sequenceTime) - s.firstFrame;
        if (mode == eBeforeAfterLoop) {
            offsetFromStart = positiveModulo(offsetFromStart, seqFrames);
        } else {
            const int64_t loopFrames = seqFrames * 2 - 2;
            offsetFromStart = positiveModulo(offsetFromStart, loopFrames);
            if (offsetFromStart >= seqFrames) {
                offsetFromStart = loopFrames - offsetFromStart;
            }
        }
        return static_cast<double>(s.firstFrame + offsetFromStart);
    }

    Mapped mapOutsideRange(BeforeAfter mode, bool beforeStart, const Settings& s, double sequenceTime)
    {
        switch (mode) {
        case eBeforeAfterHold:
            return { Placement::eWithin, static_cast<double>(beforeStart ? s.firstFrame : s.lastFrame) };
        case eBeforeAfterLoop:
        case eBeforeAfterBounce:
            return { Placement::eWithin, wrapIntoRange(mode, s, sequenceTime) };
        case eBeforeAfterBlack:
            return { Placement::eBlack, 0. };
        case eBeforeAfterError:
            break;
        }
        return { Placement::eError, 0. };
    }

    Mapped mapToSequenceTime(double time, const Settings& s)
    {
        const double sequenceTime = time - effectiveTimeOffset(s);
        if (s.firstFrame <= sequenceTime && sequenceTime <= s.lastFrame) {
            return { Placement::eWithin, sequenceTime };
        }
        if (sequenceTime < s.firstFrame) {
            return mapOutsideRange(s.before, true, s, sequenceTime);
        }
        return mapOutsideRange(s.after, false, s, sequenceTime);
    }

    Result error(const std::string& message)
    {
        Result r;
        r.kind = Result::eError;
        r.message = message;
        return r;
    }
} // namespace

int
effectiveTimeOffset(const Settings& settings)
{
    if (settings.frameMode == eFrameModeStartingTime) {
        return settings.startingTime - settings.firstFrame;
    }
    return settings.timeOffset;
}

FrameRange
outputRange(const Settings& settings)
{
    const int start = settings.firstFrame + effectiveTimeOffset(settings);
    FrameRange range;
    range.min = start;
    range.max = start + (settings.lastFrame - settings.firstFrame);
    return range;
}

Result
resolve(double time, const Settings& settings, const std::set<int>& framesOnDisk)
{
    // Keeps the integer casts below defined.
    if (!std::isfinite(time) || std::fabs(time) > 1e9) {
        return error("Time out of range");
    }

    const Mapped mapped = mapToSequenceTime(time, settings);
    if (mapped.placement == Placement::eBlack) {
        return Result();
    }
    if (mapped.placement == Placement::eError) {
        return error("Out of frame range");
    }

    const int frame = static_cast<int>(std::floor(mapped.sequenceTime + 0.5));
    const MissingFrame policy = settings.onMissingFrame;
    const bool searchOtherFrame = policy == eMissingPrevious || policy == eMissingNext || policy == eMissingNearest;

    // The nearest order is 0, +1, -1, +2, -2, ...; frames are probed in 64 bits so that a search
    // from either end of the int range stays defined.
    int offset = 0;
    do {
        const int64_t candidate = static_cast<int64_t>(frame) + offset;
        if (candidate >= std::numeric_limits<int>::min() && candidate <= std::numeric_limits<int>::max() && framesOnDisk.count(static_cast<int>(candidate))) {
            Result r;
            r.kind = Result::eFile;
            r.frame = static_cast<int>(candidate);
            return r;
        }
        if (policy == eMissingPrevious) {
            --offset;
        } else if (policy == eMissingNext) {
            ++offset;
        } else if (policy == eMissingNearest) {
            offset = offset <= 0 ? -offset + 1 : -offset;
        }
    } while (searchOtherFrame && std::abs(offset) <= kMissingFrameSearchRange);

    if (policy == eMissingBlack) {
        return Result();
    }
    return error("Missing frame " + std::to_string(frame));
}
} // namespace ReadTimeDomain

NATRON_NAMESPACE_EXIT
