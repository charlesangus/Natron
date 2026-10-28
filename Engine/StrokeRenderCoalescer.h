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

#ifndef Engine_StrokeRenderCoalescer_h
#define Engine_StrokeRenderCoalescer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <mutex>

NATRON_NAMESPACE_ENTER

/**
 * @brief Lets one live stroke render run at a time. Requests that arrive while it runs collapse into one, to issue when
 * it finishes: that render picks up every point appended since the last rendered one, so the backlog never grows past
 * one render however fast points arrive. Thread-safe.
 **/
class StrokeRenderCoalescer {
public:
    StrokeRenderCoalescer()
        : _mutex()
        , _inFlight(false)
        , _pending(false)
    {
    }

    /**
     * @brief Returns true when the caller may start a render now, which is then in flight until finish(). Otherwise the
     * request is folded into the pending one.
     **/
    bool request()
    {
        std::lock_guard<std::mutex> k(_mutex);
        if (_inFlight) {
            _pending = true;

            return false;
        }
        _inFlight = true;

        return true;
    }

    /**
     * @brief Ends the render in flight. Returns true when requests were folded meanwhile; the caller then requests again.
     **/
    bool finish()
    {
        std::lock_guard<std::mutex> k(_mutex);
        bool pending = _pending;
        _inFlight = false;
        _pending = false;

        return pending;
    }

    bool isInFlight() const
    {
        std::lock_guard<std::mutex> k(_mutex);

        return _inFlight;
    }

private:
    mutable std::mutex _mutex;
    bool _inFlight;
    bool _pending;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_StrokeRenderCoalescer_h
