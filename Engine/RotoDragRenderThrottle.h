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

#ifndef Engine_RotoDragRenderThrottle_h
#define Engine_RotoDragRenderThrottle_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Decides when a shape drag renders. Live drags keep one render in flight: edits made while it runs collapse
 * into one render issued when it ends, which picks up the latest state, so renders never queue behind the pen.
 * Adaptive drags fall back to rendering on release for the rest of the gesture once a render outlasts the threshold.
 * Every gesture starts live again, so a lighter graph or frame gets live renders back on the next drag.
 *
 * Main thread only. Times are in milliseconds on any monotonic clock.
 **/
class RotoDragRenderThrottle {
public:
    enum ModeEnum {
        eModeAdaptive = 0,
        eModeAlwaysLive,
        eModeOnRelease
    };

    enum ActionEnum {
        // Nothing to do now.
        eActionNone = 0,
        // Request a render of the current state.
        eActionRender,
        // The gesture fell back to rendering on release: abort the renders in flight, their frames are stale.
        eActionAbort
    };

    RotoDragRenderThrottle()
        : _mode(eModeOnRelease)
        , _thresholdMs(0.)
        , _open(false)
        , _live(false)
        , _pending(false)
        , _inFlight(0)
        , _inFlightSinceMs(0.)
    {
    }

    void beginGesture(ModeEnum mode,
                      double thresholdMs)
    {
        _mode = mode;
        _thresholdMs = thresholdMs;
        _open = true;
        _live = mode != eModeOnRelease;
        _pending = false;
        _inFlight = 0;
        _inFlightSinceMs = 0.;
    }

    /**
     * @brief Ends the gesture; the caller then renders the final state. Returns eActionAbort when live renders are still
     * in flight, as they show an older state than that final render.
     **/
    ActionEnum endGesture()
    {
        ActionEnum action = (_open && _inFlight > 0) ? eActionAbort : eActionNone;
        _open = false;
        _live = false;
        _pending = false;
        _inFlight = 0;

        return action;
    }

    /**
     * @brief The shape changed at nowMs.
     **/
    ActionEnum edited(double nowMs)
    {
        if (!_open || !_live) {
            return eActionNone;
        }
        if (_inFlight == 0) {
            return eActionRender;
        }
        if ((_mode == eModeAdaptive) && (nowMs - _inFlightSinceMs > _thresholdMs)) {
            fallBack();

            return eActionAbort;
        }
        _pending = true;

        return eActionNone;
    }

    /**
     * @brief A render requested during the gesture started at nowMs. It renders every edit made so far.
     **/
    void renderStarted(double nowMs)
    {
        if (!_open) {
            return;
        }
        if (_inFlight == 0) {
            _inFlightSinceMs = nowMs;
        }
        ++_inFlight;
        _pending = false;
    }

    /**
     * @brief A render requested during the gesture ended after elapsedMs, finished or aborted.
     **/
    ActionEnum renderEnded(double elapsedMs,
                           double nowMs)
    {
        if (!_open || (_inFlight == 0)) {
            return eActionNone;
        }
        --_inFlight;
        if (_inFlight > 0) {
            _inFlightSinceMs = nowMs;
        }
        if (_live && (_mode == eModeAdaptive) && (elapsedMs > _thresholdMs)) {
            fallBack();

            return _inFlight > 0 ? eActionAbort : eActionNone;
        }
        if (_live && _pending && (_inFlight == 0)) {
            return eActionRender;
        }

        return eActionNone;
    }

    bool isGestureOpen() const
    {
        return _open;
    }

    bool isLive() const
    {
        return _open && _live;
    }

    int getRendersInFlight() const
    {
        return _inFlight;
    }

private:
    void fallBack()
    {
        _live = false;
        _pending = false;
    }

    ModeEnum _mode;
    double _thresholdMs;
    bool _open;
    bool _live;
    bool _pending;
    int _inFlight;
    double _inFlightSinceMs;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoDragRenderThrottle_h
