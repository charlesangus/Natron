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

#ifndef NATRON_GUI_SCENEPRESENTPACER_H
#define NATRON_GUI_SCENEPRESENTPACER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <optional>
#include <utility>

NATRON_NAMESPACE_ENTER

/**
 * @brief Which of two render targets a viewer renders into and which it shows, with at most one
 * frame on the GPU, so that showing a frame never waits for the GPU to finish it.
 *
 * A frame is rendered into backTarget() and submitted with a fence; poll() asks whether the GPU
 * has finished it without waiting, and once it has, that target is the front one and takeReady()
 * hands the frame out to be shown. The other target is then the one rendered into next, so the
 * image being shown is never drawn over. A frame submitted while another is still on the GPU
 * replaces it, the newest winning; every fence is released exactly once, through release.
 *
 * Fences are opaque here: testFence says, without waiting, whether one has signalled, and release
 * lets go of one; what either needs current is up to the caller. One thread at a time.
 **/
template <typename Frame>
class ScenePresentPacer {
public:
    using Fence = void*;

    ScenePresentPacer(std::function<bool(Fence)> testFence,
                      std::function<void(Fence)> release)
        : _testFence(std::move(testFence))
        , _release(std::move(release))
    {
    }

    ScenePresentPacer(const ScenePresentPacer&) = delete;
    ScenePresentPacer& operator=(const ScenePresentPacer&) = delete;

    // The fence of a frame on the GPU is not released here: no GL context need be current.
    ~ScenePresentPacer() = default;

    int backTarget() const
    {
        return (_front == 0) ? 1 : 0;
    }

    // -1 before any frame finished.
    int frontTarget() const
    {
        return _front;
    }

    bool inFlight() const
    {
        return bool(_inFlight);
    }

    // A frame is on the GPU or finished and not yet shown.
    bool pending() const
    {
        return bool(_inFlight) || bool(_ready);
    }

    // frame was rendered into backTarget(); fence signals when the GPU has finished it.
    void submitted(Fence fence,
                   Frame frame)
    {
        if (_inFlight && _inFlight->fence) {
            _release(_inFlight->fence);
        }
        _inFlight = InFlight { fence, backTarget(), std::move(frame) };
        _owed = false;
    }

    // True if a finished frame waits to be shown.
    bool poll()
    {
        if (_inFlight && (!_inFlight->fence || _testFence(_inFlight->fence))) {
            if (_inFlight->fence) {
                _release(_inFlight->fence);
            }
            _front = _inFlight->target;
            _ready = std::move(_inFlight->frame);
            _inFlight.reset();
        }

        return bool(_ready);
    }

    // The finished frame, in frontTarget(), once.
    std::optional<Frame> takeReady()
    {
        std::optional<Frame> ready = std::move(_ready);

        _ready.reset();

        return ready;
    }

    // Something asked for a frame the one on the GPU may not show.
    void markOwed()
    {
        _owed = true;
    }

    bool takeOwed()
    {
        return std::exchange(_owed, false);
    }

    // Forgets every frame, as when the targets go.
    void clear()
    {
        if (_inFlight && _inFlight->fence) {
            _release(_inFlight->fence);
        }
        _inFlight.reset();
        _ready.reset();
        _front = -1;
        _owed = false;
    }

private:
    struct InFlight {
        Fence fence;
        int target;
        Frame frame;
    };

    std::function<bool(Fence)> _testFence;
    std::function<void(Fence)> _release;
    std::optional<InFlight> _inFlight;
    std::optional<Frame> _ready;
    int _front = -1;
    bool _owed = false;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEPRESENTPACER_H
