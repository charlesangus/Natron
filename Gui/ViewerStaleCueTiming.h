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

#ifndef Gui_ViewerStaleCueTiming_h
#define Gui_ViewerStaleCueTiming_h

#include "Global/Macros.h"

#include <cstdint>
#include <optional>

NATRON_NAMESPACE_ENTER

// Decides when a viewer's stale cue is visible. It shows once the image has been stale for the
// delay outside playback, then stays up until the image is current and the hold has passed since
// it showed. Playback hides it and restarts the delay. Times are in milliseconds on any monotonic
// clock; the caller calls advance() at nextDeadlineMs().
class ViewerStaleCueTiming {
public:
    ViewerStaleCueTiming(std::int64_t delayMs = 0,
                         std::int64_t holdMs = 0);

    void setDelayMs(std::int64_t delayMs);
    void setHoldMs(std::int64_t holdMs);
    std::int64_t delayMs() const { return _delayMs; }
    std::int64_t holdMs() const { return _holdMs; }

    void setStale(bool stale,
                  std::int64_t nowMs);
    void setPlayback(bool playing,
                     std::int64_t nowMs);
    void advance(std::int64_t nowMs);

    bool visible() const { return _visible; }
    bool stale() const { return _stale; }
    bool playback() const { return _playback; }

    // When the visibility can next change with no new input, or none when only an input can change it.
    std::optional<std::int64_t> nextDeadlineMs() const;

private:
    std::int64_t _delayMs;
    std::int64_t _holdMs;
    bool _stale = false;
    bool _playback = false;
    bool _visible = false;
    std::int64_t _staleSinceMs = 0;
    std::int64_t _shownAtMs = 0;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ViewerStaleCueTiming_h
