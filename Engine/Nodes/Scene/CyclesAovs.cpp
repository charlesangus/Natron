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

#include "CyclesAovs.h"

#include <limits>

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>

#include <tbb/task_arena.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace CyclesAovs {
bool
isCycles(const std::string& renderer)
{
    return renderer == kRenderDelegateCyclesId;
}

void
configureEngine(UsdImagingGLEngine* engine)
{
    engine->SetRendererSetting(TfToken("stageMetersPerUnit"), VtValue(1.0));
}

float
depthToDistance(float value)
{
    if (value >= 1e10f) {
        return std::numeric_limits<float>::infinity();
    }

    return value;
}

int
instanceIndex(std::int32_t value)
{
    return value + 1;
}

void
withThreadBudget(int threads,
                 const std::function<void()>& job)
{
    if (threads <= 0) {
        job();

        return;
    }
    tbb::task_arena arena(threads);
    arena.execute(job);
}
} // namespace CyclesAovs

NATRON_NAMESPACE_EXIT
