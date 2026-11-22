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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ViewerStaleCueTiming.h"

#include <algorithm>

NATRON_NAMESPACE_ENTER

ViewerStaleCueTiming::ViewerStaleCueTiming(std::int64_t delayMs,
                                           std::int64_t holdMs)
    : _delayMs(std::max<std::int64_t>(0, delayMs))
    , _holdMs(std::max<std::int64_t>(0, holdMs))
{
}

void
ViewerStaleCueTiming::setDelayMs(std::int64_t delayMs)
{
    _delayMs = std::max<std::int64_t>(0, delayMs);
}

void
ViewerStaleCueTiming::setHoldMs(std::int64_t holdMs)
{
    _holdMs = std::max<std::int64_t>(0, holdMs);
}

void
ViewerStaleCueTiming::setStale(bool stale,
                               std::int64_t nowMs)
{
    if (stale && !_stale) {
        _staleSinceMs = nowMs;
    }
    _stale = stale;
    advance(nowMs);
}

void
ViewerStaleCueTiming::setPlayback(bool playing,
                                  std::int64_t nowMs)
{
    if (playing) {
        _playback = true;
        _visible = false;

        return;
    }
    if (_playback) {
        _playback = false;
        _staleSinceMs = nowMs;
    }
    advance(nowMs);
}

void
ViewerStaleCueTiming::advance(std::int64_t nowMs)
{
    if (_playback) {
        _visible = false;

        return;
    }
    if (!_visible) {
        if (_stale && (nowMs - _staleSinceMs >= _delayMs)) {
            _visible = true;
            _shownAtMs = nowMs;
        }

        return;
    }
    if (!_stale && (nowMs - _shownAtMs >= _holdMs)) {
        _visible = false;
    }
}

std::optional<std::int64_t>
ViewerStaleCueTiming::nextDeadlineMs() const
{
    if (_playback) {
        return std::nullopt;
    }
    if (!_visible && _stale) {
        return _staleSinceMs + _delayMs;
    }
    if (_visible && !_stale) {
        return _shownAtMs + _holdMs;
    }

    return std::nullopt;
}

NATRON_NAMESPACE_EXIT
