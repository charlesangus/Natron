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

#include "Global/Enums.h"
#include "Global/Macros.h"

#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
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
    float maxAbsDiff = 0.f;
    std::string file;
    std::string error;
};

// Renders with GPU placement off, as the GPU mode setting set to Off would, while it lives. The
// both-ways comparisons below hold one so that they compare the two schedulers on the CPU: with a
// device, the Task graph pass could otherwise place nodes on the GPU, whose results may differ from
// the CPU's by a rounding step. No render may be running when it is created or destroyed.
class GpuPlacementOffGuard {
public:
    GpuPlacementOffGuard();
    ~GpuPlacementOffGuard();

    GpuPlacementOffGuard(const GpuPlacementOffGuard&) = delete;
    GpuPlacementOffGuard& operator=(const GpuPlacementOffGuard&) = delete;

private:
    std::unique_ptr<GpuContext> _saved;
};

// Renders frames [firstFrame, lastFrame] through `writer` (a native Write node) once in Legacy
// mode, then once in Task graph mode per entry of `poolSizes` with the global thread pool capped
// at that size, and compares every output EXR of every project view bit for bit against the
// Legacy one. The writer's output file, bitDepth and compression knobs are overwritten so its
// output lands in a temporary directory in the layout FlatExrReader parses, one file per view
// when the project has several; the directory is removed before returning.
// `beforeTaskGraph`, when set, is called once between the Legacy pass and the first Task graph
// pass, so a self-test can make the two renders differ on purpose. A `tolerance` above zero
// replaces the bit-exact comparison with a per-float absolute difference bound.
RenderMismatch renderBothWays(const NodePtr& writer,
                              int firstFrame,
                              int lastFrame,
                              const std::vector<int>& poolSizes,
                              const std::function<void()>& beforeTaskGraph = std::function<void()>(),
                              float tolerance = 0.f);

// Renders `roi` (in pixel coordinates at `mipmapLevel`) of `node`'s RGBA float output once in
// Legacy mode directly through renderRoI, bypassing the cache, then per pool size as a frame
// built and run by the RenderScheduler, and compares the pixels inside `roi` bit for bit. A Task
// graph pass that runs no task through the scheduler is reported as a mismatch. `unplannedPulls`,
// when given, receives each Task graph pass's count of pulls past the frame store, in pool order.
RenderMismatch renderBothWaysDirect(const NodePtr& node,
                                    double time,
                                    ViewIdx view,
                                    unsigned mipmapLevel,
                                    const RectI& roi,
                                    const std::vector<int>& poolSizes,
                                    const std::function<void()>& beforeTaskGraph = std::function<void()>(),
                                    float tolerance = 0.f,
                                    std::vector<int>* unplannedPulls = 0);

std::string describe(const RenderMismatch& m);

// The error renderNodePlanesDirect() and renderNodePlanesInMode() give when the render succeeded
// but produced no plane: an empty output, such as black routed from nothing connected or an empty
// region of definition, rather than a failed render.
extern const char kRenderProducedNoPlane[];

// One plane read back by renderNodePlanesDirect(). `pixels` is row-major over `window` from its
// bottom row, with the values of one pixel interleaved as listed in `channels`.
struct RenderedPlane {
    ImageLayerDesc layer;
    RectI window;
    std::vector<std::string> channels;
    std::vector<float> pixels;
};

// Renders the planes in `layers` of `node` over `roi` (pixel coordinates at `mipmapLevel`) in one
// Legacy-mode renderRoI call, and reads each back as float over the whole of `roi`, zero where the
// render was clipped to the node's region of definition. `out` gets one plane for each entry of
// `layers`, in the same order. The scheduler mode is restored before returning.
// On failure `out` is empty and `error`, when given, says why. With `byPassCache`, the render
// bypasses the cache for the whole tree, not just for `node`: every node upstream drops the entry
// it finds and renders again, and the caches are emptied before and after. A test of cache reuse
// passes false, which also leaves the caches as they are. With `draftMode`, the render is a draft
// one, as the viewer's during a drag.
bool renderNodePlanesDirect(const NodePtr& node,
                            double time,
                            ViewIdx view,
                            unsigned mipmapLevel,
                            const RectI& roi,
                            const std::list<ImageLayerDesc>& layers,
                            std::vector<RenderedPlane>* out,
                            std::string* error = 0,
                            bool byPassCache = true,
                            bool draftMode = false);

// Renders `node` over `roi` the way `mode` renders a frame, and reads every plane back as
// renderNodePlanesDirect() does. Legacy renders the planes in `layers` (RGBA when it is empty) in
// one renderRoI call on this thread; Task graph runs a frame built by the RenderScheduler, which
// renders the planes its request pass plans for the node and ignores `layers`. `out` gets every
// plane rendered: in `layers` order for Legacy, in layer order for Task graph. The scheduler always
// renders through the cache, so there `byPassCache` only empties the caches before and after the
// render. The scheduler mode is restored before returning.
bool renderNodePlanesInMode(const NodePtr& node,
                            double time,
                            ViewIdx view,
                            unsigned mipmapLevel,
                            const RectI& roi,
                            const std::list<ImageLayerDesc>& layers,
                            RenderSchedulerModeEnum mode,
                            bool byPassCache,
                            std::vector<RenderedPlane>* out,
                            std::string* error = 0);

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_RENDERBOTHWAYS_H
