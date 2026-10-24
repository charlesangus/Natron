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

#ifndef NATRON_TESTS_NATIVEGPUPARITY_H
#define NATRON_TESTS_NATIVEGPUPARITY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "NativeParity.h"

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectI.h"
#include "Engine/RenderScheduler.h"

namespace gpu {
class GpuRuntime;
}

NATRON_NAMESPACE_ENTER

// How a view sits in its device buffer. The view starts `padFloats` floats into the buffer and its
// rows are `rowPadFloats` floats longer than the pixels in them. The margins grow an output view past
// the rendered window, in pixels on each side, and are ignored for inputs, whose view is exactly
// their image.
struct GpuViewLayout {
    int padFloats = 0;
    int rowPadFloats = 0;
    int marginLeft = 0;
    int marginBottom = 0;
    int marginRight = 0;
    int marginTop = 0;
};

// An input of runRenderGpu(): the pixels of `bounds`, packed row by row from the bottom row. An image
// with no pixels stands for an input that is not connected. `bounds` need not cover the window the
// effect renders; reads past it are the effect's to wrap.
struct GpuParityImage {
    RectI bounds;
    int nComps = 4;
    std::vector<float> pixels;
    GpuViewLayout layout;
};

// An extra layer of input `inputNb` for runRenderGpu(), read in addition to the input's own image.
struct GpuParityLayerImage {
    int inputNb = 0;
    ImageLayerDesc layer;
    GpuParityImage image;
};

struct GpuRunResult {
    bool ok = false;

    // The rendered window, nComps floats per pixel, packed from the bottom row.
    std::vector<float> pixels;

    // Floats of the output buffer outside the window, padding included, that differ from what the
    // buffer held before the render.
    std::size_t writtenOutside = 0;
};

// The first mismatch and the worst error of compareImages().
struct ImageDiff {
    std::size_t compared = 0;
    std::size_t mismatches = 0;
    double worstAbsolute = 0.;
    std::size_t worstIndex = 0;
    std::size_t firstMismatchIndex = 0;
};

// Compares two packed images float by float with the tolerance. The indices are float indices, so
// the pixel is index / nComps and the channel index % nComps. The images must be the same size.
ImageDiff compareImages(const std::vector<float>& reference,
                        const std::vector<float>& got,
                        const ParityTolerance& tolerance);

// Reports that there is no GPU to run on: a skip notice, or a test failure under
// NATRON_TEST_REQUIRE_GPU=1. The bundled gtest has no GTEST_SKIP, so the caller returns after it.
void reportNoGpu(const std::string& reason);

#ifdef NATRON_HAVE_GPU

// A Vulkan runtime and the device buffers the harness reuses between renders.
class GpuParityRuntime;

// Creates a runtime on whatever Vulkan device exists, software ones included, since the
// application's context refuses CPU devices outside Force mode. Returns false after reportNoGpu()
// when there is none: the test must return.
bool createRuntime(std::unique_ptr<GpuParityRuntime>& runtime);

class GpuParityRuntime {
public:
    ~GpuParityRuntime();

    gpu::GpuRuntime& runtime();

    // A device buffer of at least `floats` floats, kept under `slot` until the next call for the same
    // slot asks for more. Null after a failed test expectation.
    gpu::GpuBuffer* buffer(int slot,
                           std::size_t floats);

private:
    friend bool createRuntime(std::unique_ptr<GpuParityRuntime>& runtime);

    struct Impl;
    GpuParityRuntime();
    std::unique_ptr<Impl> _impl;
};

// Uploads the images, records effect.renderGpu() over outputRect into output views laid out as
// `layout` says, submits, waits and downloads. `query.nComps` is the output's component count and
// `query.plane` the plane rendered, taken as RGBA or Alpha by nComps when left unset. The mask, when
// given, goes to the effect's mask input, which only a StagedImageEffect has. `layers` are uploaded
// into GpuRenderArgs::layerInputs. The test fails when the effect does not accept the query or does
// not render.
GpuRunResult runRenderGpu(GpuParityRuntime& runtime,
                          NativeEffectBase& effect,
                          const GpuQuery& query,
                          const std::vector<GpuParityImage>& inputs,
                          const GpuParityImage* mask,
                          const RectI& outputRect,
                          const GpuViewLayout& layout = GpuViewLayout(),
                          const std::vector<GpuParityLayerImage>& layers = std::vector<GpuParityLayerImage>());

#endif // NATRON_HAVE_GPU

// A frame rendered through the scheduler on the GPU and on the CPU. The pixels are RGBA over
// `window`, packed from the bottom row.
struct EndToEndResult {
    bool ok = false;

    // Set when no GPU is available; reportNoGpu() has then reported the skip and nothing is rendered.
    bool skipped = false;
    std::string error;

    RectI window;
    std::vector<float> gpu;
    std::vector<float> cpu;

    // The regions the GPU frame was placed in, and the nodes of each one's members.
    std::vector<FrameGraph::Region> regions;
    std::vector<std::vector<NodePtr>> regionNodes;

    // Tasks the GPU frame ran on the device, by node script name.
    std::map<std::string, int> gpuTasksPerNode;

    bool ranInRegion(const NodePtr& node) const;
};

// Called once the forced GPU context is installed and before the GPU frame is planned, so a test can
// register kernels on its runtime (appPTR->getGpuContext()->runtime()). Returning false fails the run
// with `error`.
typedef std::function<bool(std::string* error)> GpuContextReady;

// Renders `root` over `window` at `time` twice. First as a frame of the render scheduler with a
// forced GPU context, with every node of `graph` whose effect accepts the task on the GPU, as
// regions. Then on the CPU under a GpuPlacementOffGuard.
EndToEndResult renderEndToEnd(const std::vector<NodePtr>& graph,
                              const NodePtr& root,
                              const RectI& window,
                              double time = 1.,
                              const GpuContextReady& onGpuContext = GpuContextReady());

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVEGPUPARITY_H
