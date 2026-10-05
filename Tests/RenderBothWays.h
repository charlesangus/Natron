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

#ifndef NATRON_TESTS_RENDERBOTHWAYS_H
#define NATRON_TESTS_RENDERBOTHWAYS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

// The first difference between a Legacy render and a Task graph render of the same request.
// `x`/`y` are in the compared image's own pixel coordinates: the EXR data window for
// renderBothWays(), the mipmap-level pixel space for renderBothWaysDirect(). `error` is set
// instead of the pixel fields when a render or a read failed, or when the two results differ
// in size or channel list rather than in a value.
struct RenderMismatch {
    bool any = false;
    int frame = 0;
    int view = 0;
    int x = 0;
    int y = 0;
    std::string channel;
    float legacy = 0.f;
    float taskGraph = 0.f;
    std::string file;
    std::string error;
};

// Renders frames [firstFrame, lastFrame] through `writer` (a WriteOIIO node) once in Legacy
// mode, then once in Task graph mode per entry of `poolSizes` with the global thread pool capped
// at that size, and compares every output EXR of every project view bit for bit against the
// Legacy one. The writer's output file, bitDepth and compression knobs are overwritten so its
// output lands in a temporary directory in the layout FlatExrReader parses, one file per view
// when the project has several; the directory is removed before returning.
// `beforeTaskGraph`, when set, is called once between the Legacy pass and the first Task graph
// pass, so a self-test can make the two renders differ on purpose.
RenderMismatch renderBothWays(const NodePtr& writer,
                              int firstFrame,
                              int lastFrame,
                              const std::vector<int>& poolSizes,
                              const std::function<void()>& beforeTaskGraph = std::function<void()>());

// Renders `roi` (in pixel coordinates at `mipmapLevel`) of `node`'s RGBA float output once in
// Legacy mode directly through renderRoI, bypassing the cache, then per pool size as a frame
// built and run by the RenderScheduler, and compares the pixels inside `roi` bit for bit. A Task
// graph pass that runs no task through the scheduler is reported as a mismatch.
RenderMismatch renderBothWaysDirect(const NodePtr& node,
                                    double time,
                                    ViewIdx view,
                                    unsigned mipmapLevel,
                                    const RectI& roi,
                                    const std::vector<int>& poolSizes,
                                    const std::function<void()>& beforeTaskGraph = std::function<void()>());

std::string describe(const RenderMismatch& m);

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_RENDERBOTHWAYS_H
