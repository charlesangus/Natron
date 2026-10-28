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

#include "ZDefocus.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Filter/Defocus.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
constexpr InputSpec kZDefocusInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "Kernel", .role = InputRole::Aux, .optional = true },
};

constexpr NodeSpec kZDefocusNodeSpec = {
    .id = PLUGINID_NATRON_ZDEFOCUS,
    .label = "ZDefocus",
    .description = "Defocus the input stream by depth, as a lens focused at the Focal Plane does: every pixel spreads into a bokeh whose size its "
                   "Depth sets.\n"
                   "\n"
                   "In Manual mode the Depth Of Field around the Focal Plane is sharp and the blur grows to Size infinitely far behind it; in Lens mode "
                   "a thin lens of the Focal Length, F Stop and Film Back Width sets it. Either way no bokeh exceeds Max Size.\n"
                   "\n"
                   "The depths are cut into Layers slices, each defocused with its own bokeh and laid over those behind it, so that a sharp "
                   "foreground does not bleed into a blurred background. The Focal Plane output shows the pixels in front of the focus in red, "
                   "those in focus in green and those behind it in blue.\n"
                   "\n"
                   "Depth 0 or below, or not a number, is infinitely far.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_ZDEFOCUS,
    .icon = NATRON_IMAGES_PATH "NativeNodes/ZDefocus.png",
    .inputs = kZDefocusInputs,
};

constexpr ImagePipelineSpec kZDefocusPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Passes,
};

const double kMaxSize = 500.;
// A defensive bound on either radius, in pixels, past what the knobs reach at any aspect ratio.
const double kMaxRadius = 2048.;
const int kMaxLayers = 64;
const int kKernelTypeDisc = 0;
const int kKernelTypeBlades = 1;
const int kKernelTypeImage = 2;
const int kOutputFocalPlane = 1;
const int kModeLens = 1;
const int kUnitsInverseDistance = 1;
// A Kernel image larger than this many pixels is read over its image only.
const std::size_t kMaxKernelImagePixels = (std::size_t)1 << 24;
// The focal plane output's classes, which are also the channels it marks.
const uint16_t kClassFront = 0;
const uint16_t kClassInFocus = 1;
const uint16_t kClassBehind = 2;

// Millimetres per scene unit, in the order of the Scene Unit options.
const double kSceneUnitMillimetres[] = { 1., 10., 1000., 25.4, 304.8 };
const int kSceneUnitMetres = 2;

BokehShape
shapeOf(int kernelType)
{
    switch (kernelType) {
    case kKernelTypeBlades:
        return BokehShape::Blades;
    case kKernelTypeImage:
        return BokehShape::Image;
    default:
        return BokehShape::Disc;
    }
}

struct ZDefocusLayer {
    std::vector<BokehRun> runs;
    // A kernel that leaves the image unchanged: the layer is its weighted source.
    bool sharp = true;
};

/**
 * @brief What the passes of one render share. Over cocRect, a superset of every plane job's
 * scratch: each pixel's nearer slice and the weight of the next one, or for the focal plane
 * output its class. The layers' runs, the highlight and the render window. Per plane job, the
 * prefix tables, (width + 1) x height doubles for each plane and one for the slice's weights, and
 * the accumulated coverage, width x height floats; only a job larger than any before reallocates
 * them, and never while a pass of another job runs.
 **/
struct ZDefocusState {
    RectI cocRect;
    std::vector<uint16_t> slice;
    std::vector<float> frac;
    std::vector<ZDefocusLayer> layers;
    BokehHighlight highlight;
    RectI window;

    // With lens aberrations: whether slice k leaves channel group g sharp, groupSharp[k][g]; the runs
    // of slice k's kernel for a group and a cell's clip; those of the band being composited; and a
    // coverage per group, one after another, so that each channel is divided by its own.
    std::shared_ptr<const LensAberration> lens;
    std::vector<std::vector<unsigned char>> groupSharp;
    std::function<BokehRunCache::Runs(int, int, const BokehClip&)> kernel;
    LensBandRuns band;
    int coverageGroups = 1;
    std::size_t lensTaps = 1;

    std::mutex mutex;
    std::unique_ptr<double[]> tables;
    std::size_t capacity = 0;
    std::unique_ptr<float[]> coverage;
    std::size_t coverageCapacity = 0;

    static std::size_t tablePlaneSize(const PassScratch& scratch)
    {
        return ((std::size_t)scratch.width + 1) * (std::size_t)scratch.height;
    }

    void reserve(const PassScratch& scratch)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const std::size_t size = tablePlaneSize(scratch) * ((std::size_t)scratch.nPlanes + 1);
        if (capacity < size) {
            tables.reset(new double[size]);
            capacity = size;
        }
        const std::size_t pixels = (std::size_t)scratch.width * (std::size_t)scratch.height * (std::size_t)coverageGroups;
        if (coverageCapacity < pixels) {
            coverage.reset(new float[pixels]);
            coverageCapacity = pixels;
        }
    }

    /// The index in cocRect of the first pixel of row y of scratch.
    std::size_t cocIndex(const PassScratch& scratch,
                         int y) const
    {
        return (std::size_t)(scratch.rect.y1 + y - cocRect.y1) * (std::size_t)cocRect.width() + (std::size_t)(scratch.rect.x1 - cocRect.x1);
    }

    /// Slice k's weight over row y of scratch.
    void weightRow(const PassScratch& scratch,
                   int k,
                   int y,
                   float* w) const
    {
        const std::size_t base = cocIndex(scratch, y);
        for (int x = 0; x < scratch.width; ++x) {
            const int s = slice[base + x];
            w[x] = (s == k) ? 1.f - frac[base + x] : ((s + 1 == k) ? frac[base + x] : 0.f);
        }
    }
};

using ZDefocusStatePtr = std::shared_ptr<ZDefocusState>;

// The fewest rows of a band of lens cells, so that a small cell grid still gathers in parallel.
const int kMinBandRows = 32;
using Worker = std::function<PassRowFn(const PassScratch&)>;

/// Slot 1, every pixel of every plane: slot 0 with its highlights boosted.
Worker
boostWorker(const ZDefocusStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        const BokehHighlight highlight = state->highlight;
        const PassScratch* buffers = &scratch;

        return [highlight, buffers](int line) {
            const int height = buffers->height;
            const int width = buffers->width;
            const int p = line / height;
            const std::size_t row = (std::size_t)(line % height) * width;
            const float* const src = buffers->plane(0, p) + row;
            float* const dst = buffers->plane(1, p) + row;
            if (!highlight.isActive()) {
                std::copy(src, src + width, dst);

                return;
            }
            for (int x = 0; x < width; ++x) {
                dst[x] = highlight.boost(src[x]);
            }
        };
    };
}

/// Slot 2 and the coverage, every pixel: zero, which the layers are laid over.
Worker
clearWorker(const ZDefocusStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        float* const coverage = state->coverage.get();
        const int groups = state->coverageGroups;
        const PassScratch* buffers = &scratch;

        return [coverage, groups, buffers](int y) {
            const int width = buffers->width;
            const std::size_t row = (std::size_t)y * width;
            const std::size_t stride = (std::size_t)width * (std::size_t)buffers->height;
            for (int p = 0; p < buffers->nPlanes; ++p) {
                float* const dst = buffers->plane(2, p) + row;
                std::fill(dst, dst + width, 0.f);
            }
            for (int g = 0; g < groups; ++g) {
                std::fill(coverage + g * stride + row, coverage + g * stride + row + width, 0.f);
            }
        };
    };
}

/// The prefix tables of slice k's layer, every row: its weights, and slot 1 times them.
Worker
prefixWorker(const ZDefocusStatePtr& state,
             int k)
{
    return [state, k](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        const ZDefocusState* s = state.get();
        double* const tables = state->tables.get();
        const std::size_t planeSize = ZDefocusState::tablePlaneSize(scratch);
        const PassScratch* buffers = &scratch;
        std::shared_ptr<std::vector<float>> rows = std::make_shared<std::vector<float>>((std::size_t)scratch.width * 2);

        return [s, k, tables, planeSize, buffers, rows](int y) {
            const int width = buffers->width;
            const int nPlanes = buffers->nPlanes;
            float* const w = rows->data();
            float* const layer = w + width;
            const std::size_t tableRow = (std::size_t)y * (width + 1);
            s->weightRow(*buffers, k, y, w);
            bokehPrefixRow(w, width, tables + (std::size_t)nPlanes * planeSize + tableRow);
            for (int p = 0; p < nPlanes; ++p) {
                const float* const src = buffers->plane(1, p) + (std::size_t)y * width;
                for (int x = 0; x < width; ++x) {
                    layer[x] = w[x] * src[x];
                }
                bokehPrefixRow(layer, width, tables + (std::size_t)p * planeSize + tableRow);
            }
        };
    };
}

/**
 * @brief Over the render window, slice k's defocused layer laid over slot 2 and the coverage:
 * the layer's colour plus what its coverage leaves of the slices behind it.
 **/
Worker
compositeWorker(const ZDefocusStatePtr& state,
                int k)
{
    return [state, k](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        const ZDefocusState* s = state.get();
        const double* const tables = s->tables.get();
        float* const coverage = state->coverage.get();
        const std::size_t planeSize = ZDefocusState::tablePlaneSize(scratch);
        const PassScratch* buffers = &scratch;
        const int x1 = std::clamp(s->window.x1 - scratch.rect.x1, 0, scratch.width);
        const int x2 = std::clamp(s->window.x2 - scratch.rect.x1, x1, scratch.width);
        const int y1 = s->window.y1 - scratch.rect.y1;
        const int y2 = s->window.y2 - scratch.rect.y1;
        const std::size_t n = (std::size_t)std::max(1, x2 - x1);
        std::shared_ptr<std::vector<double>> acc = std::make_shared<std::vector<double>>(n);
        std::shared_ptr<std::vector<float>> rows = std::make_shared<std::vector<float>>(2 * n + (std::size_t)scratch.width);

        return [s, k, tables, coverage, planeSize, buffers, x1, x2, y1, y2, n, acc, rows](int y) {
            if ((y < y1) || (y >= y2) || (x1 == x2)) {
                return;
            }
            const int width = buffers->width;
            const int height = buffers->height;
            const int nPlanes = buffers->nPlanes;
            const ZDefocusLayer& layer = s->layers[k];
            float* const a = rows->data();
            float* const c = a + n;
            float* const w = c + n;
            const std::size_t row = (std::size_t)y * width;
            if (layer.sharp) {
                s->weightRow(*buffers, k, y, w);
                std::copy(w + x1, w + x2, a);
            } else {
                bokehGatherRow(tables + (std::size_t)nPlanes * planeSize, width, height, layer.runs, y, x1, x2, acc->data(), a);
            }
            float* const cov = coverage + row + x1;
            for (int p = 0; p < nPlanes; ++p) {
                if (layer.sharp) {
                    const float* const src = buffers->plane(1, p) + row + x1;
                    for (int i = 0; i < x2 - x1; ++i) {
                        c[i] = a[i] * src[i];
                    }
                } else {
                    bokehGatherRow(tables + (std::size_t)p * planeSize, width, height, layer.runs, y, x1, x2, acc->data(), c);
                }
                float* const dst = buffers->plane(2, p) + row + x1;
                for (int i = 0; i < x2 - x1; ++i) {
                    dst[i] = c[i] + std::max(0.f, 1.f - a[i]) * dst[i];
                }
            }
            for (int i = 0; i < x2 - x1; ++i) {
                cov[i] = a[i] + std::max(0.f, 1.f - a[i]) * cov[i];
            }
        };
    };
}

/**
 * @brief Slot 2 over the render window: the layers laid over each other, divided by their
 * coverage so that the slices' gaps where nearer objects stood do not darken what shows
 * through them, and unboosted.
 **/
Worker
finishWorker(const ZDefocusStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        const ZDefocusState* s = state.get();
        const float* const coverage = state->coverage.get();
        const PassScratch* buffers = &scratch;
        const int x1 = std::clamp(s->window.x1 - scratch.rect.x1, 0, scratch.width);
        const int x2 = std::clamp(s->window.x2 - scratch.rect.x1, x1, scratch.width);
        const int y1 = s->window.y1 - scratch.rect.y1;
        const int y2 = s->window.y2 - scratch.rect.y1;
        const BokehHighlight highlight = s->highlight;
        const LensAberration* const lens = s->lens.get();

        return [coverage, buffers, x1, x2, y1, y2, highlight, lens](int y) {
            if ((y < y1) || (y >= y2)) {
                return;
            }
            const std::size_t row = (std::size_t)y * buffers->width;
            const std::size_t stride = (std::size_t)buffers->width * (std::size_t)buffers->height;
            for (int p = 0; p < buffers->nPlanes; ++p) {
                const int group = lens ? lens->groupOf(buffers->channelBits[p]) : 0;
                const float* const cov = coverage + (std::size_t)group * stride + row;
                float* const dst = buffers->plane(2, p) + row;
                for (int x = x1; x < x2; ++x) {
                    const float v = (cov[x] > 0.f) ? dst[x] / cov[x] : 0.f;
                    dst[x] = highlight.isActive() ? highlight.unboost(v) : v;
                }
            }
        };
    };
}

/// Slice k's kernels for band b, spread over the lines, replacing the band before.
Worker
lensBuildWorker(const ZDefocusStatePtr& state,
                int k,
                const LensBand& b)
{
    return [state, k, b](const PassScratch& scratch) -> PassRowFn {
        ZDefocusState* s = state.get();
        const int nLines = scratch.height;

        return [s, k, b, nLines](int line) {
            std::size_t first = 0;
            std::size_t end = 0;
            spreadItems(line, nLines, s->band.size(b), &first, &end);
            for (std::size_t item = first; item < end; ++item) {
                int group = 0;
                int i = 0;
                int j = 0;
                s->band.item(b, item, &group, &i, &j);
                s->band.runs[item] = s->groupSharp[k][group] ? BokehRunCache::Runs() : s->kernel(k, group, s->lens->clip(i, j));
            }
        };
    };
}

/**
 * @brief compositeWorker() over band b's rows of the render window with lens aberrations: each
 * channel group gathers with its own kernel, cell by cell, and lays its coverage over its own.
 **/
Worker
lensCompositeWorker(const ZDefocusStatePtr& state,
                    int k,
                    const LensBand& b)
{
    return [state, k, b](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        const ZDefocusState* s = state.get();
        const double* const tables = s->tables.get();
        float* const coverage = state->coverage.get();
        const std::size_t planeSize = ZDefocusState::tablePlaneSize(scratch);
        const PassScratch* buffers = &scratch;
        const int x1 = std::clamp(s->window.x1 - scratch.rect.x1, 0, scratch.width);
        const int x2 = std::clamp(s->window.x2 - scratch.rect.x1, x1, scratch.width);
        const int y1 = std::clamp(std::max(b.y1, s->window.y1) - scratch.rect.y1, 0, scratch.height);
        const int y2 = std::clamp(std::min(b.y2, s->window.y2) - scratch.rect.y1, y1, scratch.height);
        const int groups = s->lens->groupCount();
        const std::size_t n = (std::size_t)std::max(1, x2 - x1);
        std::shared_ptr<std::vector<double>> acc = std::make_shared<std::vector<double>>(n);
        std::shared_ptr<std::vector<float>> rows = std::make_shared<std::vector<float>>((std::size_t)(groups + 1) * n + (std::size_t)scratch.width);

        return [s, k, b, tables, coverage, planeSize, buffers, x1, x2, y1, y2, groups, n, acc, rows](int line) {
            if (x1 == x2) {
                return;
            }
            const int width = buffers->width;
            const int height = buffers->height;
            const int nPlanes = buffers->nPlanes;
            const LensAberration& lens = *s->lens;
            const std::vector<unsigned char>& sharp = s->groupSharp[k];
            const std::size_t stride = (std::size_t)width * (std::size_t)height;
            const int rx1 = buffers->rect.x1;
            float* const c = rows->data() + (std::size_t)groups * n;
            float* const w = c + n;
            std::size_t first = 0;
            std::size_t end = 0;
            spreadItems(line, height, (std::size_t)(y2 - y1), &first, &end);
            for (std::size_t item = first; item < end; ++item) {
                const int y = y1 + (int)item;
                const int j = lens.rowOf(buffers->rect.y1 + y);
                const std::size_t row = (std::size_t)y * width;
                // Each group's coverage of the slice, gathered from its weights.
                s->weightRow(*buffers, k, y, w);
                for (int g = 0; g < groups; ++g) {
                    float* const a = rows->data() + (std::size_t)g * n;
                    if (sharp[g]) {
                        std::copy(w + x1, w + x2, a);
                        continue;
                    }
                    lens.forEachSegment(rx1 + x1, rx1 + x2, [&](int xa, int xb, int i) {
                        bokehGatherRow(tables + (std::size_t)nPlanes * planeSize, width, height, s->band.at(b, g, i, j), y, xa - rx1, xb - rx1, acc->data(), a + (xa - rx1 - x1));
                    });
                }
                for (int p = 0; p < nPlanes; ++p) {
                    const int g = lens.groupOf(buffers->channelBits[p]);
                    const float* const a = rows->data() + (std::size_t)g * n;
                    if (sharp[g]) {
                        const float* const src = buffers->plane(1, p) + row + x1;
                        for (int x = 0; x < x2 - x1; ++x) {
                            c[x] = a[x] * src[x];
                        }
                    } else {
                        lens.forEachSegment(rx1 + x1, rx1 + x2, [&](int xa, int xb, int i) {
                            bokehGatherRow(tables + (std::size_t)p * planeSize, width, height, s->band.at(b, g, i, j), y, xa - rx1, xb - rx1, acc->data(), c + (xa - rx1 - x1));
                        });
                    }
                    float* const dst = buffers->plane(2, p) + row + x1;
                    for (int x = 0; x < x2 - x1; ++x) {
                        dst[x] = c[x] + std::max(0.f, 1.f - a[x]) * dst[x];
                    }
                }
                for (int g = 0; g < groups; ++g) {
                    const float* const a = rows->data() + (std::size_t)g * n;
                    float* const cov = coverage + (std::size_t)g * stride + row + x1;
                    for (int x = 0; x < x2 - x1; ++x) {
                        cov[x] = a[x] + std::max(0.f, 1.f - a[x]) * cov[x];
                    }
                }
            }
        };
    };
}

/**
 * @brief Slot 1, every pixel: the source halved and its colour channels' halves lit by the
 * pixel's class, red in front of the focus, green in it and blue behind it. Other channels, and
 * the alpha, keep the source.
 **/
Worker
focalPlaneWorker(const ZDefocusStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        const ZDefocusState* s = state.get();
        const PassScratch* buffers = &scratch;

        return [s, buffers](int y) {
            const int width = buffers->width;
            const std::size_t row = (std::size_t)y * width;
            const std::size_t base = s->cocIndex(*buffers, y);
            for (int p = 0; p < buffers->nPlanes; ++p) {
                const int bit = buffers->channelBits[p];
                const float* const src = buffers->plane(0, p) + row;
                float* const dst = buffers->plane(1, p) + row;
                if (bit > 2) {
                    std::copy(src, src + width, dst);
                    continue;
                }
                for (int x = 0; x < width; ++x) {
                    const float lit = (s->slice[base + x] == bit) ? 1.f : 0.f;
                    dst[x] = 0.5f * src[x] + 0.5f * lit;
                }
            }
        };
    };
}

PassList
focalPlanePasses(const ZDefocusStatePtr& state)
{
    PassList list;

    list.scratchSlots = 2;
    list.resultSlot = 1;
    Pass focal = Pass::point(false, state ? focalPlaneWorker(state) : Worker());
    focal.slot = 1;
    list.passes.push_back(focal);

    return list;
}

/**
 * @brief Slice k's composite with lens aberrations, a band of cells at a time so that only the
 * band's kernels are held: each band's kernels built, then gathered.
 **/
void
appendLensComposite(const ZDefocusStatePtr& state,
                    int k,
                    PassList* list)
{
    const std::vector<LensBand> bands = state->lens->bands(state->window, kMinBandRows);
    if (state->band.runs.empty()) {
        state->band.reserve(bands);
    }
    for (const LensBand& b : bands) {
        Pass build = Pass::point(false, lensBuildWorker(state, k, b));
        build.slot = 2;
        list->passes.push_back(build);

        Pass composite = Pass::point(false, lensCompositeWorker(state, k, b));
        composite.slot = 2;
        composite.tapsPerSample = state->lensTaps;
        list->passes.push_back(composite);
    }
}

/**
 * @brief The passes of a Result render, with no work when state is null. The first carries the
 * whole halo, so that the region actions see it once whatever the slices.
 **/
PassList
resultPasses(int haloX,
             int haloY,
             double sigma,
             std::size_t taps,
             const ZDefocusStatePtr& state,
             const std::vector<unsigned char>& used)
{
    PassList list;

    list.scratchSlots = 3;
    list.resultSlot = 2;

    Pass extent = Pass::gather(haloX, haloY, false, Worker());
    extent.slot = 2;
    extent.sigma = sigma;
    extent.tapsPerSample = taps;
    list.passes.push_back(extent);
    if (!state) {
        return list;
    }

    Pass boost = Pass::point(true, boostWorker(state));
    boost.slot = 1;
    list.passes.push_back(boost);

    Pass clear = Pass::point(false, clearWorker(state));
    clear.slot = 2;
    list.passes.push_back(clear);

    // Back to front, each slice laid over the farther ones.
    for (int k = (int)state->layers.size() - 1; k >= 0; --k) {
        if (!used[k]) {
            continue;
        }
        const ZDefocusLayer& layer = state->layers[k];
        if (!layer.sharp) {
            Pass prefix = Pass::point(false, prefixWorker(state, k));
            prefix.slot = 2;
            list.passes.push_back(prefix);
        }
        if (state->lens) {
            appendLensComposite(state, k, &list);
            continue;
        }
        Pass composite = Pass::point(false, compositeWorker(state, k));
        composite.slot = 2;
        // Two table reads per run, for the weights and for each plane.
        composite.tapsPerSample = std::max<std::size_t>(1, 2 * layer.runs.size());
        list.passes.push_back(composite);
    }

    Pass finish = Pass::point(false, finishWorker(state));
    finish.slot = 2;
    list.passes.push_back(finish);

    return list;
}

double
clampedKnob(double value,
            double lo,
            double hi,
            double fallback)
{
    return std::isfinite(value) ? std::min(hi, std::max(lo, value)) : fallback;
}

#ifdef NATRON_HAVE_GPU
// The largest halo, in pixels, the GPU records; a larger defocus stays on the CPU.
const int kGpuMaxHalo = 256;

// Mirror the constants of Kernels/zdefocus.slang.
const uint32_t kGpuBlock = 256;
const uint32_t kGpuBoost = 1;
const uint32_t kGpuSharp = 2;
const uint32_t kGpuFirst = 4;
const uint32_t kGpuInverseDistance = 8;
const uint32_t kGpuHasDepth = 16;
const std::size_t kGpuMetaCentres = 12;

// Mirrors ZDefocusParams in Kernels/zdefocus.slang, which has no padding between members.
struct ZDefocusGpuParams {
    int32_t domX1;
    int32_t domY1;
    uint32_t domWidth;
    uint32_t domHeight;
    uint32_t channels;
    uint32_t nBlocks;
    uint32_t cocOffset;
    uint32_t localOffset;
    uint32_t totalOffset;
    uint32_t accOffset;
    uint32_t runOffset;
    uint32_t runCount;
    uint32_t slice;
    uint32_t flags;
    float threshold;
    float gain;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t dstOffset;
    uint32_t dstStride;
    uint32_t depthOffset;
    uint32_t depthStride;
    uint32_t depthComps;
    uint32_t depthComponent;
};
static_assert(sizeof(ZDefocusGpuParams) == 128);
static_assert(offsetof(ZDefocusGpuParams, srcX1) == 64);
static_assert(offsetof(ZDefocusGpuParams, depthOffset) == 112);

// The float index of pixel (x, y) of image, or false when the image exceeds the kernels' 32-bit
// indices.
bool
gpuFloatIndex(const gpu::GpuBlurImage& image,
              int x,
              int y,
              int nComps,
              uint32_t* out)
{
    const uint64_t index = image.offsetFloats + (uint64_t)(y - image.bounds.y1) * image.strideFloats + (uint64_t)(x - image.bounds.x1) * nComps;
    const uint64_t end = image.offsetFloats + (uint64_t)(image.bounds.y2 - image.bounds.y1) * image.strideFloats;
    if ((index > UINT32_MAX) || (end > UINT32_MAX)) {
        return false;
    }
    *out = (uint32_t)index;

    return true;
}

// Every kernel of Kernels/zdefocus.slang binds these, in this order.
struct ZDefocusGpuBuffers {
    const gpu::GpuBuffer* source = nullptr;
    const gpu::GpuBuffer* depth = nullptr;
    const gpu::GpuBuffer* meta = nullptr;
    const gpu::GpuBuffer* work = nullptr;
    const gpu::GpuBuffer* out = nullptr;
};

bool
dispatchZDefocus(gpu::GpuRecordContext& ctx,
                 gpu::GpuKernelId kernel,
                 const ZDefocusGpuBuffers& b,
                 const ZDefocusGpuParams& p,
                 uint32_t width,
                 uint32_t height)
{
    if (!width || !height) {
        return true;
    }

    return (bool)ctx.dispatch(kernel, { b.source, b.depth, b.meta, b.work, b.out }, p, { width, height, 1 });
}

uint32_t
floatBits(double value)
{
    return std::bit_cast<uint32_t>((float)value);
}
#endif // NATRON_HAVE_GPU
} // anonymous namespace

struct ZDefocus::KernelCache {
    std::mutex mutex;
    BokehShapeParams shape;
    std::vector<std::pair<double, double>> radii;
    std::vector<std::shared_ptr<const BokehKernel>> kernels;
};

ZDefocus::ZDefocus(NodePtr node)
    : StagedImageEffect(node)
    , _depth()
    , _depthUnits()
    , _mode()
    , _focalPlane()
    , _depthOfField()
    , _size()
    , _maxSize()
    , _focalLength()
    , _fStop()
    , _filmBackWidth()
    , _sceneUnit()
    , _focalPoint()
    , _layers()
    , _aspectRatio()
    , _kernelType()
    , _blades()
    , _rotation()
    , _roundness()
    , _kernelChannel()
    , _highlightThreshold()
    , _highlightGain()
    , _output()
    , _bokehKnobs()
    , _kernelCache(new KernelCache)
    , _runCache(std::make_shared<BokehRunCache>())
{
}

ZDefocus::~ZDefocus()
{
}

const NodeSpec*
ZDefocus::nodeSpec() const
{
    return staticNodeSpec<kZDefocusNodeSpec>();
}

const ImagePipelineSpec&
ZDefocus::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kZDefocusPipeline, kZDefocusNodeSpec>();
}

std::string
ZDefocus::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kZDefocusInputSource:
        return tr("The image to defocus, which holds the Depth channel.").toStdString();
    case kZDefocusInputMask:
        return tr("The mask the defocused result is mixed with the source through.").toStdString();
    case kZDefocusInputKernel:
        return tr("The Kernel: with the Image kernel type, the bokeh's shape, its Kernel Channel over its region of definition.").toStdString();
    }

    return std::string();
}

void
ZDefocus::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobChannelSelectPtr depthKnob = AppManager::createKnob<KnobChannelSelect>(this, tr("Depth"));
    depthKnob->setName(kZDefocusParamDepth);
    depthKnob->setAnimationEnabled(false);
    depthKnob->setDefaultValue(depthKnob->encode("depth.Z"));
    depthKnob->setHintToolTip(tr("The channel of the Source input, in any of its layers, that holds the depth. A channel the input does not have, "
                                 "or None, reads 0, which is infinitely far."));
    page->addKnob(depthKnob);
    _depth = depthKnob;

    KnobChoicePtr unitsKnob = AppManager::createKnob<KnobChoice>(this, tr("Depth Units"));
    unitsKnob->setName(kZDefocusParamDepthUnits);
    unitsKnob->setHintToolTip(tr("What the Depth channel holds. The Focal Plane and Depth Of Field are in the same units."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kZDefocusParamDepthUnitsDistance, tr("Distance").toStdString(), tr("The distance to the camera.").toStdString()));
        options.push_back(ChoiceOption(kZDefocusParamDepthUnitsInverseDistance, tr("Inverse Distance").toStdString(),
                                       tr("One over the distance to the camera, larger nearer, as most renderers write it.").toStdString()));
        unitsKnob->populateChoices(options);
    }
    unitsKnob->setDefaultValue(kUnitsInverseDistance);
    unitsKnob->setAnimationEnabled(false);
    page->addKnob(unitsKnob);
    _depthUnits = unitsKnob;

    KnobChoicePtr modeKnob = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
    modeKnob->setName(kZDefocusParamMode);
    modeKnob->setHintToolTip(tr("How the depth sets the size of the bokeh."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kZDefocusParamModeManual, tr("Manual").toStdString(),
                                       tr("The Depth Of Field around the Focal Plane is sharp, and the bokeh grows to Size infinitely far behind it.").toStdString()));
        options.push_back(ChoiceOption(kZDefocusParamModeLens, tr("Lens").toStdString(),
                                       tr("A thin lens of the Focal Length and F Stop on a film back of the Film Back Width, focused at the Focal Plane.").toStdString()));
        modeKnob->populateChoices(options);
    }
    modeKnob->setDefaultValue(0);
    modeKnob->setAnimationEnabled(false);
    page->addKnob(modeKnob);
    _mode = modeKnob;

    KnobDoublePtr focalPlaneKnob = AppManager::createKnob<KnobDouble>(this, tr("Focal Plane"));
    focalPlaneKnob->setName(kZDefocusParamFocalPlane);
    focalPlaneKnob->setHintToolTip(tr("The depth in focus, in the Depth Units. Moving the Focal Point sets it to the depth under the point."));
    focalPlaneKnob->setDisplayMinimum(0.);
    focalPlaneKnob->setDisplayMaximum(10.);
    focalPlaneKnob->setDefaultValue(1.);
    page->addKnob(focalPlaneKnob);
    _focalPlane = focalPlaneKnob;

    KnobDoublePtr dofKnob = AppManager::createKnob<KnobDouble>(this, tr("Depth Of Field"));
    dofKnob->setName(kZDefocusParamDepthOfField);
    dofKnob->setHintToolTip(tr("Manual mode only: the width, in the Depth Units, of the slice of depths around the Focal Plane that is sharp."));
    dofKnob->setMinimum(0.);
    dofKnob->setDisplayMinimum(0.);
    dofKnob->setDisplayMaximum(10.);
    dofKnob->setDefaultValue(0.);
    page->addKnob(dofKnob);
    _depthOfField = dofKnob;

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(this, tr("Size"));
    sizeKnob->setName(kZDefocusParamSize);
    sizeKnob->setHintToolTip(tr("Manual mode only: the diameter of the bokeh, in pixel units, infinitely far behind the Depth Of Field."));
    sizeKnob->setSpatial(true);
    sizeKnob->setMinimum(0.);
    sizeKnob->setMaximum(kMaxSize);
    sizeKnob->setDisplayMinimum(0.);
    sizeKnob->setDisplayMaximum(100.);
    sizeKnob->setIncrement(0.1);
    sizeKnob->setDecimals(1);
    sizeKnob->setDefaultValue(10.);
    page->addKnob(sizeKnob);
    _size = sizeKnob;

    KnobDoublePtr maxSizeKnob = AppManager::createKnob<KnobDouble>(this, tr("Max Size"));
    maxSizeKnob->setName(kZDefocusParamMaxSize);
    maxSizeKnob->setHintToolTip(tr("The largest diameter of the bokeh, in pixel units, whatever the depth. The depths are sliced over the "
                                   "diameters up to it, so a smaller one gives the Layers finer steps."));
    maxSizeKnob->setSpatial(true);
    maxSizeKnob->setMinimum(0.);
    maxSizeKnob->setMaximum(kMaxSize);
    maxSizeKnob->setDisplayMinimum(0.);
    maxSizeKnob->setDisplayMaximum(200.);
    maxSizeKnob->setDefaultValue(50.);
    page->addKnob(maxSizeKnob);
    _maxSize = maxSizeKnob;

    KnobDoublePtr focalLengthKnob = AppManager::createKnob<KnobDouble>(this, tr("Focal Length"));
    focalLengthKnob->setName(kZDefocusParamFocalLength);
    focalLengthKnob->setHintToolTip(tr("Lens mode only: the lens's focal length in millimetres."));
    focalLengthKnob->setMinimum(1.);
    focalLengthKnob->setMaximum(5000.);
    focalLengthKnob->setDisplayMinimum(8.);
    focalLengthKnob->setDisplayMaximum(300.);
    focalLengthKnob->setDefaultValue(50.);
    page->addKnob(focalLengthKnob);
    _focalLength = focalLengthKnob;

    KnobDoublePtr fStopKnob = AppManager::createKnob<KnobDouble>(this, tr("F Stop"));
    fStopKnob->setName(kZDefocusParamFStop);
    fStopKnob->setHintToolTip(tr("Lens mode only: the aperture's f-number. Doubling it halves the bokeh."));
    fStopKnob->setMinimum(0.5);
    fStopKnob->setMaximum(128.);
    fStopKnob->setDisplayMinimum(1.);
    fStopKnob->setDisplayMaximum(32.);
    fStopKnob->setDefaultValue(2.8);
    page->addKnob(fStopKnob);
    _fStop = fStopKnob;

    KnobDoublePtr filmBackKnob = AppManager::createKnob<KnobDouble>(this, tr("Film Back Width"));
    filmBackKnob->setName(kZDefocusParamFilmBackWidth);
    filmBackKnob->setHintToolTip(tr("Lens mode only: the width, in millimetres, of the film back or sensor the image's width spans."));
    filmBackKnob->setMinimum(0.1);
    filmBackKnob->setMaximum(1000.);
    filmBackKnob->setDisplayMinimum(4.);
    filmBackKnob->setDisplayMaximum(70.);
    filmBackKnob->setDefaultValue(36.);
    page->addKnob(filmBackKnob);
    _filmBackWidth = filmBackKnob;

    KnobChoicePtr unitKnob = AppManager::createKnob<KnobChoice>(this, tr("Scene Unit"));
    unitKnob->setName(kZDefocusParamSceneUnit);
    unitKnob->setHintToolTip(tr("Lens mode only: the unit of distance of the Depth channel, so that it compares with the Focal Length."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kZDefocusParamSceneUnitMillimetres, tr("Millimetres").toStdString(), std::string()));
        options.push_back(ChoiceOption(kZDefocusParamSceneUnitCentimetres, tr("Centimetres").toStdString(), std::string()));
        options.push_back(ChoiceOption(kZDefocusParamSceneUnitMetres, tr("Metres").toStdString(), std::string()));
        options.push_back(ChoiceOption(kZDefocusParamSceneUnitInches, tr("Inches").toStdString(), std::string()));
        options.push_back(ChoiceOption(kZDefocusParamSceneUnitFeet, tr("Feet").toStdString(), std::string()));
        unitKnob->populateChoices(options);
    }
    unitKnob->setDefaultValue(kSceneUnitMetres);
    unitKnob->setAnimationEnabled(false);
    page->addKnob(unitKnob);
    _sceneUnit = unitKnob;

    KnobDoublePtr focalPointKnob = AppManager::createKnob<KnobDouble>(this, tr("Focal Point"), 2);
    focalPointKnob->setName(kZDefocusParamFocalPoint);
    focalPointKnob->setHintToolTip(tr("A point of the image: moving it sets the Focal Plane to the depth there."));
    focalPointKnob->setSpatial(true);
    focalPointKnob->disableSlider();
    focalPointKnob->setDefaultValuesAreNormalized(true);
    focalPointKnob->setAnimationEnabled(false);
    focalPointKnob->setEvaluateOnChange(false);
    for (int d = 0; d < 2; ++d) {
        focalPointKnob->setMinimum(-DBL_MAX, d);
        focalPointKnob->setMaximum(DBL_MAX, d);
        focalPointKnob->setDefaultValue(0.5, d);
    }
    page->addKnob(focalPointKnob);
    focalPointKnob->setHasHostOverlayHandle(true);
    _focalPoint = focalPointKnob;

    KnobIntPtr layersKnob = AppManager::createKnob<KnobInt>(this, tr("Layers"));
    layersKnob->setName(kZDefocusParamLayers);
    layersKnob->setHintToolTip(tr("How many depth slices the defocus is cut into, one in focus and the rest shared between the bokeh sizes in front "
                                  "of it and behind it. More slices give smoother steps and take longer."));
    layersKnob->setMinimum(2);
    layersKnob->setMaximum(kMaxLayers);
    layersKnob->setDisplayMinimum(2);
    layersKnob->setDisplayMaximum(kMaxLayers);
    layersKnob->setDefaultValue(16);
    layersKnob->setAnimationEnabled(false);
    page->addKnob(layersKnob);
    _layers = layersKnob;

    KnobDoublePtr aspectKnob = AppManager::createKnob<KnobDouble>(this, tr("Aspect Ratio"));
    aspectKnob->setName(kZDefocusParamAspectRatio);
    aspectKnob->setHintToolTip(tr("Width of the bokeh over its height."));
    aspectKnob->setMinimum(0.1);
    aspectKnob->setMaximum(10.);
    aspectKnob->setDisplayMinimum(0.25);
    aspectKnob->setDisplayMaximum(4.);
    aspectKnob->setIncrement(0.01);
    aspectKnob->setDecimals(2);
    aspectKnob->setDefaultValue(1.);
    page->addKnob(aspectKnob);
    _aspectRatio = aspectKnob;

    KnobChoicePtr typeKnob = AppManager::createKnob<KnobChoice>(this, tr("Kernel Type"));
    typeKnob->setName(kZDefocusParamKernelType);
    typeKnob->setHintToolTip(tr("The shape of the bokeh."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kZDefocusParamKernelTypeDisc, tr("Disc").toStdString(), tr("A disc, as a round aperture gives.").toStdString()));
        options.push_back(ChoiceOption(kZDefocusParamKernelTypeBlades, tr("Blades").toStdString(), tr("An aperture of Blades straight or curved blades.").toStdString()));
        options.push_back(ChoiceOption(kZDefocusParamKernelTypeImage, tr("Image").toStdString(), tr("The Kernel input's Kernel Channel, stretched over each slice's bokeh.").toStdString()));
        typeKnob->populateChoices(options);
    }
    typeKnob->setDefaultValue(kKernelTypeDisc);
    typeKnob->setAnimationEnabled(false);
    page->addKnob(typeKnob);
    _kernelType = typeKnob;

    KnobIntPtr bladesKnob = AppManager::createKnob<KnobInt>(this, tr("Blades"));
    bladesKnob->setName(kZDefocusParamBlades);
    bladesKnob->setHintToolTip(tr("Blades kernel only: the number of blades of the aperture."));
    bladesKnob->setMinimum(3);
    bladesKnob->setMaximum(16);
    bladesKnob->setDisplayMinimum(3);
    bladesKnob->setDisplayMaximum(12);
    bladesKnob->setDefaultValue(6);
    page->addKnob(bladesKnob);
    _blades = bladesKnob;

    KnobDoublePtr rotationKnob = AppManager::createKnob<KnobDouble>(this, tr("Rotation"));
    rotationKnob->setName(kZDefocusParamRotation);
    rotationKnob->setHintToolTip(tr("Blades kernel only: the aperture's rotation in degrees, counter-clockwise. At 0 a corner points up."));
    rotationKnob->setDisplayMinimum(-180.);
    rotationKnob->setDisplayMaximum(180.);
    rotationKnob->setDefaultValue(0.);
    page->addKnob(rotationKnob);
    _rotation = rotationKnob;

    KnobDoublePtr roundnessKnob = AppManager::createKnob<KnobDouble>(this, tr("Roundness"));
    roundnessKnob->setName(kZDefocusParamRoundness);
    roundnessKnob->setHintToolTip(tr("Blades kernel only: how curved the blades are, from straight at 0 to the disc at 1."));
    roundnessKnob->setMinimum(0.);
    roundnessKnob->setMaximum(1.);
    roundnessKnob->setDisplayMinimum(0.);
    roundnessKnob->setDisplayMaximum(1.);
    roundnessKnob->setDefaultValue(0.);
    page->addKnob(roundnessKnob);
    _roundness = roundnessKnob;

    _bokehKnobs.addTextureKnobs(this, page);

    KnobChannelSelectPtr channelKnob = AppManager::createKnob<KnobChannelSelect>(this, tr("Kernel Channel"));
    channelKnob->setName(kZDefocusParamKernelChannel);
    channelKnob->setAnimationEnabled(false);
    channelKnob->setDefaultValue(channelKnob->encode(std::string(kNatronColorViewRGBA) + ".A"));
    channelKnob->setHintToolTip(tr("Image kernel only: the channel of the Kernel input, in any of its layers, that holds the bokeh. A channel the input does not have, or None, reads 0."));
    page->addKnob(channelKnob);
    _kernelChannel = channelKnob;

    _bokehKnobs.addLensKnobs(this, page);

    KnobDoublePtr thresholdKnob = AppManager::createKnob<KnobDouble>(this, tr("Highlight Threshold"));
    thresholdKnob->setName(kZDefocusParamHighlightThreshold);
    thresholdKnob->setHintToolTip(tr("Values above the threshold are highlights, which Highlight Gain boosts before the defocus."));
    thresholdKnob->setMinimum(0.);
    thresholdKnob->setDisplayMinimum(0.);
    thresholdKnob->setDisplayMaximum(10.);
    thresholdKnob->setDefaultValue(1.);
    page->addKnob(thresholdKnob);
    _highlightThreshold = thresholdKnob;

    KnobDoublePtr gainKnob = AppManager::createKnob<KnobDouble>(this, tr("Highlight Gain"));
    gainKnob->setName(kZDefocusParamHighlightGain);
    gainKnob->setHintToolTip(tr("How much the highlights' excess over the threshold is multiplied by before the defocus, and divided by after it, "
                                "so that they bloom into bright bokeh. 1 leaves them as they are."));
    gainKnob->setMinimum(1.);
    gainKnob->setMaximum(1000.);
    gainKnob->setDisplayMinimum(1.);
    gainKnob->setDisplayMaximum(20.);
    gainKnob->setDefaultValue(1.);
    page->addKnob(gainKnob);
    _highlightGain = gainKnob;

    KnobChoicePtr outputKnob = AppManager::createKnob<KnobChoice>(this, tr("Output"));
    outputKnob->setName(kZDefocusParamOutput);
    outputKnob->setHintToolTip(tr("What the node outputs."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kZDefocusParamOutputResult, tr("Result").toStdString(), tr("The defocused image.").toStdString()));
        options.push_back(ChoiceOption(kZDefocusParamOutputFocalPlane, tr("Focal Plane").toStdString(),
                                       tr("The source, its colour half red in front of the focus, green in it and blue behind it.").toStdString()));
        outputKnob->populateChoices(options);
    }
    outputKnob->setDefaultValue(0);
    outputKnob->setAnimationEnabled(false);
    page->addKnob(outputKnob);
    _output = outputKnob;

    addStageKnobs(page);

    getNode()->declareLayerKnob(depthKnob, kZDefocusInputSource, LayerKnobSpec::eRoleInputBound);
    getNode()->declareLayerKnob(channelKnob, kZDefocusInputKernel, LayerKnobSpec::eRoleInputBound);
} // ZDefocus::initializeKnobs

ZDefocusParams
ZDefocus::getParams(double time,
                    ViewIdx view) const
{
    const KnobReader reader(time, view);
    ZDefocusParams params;
    DepthOfFieldParams& dof = params.dof;

    dof.units = (reader.value(_depthUnits) == kUnitsInverseDistance) ? DepthUnits::InverseDistance : DepthUnits::Distance;
    dof.mode = (reader.value(_mode) == kModeLens) ? DepthOfFieldMode::Lens : DepthOfFieldMode::Manual;
    dof.focalPlane = clampedKnob(reader.value(_focalPlane), -DBL_MAX, DBL_MAX, 0.);
    dof.depthOfField = clampedKnob(reader.value(_depthOfField), 0., DBL_MAX, 0.);
    dof.size = clampedKnob(reader.value(_size), 0., kMaxSize, 0.);
    dof.maxSize = clampedKnob(reader.value(_maxSize), 0., kMaxSize, 0.);
    dof.focalLength = clampedKnob(reader.value(_focalLength), 1., 5000., 50.);
    dof.fStop = clampedKnob(reader.value(_fStop), 0.5, 128., 2.8);
    dof.filmBackWidth = clampedKnob(reader.value(_filmBackWidth), 0.1, 1000., 36.);
    const int unit = std::clamp((int)reader.value(_sceneUnit), 0, (int)(sizeof(kSceneUnitMillimetres) / sizeof(kSceneUnitMillimetres[0])) - 1);
    dof.millimetresPerUnit = kSceneUnitMillimetres[unit];
    NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    if (metadata) {
        const double par = metadata->getPixelAspectRatio(-1);
        dof.imageWidth = (double)metadata->getOutputFormat().width() * ((par > 0.) ? par : 1.);
    }

    params.layers = std::clamp((int)reader.value(_layers), 2, kMaxLayers);
    params.aspectRatio = clampedKnob(reader.value(_aspectRatio), 0.1, 10., 1.);
    params.shape.shape = shapeOf(reader.value(_kernelType));
    params.shape.blades = std::max(3, (int)reader.value(_blades));
    params.shape.rotation = reader.value(_rotation);
    params.shape.roundness = reader.value(_roundness);
    _bokehKnobs.readTexture(reader, &params.shape);
    params.lens = _bokehKnobs.readLens(reader);
    params.highlight.threshold = (float)std::max(0., reader.value(_highlightThreshold));
    params.highlight.gain = (float)std::max(1., reader.value(_highlightGain));
    if (!std::isfinite(params.highlight.threshold) || !std::isfinite(params.highlight.gain)) {
        params.highlight = BokehHighlight();
    }
    params.focalPlaneOutput = reader.value(_output) == kOutputFocalPlane;

    return params;
} // ZDefocus::getParams

void
ZDefocus::radii(const ZDefocusParams& params,
                const PassContext& context,
                double coc,
                double* radiusX,
                double* radiusY)
{
    const OfxPointD rs = context.scale.toOfxPointD();
    const double par = (context.pixelAspectRatio > 0.) ? context.pixelAspectRatio : 1.;
    const double size = std::abs(coc);

    *radiusX = std::min(kMaxRadius, rs.x * size * params.aspectRatio / 2. / par);
    *radiusY = std::min(kMaxRadius, rs.y * size / 2.);
}

void
ZDefocus::radii(const ZDefocusParams& params,
                const PassContext& context,
                double coc,
                double scale,
                double* radiusX,
                double* radiusY)
{
    if (scale == 1.) {
        radii(params, context, coc, radiusX, radiusY);

        return;
    }
    const double maxSize = std::max(0., params.dof.maxSize);

    radii(params, context, std::min(maxSize, std::abs(coc) * scale), radiusX, radiusY);
}

std::vector<ZDefocusSlice>
ZDefocus::slices(const ZDefocusParams& params,
                 const PassContext& context)
{
    std::vector<ZDefocusSlice> result;
    const DepthOfField dof(params.dof);
    const double front = dof.frontMax();
    const double back = dof.backMax();
    double rx = 0.;
    double ry = 0.;

    // A side blurs when its most scaled channel does.
    const double scale = params.lens.maxScale();
    radii(params, context, front, scale, &rx, &ry);
    const bool frontBlurs = !BokehKernel::isIdentityRadius(rx, ry);
    radii(params, context, back, scale, &rx, &ry);
    const bool backBlurs = !BokehKernel::isIdentityRadius(rx, ry);
    if (!frontBlurs && !backBlurs) {
        return result;
    }
    // Besides the slice in focus, each side that blurs gets a share of the rest, at least one.
    const int rest = std::max(1, params.layers - 1);
    int nFront = 0;
    int nBack = 0;
    if (frontBlurs && backBlurs) {
        nFront = std::clamp((int)std::lround(rest * front / (front + back)), 1, std::max(1, rest - 1));
        nBack = std::max(1, rest - nFront);
    } else if (frontBlurs) {
        nFront = rest;
    } else {
        nBack = rest;
    }

    std::vector<double> centres;
    for (int i = nFront; i >= 1; --i) {
        centres.push_back(-front * i / nFront);
    }
    centres.push_back(0.);
    for (int i = 1; i <= nBack; ++i) {
        centres.push_back(back * i / nBack);
    }
    for (double coc : centres) {
        ZDefocusSlice slice;
        slice.coc = coc;
        radii(params, context, coc, &slice.radiusX, &slice.radiusY);
        result.push_back(slice);
    }

    return result;
} // ZDefocus::slices

float
ZDefocus::pixelCoc(const ZDefocusParams& params,
                   const PassContext& context,
                   const DepthOfField& dof,
                   float depth)
{
    const double coc = dof.coc(depth);
    double rx = 0.;
    double ry = 0.;

    radii(params, context, coc, params.lens.maxScale(), &rx, &ry);

    return BokehKernel::isIdentityRadius(rx, ry) ? 0.f : (float)coc;
}

void
ZDefocus::halo(const ZDefocusParams& params,
               const PassContext& context,
               const std::vector<ZDefocusSlice>& slices,
               int* haloX,
               int* haloY)
{
    *haloX = 0;
    *haloY = 0;
    if (params.focalPlaneOutput) {
        return;
    }
    const double scale = params.lens.maxScale();
    for (const ZDefocusSlice& slice : slices) {
        double rx = slice.radiusX;
        double ry = slice.radiusY;
        if (scale != 1.) {
            radii(params, context, slice.coc, scale, &rx, &ry);
        }
        *haloX = std::max(*haloX, BokehKernel::extent(rx));
        *haloY = std::max(*haloY, BokehKernel::extent(ry));
    }
}

bool
ZDefocus::fitsGpu(const ZDefocusParams& params,
                  const PassContext& context)
{
#ifdef NATRON_HAVE_GPU
    if (params.focalPlaneOutput) {
        return true;
    }
    if ((params.shape.shape == BokehShape::Image) || params.lens.isActive()) {
        return false;
    }
    const std::vector<ZDefocusSlice> sliceList = slices(params, context);
    if (sliceList.empty()) {
        return false;
    }
    int haloX = 0;
    int haloY = 0;
    halo(params, context, sliceList, &haloX, &haloY);

    return (haloX <= kGpuMaxHalo) && (haloY <= kGpuMaxHalo);
#else
    Q_UNUSED(params);
    Q_UNUSED(context);

    return false;
#endif
}

/**
 * @brief The passes of renderOp() as the GPU records them over io.domain, the CPU's scratch rect:
 * the boosted source and every pixel's slice; then for each slice from the back, its layer's
 * block prefixes and its gather composited over the slices behind; then the composite divided by
 * its coverage and unboosted into io.filtered. Every slice is recorded, used or not: an unused one
 * composites nothing. The focal plane output is one pass.
 **/
StatusEnum
ZDefocus::recordPassesGpu(const PassGpuIO& io,
                          const ZDefocusParams& params,
                          const PassContext& context,
                          const ImageLayerDesc& depthLayer,
                          int depthChannel)
{
#ifdef NATRON_HAVE_GPU
    gpu::GpuRecordContext& ctx = io.context;
    const RectI& domain = io.domain;
    const int nComps = io.nComps;
    if (PassExec::isEmptyRect(domain) || PassExec::isEmptyRect(io.output) || (nComps < 1) || (nComps > 4) || !io.filtered.buffer || !fitsGpu(params, context)) {
        return eStatusFailed;
    }

    // The depth as the CPU reads it: 0, infinitely far, outside the input's image.
    const GpuLayerInput* depth = nullptr;
    for (const GpuLayerInput& layerInput : io.layerInputs) {
        if ((layerInput.inputNb == kZDefocusInputSource) && (layerInput.layer == depthLayer)) {
            depth = &layerInput;
            break;
        }
    }
    if (!depth) {
        return eStatusFailed;
    }

    const DepthOfField dof(params.dof);
    std::vector<ZDefocusSlice> sliceList;
    std::vector<BokehKernel> kernels;
    if (!params.focalPlaneOutput) {
        sliceList = slices(params, context);
        bool anyBlur = false;
        for (const ZDefocusSlice& slice : sliceList) {
            kernels.push_back(BokehKernel::gatherKernel(params.shape, slice.radiusX, slice.radiusY));
            anyBlur = anyBlur || !kernels.back().isIdentity();
        }
        if (!anyBlur) {
            return eStatusFailed;
        }
    }
    double snapX = 0.;
    double snapY = 0.;
    radii(params, context, 1., &snapX, &snapY);

    std::vector<uint32_t> meta(kGpuMetaCentres, 0u);
    if (depth->view.buffer) {
        meta[0] = (uint32_t)depth->view.bounds.x1;
        meta[1] = (uint32_t)depth->view.bounds.y1;
        meta[2] = (uint32_t)depth->view.bounds.x2;
        meta[3] = (uint32_t)depth->view.bounds.y2;
    }
    meta[4] = floatBits(dof.nearEdge());
    meta[5] = floatBits(dof.farEdge());
    meta[6] = floatBits(dof.scale());
    meta[7] = floatBits(dof.minDistance());
    meta[8] = floatBits(dof.maxSize());
    meta[9] = floatBits(snapX);
    meta[10] = floatBits(snapY);
    meta[11] = (uint32_t)sliceList.size();
    for (const ZDefocusSlice& slice : sliceList) {
        meta.push_back(floatBits(slice.coc));
    }
    std::vector<uint32_t> runOffsets;
    std::vector<uint32_t> runCounts;
    for (const BokehKernel& kernel : kernels) {
        const std::vector<BokehRun> runs = kernel.isIdentity() ? std::vector<BokehRun>() : kernel.runs();
        runOffsets.push_back((uint32_t)meta.size());
        runCounts.push_back((uint32_t)runs.size());
        for (const BokehRun& run : runs) {
            meta.push_back((uint32_t)run.dy);
            meta.push_back((uint32_t)run.dx0);
            meta.push_back((uint32_t)run.length);
            meta.push_back(std::bit_cast<uint32_t>(run.weight));
        }
    }

    const uint64_t width = (uint64_t)domain.width();
    const uint64_t height = (uint64_t)domain.height();
    const uint64_t n = (uint64_t)nComps;
    const uint64_t nBlocks = (width + kGpuBlock - 1) / kGpuBlock;
    const uint64_t outPixels = (uint64_t)io.output.width() * (uint64_t)io.output.height();
    const uint64_t slotFloats = width * height * n;
    const uint64_t cocFloats = params.focalPlaneOutput ? 0 : 2 * width * height;
    const uint64_t localFloats = params.focalPlaneOutput ? 0 : (width + 1) * height * (n + 1);
    const uint64_t totalFloats = params.focalPlaneOutput ? 0 : nBlocks * height * (n + 1);
    const uint64_t accFloats = params.focalPlaneOutput ? 0 : outPixels * (n + 1);
    const uint64_t workFloats = std::max<uint64_t>(1, params.focalPlaneOutput ? 1 : slotFloats + cocFloats + localFloats + totalFloats + accFloats);
    if ((workFloats > UINT32_MAX) || (meta.size() > UINT32_MAX)) {
        return eStatusFailed;
    }

    gpu::GpuBuffer* metaBuffer = nullptr;
    gpu::GpuBuffer* work = nullptr;
    if (!ctx.scratch(meta.size() * sizeof(uint32_t), metaBuffer) || !metaBuffer || !ctx.scratch(workFloats * sizeof(float), work) || !work) {
        return eStatusFailed;
    }
    const std::span<const std::byte> bytes = std::as_bytes(std::span<const uint32_t>(meta));
    const std::size_t kChunk = 65536;
    for (std::size_t offset = 0; offset < bytes.size(); offset += kChunk) {
        if (!ctx.updateBuffer(*metaBuffer, offset, bytes.subspan(offset, std::min(kChunk, bytes.size() - offset)))) {
            return eStatusFailed;
        }
    }

    ZDefocusGpuBuffers buffers;
    buffers.source = metaBuffer;
    buffers.depth = metaBuffer;
    buffers.meta = metaBuffer;
    buffers.work = work;
    buffers.out = io.filtered.buffer;

    ZDefocusGpuParams base {};
    base.domX1 = domain.x1;
    base.domY1 = domain.y1;
    base.domWidth = (uint32_t)width;
    base.domHeight = (uint32_t)height;
    base.channels = (uint32_t)nComps;
    base.nBlocks = (uint32_t)nBlocks;
    base.cocOffset = (uint32_t)slotFloats;
    base.localOffset = (uint32_t)(slotFloats + cocFloats);
    base.totalOffset = (uint32_t)(slotFloats + cocFloats + localFloats);
    base.accOffset = (uint32_t)(slotFloats + cocFloats + localFloats + totalFloats);
    base.flags = (params.highlight.isActive() && !params.focalPlaneOutput) ? kGpuBoost : 0u;
    if (dof.units() == DepthUnits::InverseDistance) {
        base.flags |= kGpuInverseDistance;
    }
    base.threshold = params.highlight.threshold;
    base.gain = params.highlight.gain;
    if (depth->view.buffer) {
        const int component = stageGpuMaskViewComponent(depthLayer, depthChannel, depth->view.nComps);
        if ((component < 0) || (component >= depth->view.nComps) || ((depth->view.byteOffset % sizeof(float)) != 0)) {
            return eStatusFailed;
        }
        const gpu::GpuBlurImage image = PassExec::toGpuBlurImage(depth->view);
        if (!gpuFloatIndex(image, depth->view.bounds.x1, depth->view.bounds.y1, depth->view.nComps, &base.depthOffset)) {
            return eStatusFailed;
        }
        base.depthStride = image.strideFloats;
        base.depthComps = (uint32_t)depth->view.nComps;
        base.depthComponent = (uint32_t)component;
        base.flags |= kGpuHasDepth;
        buffers.depth = depth->view.buffer;
    }
    if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
        const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
        base.srcX1 = io.sourceValid.x1;
        base.srcY1 = io.sourceValid.y1;
        base.srcX2 = io.sourceValid.x2;
        base.srcY2 = io.sourceValid.y2;
        base.srcStride = source.strideFloats;
        if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, nComps, &base.srcOffset)) {
            return eStatusFailed;
        }
        buffers.source = io.source.buffer;
    }
    const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
    base.outX1 = io.output.x1;
    base.outY1 = io.output.y1;
    base.outWidth = (uint32_t)io.output.width();
    base.outHeight = (uint32_t)io.output.height();
    base.dstStride = filtered.strideFloats;
    if (!gpuFloatIndex(filtered, io.output.x1, io.output.y1, nComps, &base.dstOffset)) {
        return eStatusFailed;
    }

    if (params.focalPlaneOutput) {
        return dispatchZDefocus(ctx, gpu::GpuKernelId::zdefocus_focal, buffers, base, base.outWidth, base.outHeight) ? eStatusOK : eStatusFailed;
    }

    if (!dispatchZDefocus(ctx, gpu::GpuKernelId::zdefocus_load, buffers, base, base.domWidth, base.domHeight)) {
        return eStatusFailed;
    }
    ctx.computeBarrier();
    // Back to front, each slice laid over the farther ones.
    for (int k = (int)kernels.size() - 1; k >= 0; --k) {
        ZDefocusGpuParams p = base;
        p.slice = (uint32_t)k;
        p.runOffset = runOffsets[k];
        p.runCount = runCounts[k];
        if (k == (int)kernels.size() - 1) {
            p.flags |= kGpuFirst;
        }
        if (kernels[k].isIdentity()) {
            p.flags |= kGpuSharp;
        } else {
            if (!dispatchZDefocus(ctx, gpu::GpuKernelId::zdefocus_blocks, buffers, p, p.nBlocks, p.domHeight)) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
        }
        if (!dispatchZDefocus(ctx, gpu::GpuKernelId::zdefocus_composite, buffers, p, p.outWidth, p.outHeight)) {
            return eStatusFailed;
        }
        ctx.computeBarrier();
    }
    if (!dispatchZDefocus(ctx, gpu::GpuKernelId::zdefocus_finish, buffers, base, base.outWidth, base.outHeight)) {
        return eStatusFailed;
    }

    return eStatusOK;
#else
    Q_UNUSED(io);
    Q_UNUSED(params);
    Q_UNUSED(context);
    Q_UNUSED(depthLayer);
    Q_UNUSED(depthChannel);

    return eStatusFailed;
#endif
} // ZDefocus::recordPassesGpu

bool
ZDefocus::isImageKernel(double time,
                        ViewIdx view) const
{
    const KnobReader reader(time, view);

    return shapeOf(reader.value(_kernelType)) == BokehShape::Image;
}

bool
ZDefocus::resolveKernelChannel(double time,
                               ViewIdx view,
                               ImageLayerDesc* layer,
                               int* channel) const
{
    KnobChannelSelectPtr knob = _kernelChannel.lock();
    if (!knob || knob->isNone() || !getInput(kZDefocusInputKernel)) {
        return false;
    }
    std::list<ImageLayerDesc> present;
    const_cast<ZDefocus*>(this)->getPresentLayers(time, view, kZDefocusInputKernel, &present);

    return knob->resolve(present, layer, channel) && (*channel >= 0);
}

bool
ZDefocus::resolveDepthChannel(double time,
                              ViewIdx view,
                              ImageLayerDesc* layer,
                              int* channel) const
{
    KnobChannelSelectPtr knob = _depth.lock();
    if (!knob || knob->isNone() || !getInput(kZDefocusInputSource)) {
        return false;
    }
    std::list<ImageLayerDesc> present;
    const_cast<ZDefocus*>(this)->getPresentLayers(time, view, kZDefocusInputSource, &present);

    return knob->resolve(present, layer, channel) && (*channel >= 0);
}

PassList
ZDefocus::opPasses(double time,
                   ViewIdx view,
                   const RenderScale& scale) const
{
    const ZDefocusParams params = getParams(time, view);
    if (params.focalPlaneOutput) {
        PassList list = focalPlanePasses(ZDefocusStatePtr());
#ifdef NATRON_HAVE_GPU
        ImageLayerDesc depthLayer;
        int depthChannel = -1;
        const PassContext context = getPassContext(time, view, scale);
        if (fitsGpu(params, context) && resolveDepthChannel(time, view, &depthLayer, &depthChannel)) {
            list.gpu = [params, context, depthLayer, depthChannel](const PassGpuIO& io) -> StatusEnum {
                return recordPassesGpu(io, params, context, depthLayer, depthChannel);
            };
        }
#endif

        return list;
    }
    if ((params.shape.shape == BokehShape::Image) && !getInput(kZDefocusInputKernel)) {
        return PassList();
    }
    const PassContext context = getPassContext(time, view, scale);
    const std::vector<ZDefocusSlice> sliceList = slices(params, context);
    if (sliceList.empty()) {
        return PassList();
    }
    int haloX = 0;
    int haloY = 0;
    halo(params, context, sliceList, &haloX, &haloY);
    const double sigma = 2. * std::max(sliceList.front().radiusX, std::max(sliceList.front().radiusY, std::max(sliceList.back().radiusX, sliceList.back().radiusY))) / 2.4;
    // Every slice gathers its layer and its coverage, at most two reads per kernel row each.
    const std::size_t taps = (std::size_t)(2 * (2 * haloY + 1)) * sliceList.size();
    PassList list = resultPasses(haloX, haloY, sigma, taps, ZDefocusStatePtr(), std::vector<unsigned char>());

#ifdef NATRON_HAVE_GPU
    ImageLayerDesc depthLayer;
    int depthChannel = -1;
    if (fitsGpu(params, context) && resolveDepthChannel(time, view, &depthLayer, &depthChannel)) {
        list.gpu = [params, context, depthLayer, depthChannel](const PassGpuIO& io) -> StatusEnum {
            return recordPassesGpu(io, params, context, depthLayer, depthChannel);
        };
    }
#endif

    return list;
} // ZDefocus::opPasses

bool
ZDefocus::readsGpuInput(int inputNb,
                        const ImageLayerDesc& /*plane*/,
                        double /*time*/,
                        ViewIdx /*view*/) const
{
    return inputNb != kZDefocusInputKernel;
}

std::optional<GpuCostHint>
ZDefocus::gpuCostHint(double time,
                      ViewIdx view,
                      unsigned int mipmapLevel) const
{
    return passListCostHint(opPasses(time, view, RenderScale::fromMipmapLevel(mipmapLevel)));
}

bool
ZDefocus::getGpuExtraInputLayers(int inputNb,
                                 const ImageLayerDesc& plane,
                                 double time,
                                 ViewIdx view,
                                 std::vector<ImageLayerDesc>* layers) const
{
    if (!StagedImageEffect::getGpuExtraInputLayers(inputNb, plane, time, view, layers)) {
        return false;
    }
    ImageLayerDesc layer;
    int channel = -1;
    if ((inputNb == kZDefocusInputSource) && resolveDepthChannel(time, view, &layer, &channel)) {
        layers->push_back(layer);
    }

    return true;
}

void
ZDefocus::addInputLayersNeeded(double time,
                               ViewIdx view,
                               const std::list<ImageLayerDesc>& producedPlanes,
                               EffectInstance::ComponentsNeededMap* comps)
{
    StagedImageEffect::addInputLayersNeeded(time, view, producedPlanes, comps);

    ImageLayerDesc layer;
    int channel = -1;
    if (!resolveDepthChannel(time, view, &layer, &channel)) {
        return;
    }
    std::list<ImageLayerDesc>& needed = (*comps)[kZDefocusInputSource];
    if (std::find(needed.begin(), needed.end(), layer) == needed.end()) {
        needed.push_back(layer);
    }
}

bool
ZDefocus::getInputLayersRead(int inputNb,
                             double time,
                             ViewIdx view,
                             std::list<ImageLayerDesc>* layers)
{
    if ((inputNb != kZDefocusInputKernel) || !isImageKernel(time, view)) {
        return false;
    }
    ImageLayerDesc layer;
    int channel = -1;
    if (!resolveKernelChannel(time, view, &layer, &channel)) {
        return false;
    }
    layers->push_back(layer);

    return true;
}

bool
ZDefocus::isOpIdentity(double time,
                       const RenderScale& scale,
                       const RectI& /*roi*/,
                       ViewIdx view)
{
    return opPasses(time, view, scale).empty();
}

StatusEnum
ZDefocus::readKernelImage(double time,
                          ViewIdx view,
                          const RenderScale& scale,
                          std::vector<float>* values,
                          int* width,
                          int* height,
                          bool* empty)
{
    EffectInstancePtr input = getInput(kZDefocusInputKernel);
    ImageLayerDesc layer;
    int channel = -1;
    ImagePtr image;

    *empty = true;
    if (input && resolveKernelChannel(time, view, &layer, &channel)) {
        RectI roi;
        // A selected layer is read as it is stored, not in the clip's colour layout.
        image = getImage(kZDefocusInputKernel, time, scale, view, NULL, &layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
        if (image && !PassExec::isFloatImage(image)) {
            return eStatusFailed;
        }
    }
    if (!image || (channel >= (int)image->getComponentsCount())) {
        *empty = (bool)input;

        return eStatusOK;
    }
    // The kernel is the input's region of definition, which getRegionsOfInterest() asked for.
    const RectI bounds = image->getBounds();
    RectI frame = bounds;
    RectD rodD;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &rodD, &isProjectFormat) == eStatusOK) {
        const double par = getRenderMetadata(time, view)->getPixelAspectRatio(kZDefocusInputKernel);
        const RectI rodPixel = PassExec::toPixelEnclosing(rodD, scale, par);
        if (!PassExec::isEmptyRect(rodPixel) && ((std::size_t)rodPixel.width() * (std::size_t)rodPixel.height() <= kMaxKernelImagePixels)) {
            frame = rodPixel;
        }
    }
    if (PassExec::isEmptyRect(frame) || ((std::size_t)frame.width() * (std::size_t)frame.height() > kMaxKernelImagePixels)) {
        return eStatusOK;
    }
    *width = frame.width();
    *height = frame.height();
    const int nComps = (int)image->getComponentsCount();
    values->assign((std::size_t)*width * *height, 0.f);
    Image::ReadAccess access(image.get());
    for (int y = 0; y < *height; ++y) {
        PassExec::readChannelRow(image.get(), &access, bounds, nComps, channel, frame.x1, frame.y1 + y, *width, 0.f, values->data() + (std::size_t)y * *width);
    }
    *empty = false;

    return eStatusOK;
} // ZDefocus::readKernelImage

StatusEnum
ZDefocus::buildKernels(const ZDefocusParams& params,
                       const std::vector<ZDefocusSlice>& sliceList,
                       double time,
                       ViewIdx view,
                       const RenderScale& scale,
                       std::vector<std::shared_ptr<const BokehKernel>>* kernels,
                       bool* empty)
{
    std::vector<std::pair<double, double>> radiiList;
    for (const ZDefocusSlice& slice : sliceList) {
        radiiList.push_back(std::make_pair(slice.radiusX, slice.radiusY));
    }
    *empty = false;
    kernels->clear();

    if (params.shape.shape != BokehShape::Image) {
        KernelCache& cache = *_kernelCache;
        const std::lock_guard<std::mutex> lock(cache.mutex);
        const bool hit = !cache.kernels.empty() && (cache.radii == radiiList) && (cache.shape == params.shape);
        if (!hit) {
            cache.kernels.clear();
            for (const std::pair<double, double>& r : radiiList) {
                cache.kernels.push_back(std::make_shared<const BokehKernel>(BokehKernel::gatherKernel(params.shape, r.first, r.second)));
            }
            cache.shape = params.shape;
            cache.radii = radiiList;
        }
        *kernels = cache.kernels;

        return eStatusOK;
    }

    std::vector<float> values;
    int width = 0;
    int height = 0;
    bool noImage = true;
    const StatusEnum status = readKernelImage(time, view, scale, &values, &width, &height, &noImage);
    if (status != eStatusOK) {
        return status;
    }
    for (const std::pair<double, double>& r : radiiList) {
        if (noImage) {
            kernels->push_back(std::make_shared<const BokehKernel>());
            continue;
        }
        bool black = false;
        kernels->push_back(std::make_shared<const BokehKernel>(BokehKernel::fromImage(values.data(), width, height, r.first, r.second, &black)));
        *empty = *empty || black;
    }
    *empty = *empty || (noImage && (bool)getInput(kZDefocusInputKernel));

    return eStatusOK;
} // ZDefocus::buildKernels

StatusEnum
ZDefocus::readLensKernelImage(const RenderActionArgs& args,
                              std::shared_ptr<const DefocusKernelImage>* image)
{
    std::vector<float> read;
    int width = 0;
    int height = 0;
    bool noImage = true;
    image->reset();
    const StatusEnum status = readKernelImage(args.time, args.view, args.mappedScale, &read, &width, &height, &noImage);
    if (status != eStatusOK) {
        return status;
    }
    bool black = true;
    for (float v : read) {
        if (std::isfinite(v) && (v > 0.f)) {
            black = false;
            break;
        }
    }
    const bool empty = (noImage && (bool)getInput(kZDefocusInputKernel)) || (!noImage && black);
    setKernelWarning(empty ? Defocus::kernelEmptyMessage() : std::string());
    if (noImage || black) {
        return eStatusOK;
    }
    std::shared_ptr<DefocusKernelImage> result = std::make_shared<DefocusKernelImage>();
    result->values = std::move(read);
    result->width = width;
    result->height = height;
    *image = result;

    return eStatusOK;
}

namespace {

/**
 * @brief For a Result render with lens aberrations, the layers of state, each sharp only when
 * every channel group's kernel is, and its lens, its kernels per slice, group and cell clip, and
 * a coverage per group; no layers when no slice blurs. image is the Image kernel's, else null.
 **/
void
setUpLens(const ZDefocusParams& params,
          const PassContext& context,
          const std::vector<ZDefocusSlice>& sliceList,
          const RectI& format,
          const RectI& window,
          const std::shared_ptr<BokehRunCache>& cache,
          const std::shared_ptr<const DefocusKernelImage>& image,
          ZDefocusState* state)
{
    const OfxPointD rs = context.scale.toOfxPointD();
    std::shared_ptr<const LensAberration> lens = std::make_shared<const LensAberration>(params.lens, format, context.pixelAspectRatio, rs.x, rs.y);
    const int groups = lens->groupCount();
    // Each slice's radii per group: the group's circle of confusion clamps to Max Size on its own.
    std::vector<std::vector<std::pair<double, double>>> sliceRadii(sliceList.size(), std::vector<std::pair<double, double>>(groups));
    std::vector<std::vector<unsigned char>> groupSharp(sliceList.size(), std::vector<unsigned char>(groups, 1));
    bool anyBlur = false;
    int haloY = 0;
    for (std::size_t k = 0; k < sliceList.size(); ++k) {
        ZDefocusLayer layer;
        for (int g = 0; g < groups; ++g) {
            std::pair<double, double>& r = sliceRadii[k][g];
            ZDefocus::radii(params, context, sliceList[k].coc, lens->groupScale(g), &r.first, &r.second);
            groupSharp[k][g] = BokehKernel::isIdentityRadius(r.first, r.second);
            layer.sharp = layer.sharp && groupSharp[k][g];
            if (!groupSharp[k][g]) {
                haloY = std::max(haloY, BokehKernel::extent(r.second));
            }
        }
        anyBlur = anyBlur || !layer.sharp;
        state->layers.push_back(layer);
    }
    if (!anyBlur) {
        state->layers.clear();

        return;
    }

    if (image) {
        state->kernel = [image, sliceRadii](int k, int group, const BokehClip& clip) -> BokehRunCache::Runs {
            const std::pair<double, double>& r = sliceRadii[k][group];
            const BokehKernel kernel = BokehKernel::fromImage(image->values.data(), image->width, image->height, r.first, r.second, nullptr, clip);

            return std::make_shared<const std::vector<BokehRun>>(kernel.runs());
        };
    } else {
        const BokehShapeParams shape = params.shape;
        state->kernel = [cache, shape, sliceRadii](int k, int group, const BokehClip& clip) -> BokehRunCache::Runs {
            BokehShapeParams clipped = shape;
            clipped.clip = clip;
            const std::pair<double, double>& r = sliceRadii[k][group];

            return cache->runs(clipped, r.first, r.second);
        };
    }
    state->lens = lens;
    state->groupSharp = std::move(groupSharp);
    state->band.cells = lens->cells(window);
    state->band.groups = groups;
    state->coverageGroups = groups;
    // Two table reads per kernel row, for the weights and for each plane, per group.
    state->lensTaps = (std::size_t)(2 * (2 * haloY + 1)) * (std::size_t)groups;
} // setUpLens

} // anon namespace

StatusEnum
ZDefocus::renderOp(const RenderActionArgs& args,
                   const StagePlan& plan)
{
    const PassList full = opPasses(args.time, args.view, args.mappedScale);

    if (full.empty()) {
        return runPassesCpu(*this, plan, args, full);
    }
    const ZDefocusParams params = getParams(args.time, args.view);
    const PassContext context = getPassContext(args.time, args.view, args.mappedScale);
    ZDefocusStatePtr state = std::make_shared<ZDefocusState>();
    std::vector<ZDefocusSlice> sliceList;

    if (params.focalPlaneOutput) {
        state->cocRect = args.roi;
    } else if (params.lens.isActive()) {
        sliceList = slices(params, context);
        std::shared_ptr<const DefocusKernelImage> image;
        if (params.shape.shape == BokehShape::Image) {
            const StatusEnum status = readLensKernelImage(args, &image);
            if ((status != eStatusOK) || !image) {
                return (status != eStatusOK) ? status : runPassesCpu(*this, plan, args, PassList());
            }
        }
        NodeMetadataConstPtr metadata = getRenderMetadata(args.time, args.view);
        const RectI format = metadata ? metadata->getOutputFormat() : RectI();
        setUpLens(params, context, sliceList, format, args.roi, _runCache, image, state.get());
        if (state->layers.empty()) {
            return runPassesCpu(*this, plan, args, PassList());
        }
        state->cocRect = passListSourceRoI(args.roi, full);
        state->highlight = params.highlight;
        state->window = args.roi;
    } else {
        sliceList = slices(params, context);
        std::vector<std::shared_ptr<const BokehKernel>> kernels;
        bool empty = false;
        const StatusEnum status = buildKernels(params, sliceList, args.time, args.view, args.mappedScale, &kernels, &empty);
        if (status != eStatusOK) {
            return status;
        }
        if (params.shape.shape == BokehShape::Image) {
            setKernelWarning(empty ? Defocus::kernelEmptyMessage() : std::string());
        }
        bool anyBlur = false;
        for (const std::shared_ptr<const BokehKernel>& kernel : kernels) {
            ZDefocusLayer layer;
            layer.sharp = !kernel || kernel->isIdentity();
            if (!layer.sharp) {
                layer.runs = kernel->runs();
                anyBlur = true;
            }
            state->layers.push_back(layer);
        }
        if (!anyBlur) {
            return runPassesCpu(*this, plan, args, PassList());
        }
        state->cocRect = passListSourceRoI(args.roi, full);
        state->highlight = params.highlight;
        state->window = args.roi;
    }
    if (PassExec::isEmptyRect(state->cocRect)) {
        return runPassesCpu(*this, plan, args, PassList());
    }

    // The depth under every pixel the passes read, from the Source's selected layer.
    ImageLayerDesc depthLayer;
    int depthChannel = -1;
    ImagePtr depth;
    if (resolveDepthChannel(args.time, args.view, &depthLayer, &depthChannel)) {
        RectI roi;
        depth = getImage(kZDefocusInputSource, args.time, args.mappedScale, args.view, NULL, &depthLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
        if (depth && !PassExec::isFloatImage(depth)) {
            return eStatusFailed;
        }
        if (depth && (depthChannel >= (int)depth->getComponentsCount())) {
            depth.reset();
        }
    }
    const RectI cocRect = state->cocRect;
    const int cocWidth = cocRect.width();
    const int cocHeight = cocRect.height();
    const std::size_t nPixels = (std::size_t)cocWidth * (std::size_t)cocHeight;
    state->slice.assign(nPixels, 0);
    state->frac.assign(params.focalPlaneOutput ? 0 : nPixels, 0.f);
    std::vector<unsigned char> used(sliceList.size(), 0);
    std::vector<double> centres;
    for (const ZDefocusSlice& slice : sliceList) {
        centres.push_back(slice.coc);
    }

    const DepthOfField dof(params.dof);
    const RectI depthBounds = depth ? depth->getBounds() : RectI();
    const int depthComps = depth ? (int)depth->getComponentsCount() : 0;
    std::unique_ptr<Image::ReadAccess> access;
    if (depth) {
        access.reset(new Image::ReadAccess(depth.get()));
    }
    std::mutex usedMutex;
    RenderCancellation cancel(this);
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    ZDefocusState* s = state.get();
    PassExec::forEachLineChunk(cocHeight, (std::size_t)cocWidth, nThreads, cancel, [&](int first, int end) {
        std::vector<float> row((std::size_t)cocWidth, 0.f);
        std::vector<unsigned char> chunkUsed(used.size(), 0);
        for (int r = first; r < end; ++r) {
            if (depth) {
                PassExec::readChannelRow(depth.get(), access.get(), depthBounds, depthComps, depthChannel, cocRect.x1, cocRect.y1 + r, cocWidth, 0.f, row.data());
            }
            const std::size_t base = (std::size_t)r * cocWidth;
            for (int x = 0; x < cocWidth; ++x) {
                if (params.focalPlaneOutput) {
                    // At full size, whatever the scale: in focus below half a pixel of radius.
                    const double coc = dof.coc(row[x]);
                    s->slice[base + x] = (std::abs(coc) < 1.) ? kClassInFocus : ((coc < 0.) ? kClassFront : kClassBehind);
                    continue;
                }
                const double coc = pixelCoc(params, context, dof, row[x]);
                // The slice at or below coc and the weight of the next one, a tent between the two.
                std::size_t k = (std::size_t)(std::upper_bound(centres.begin(), centres.end(), coc) - centres.begin());
                float t = 0.f;
                if (k == 0) {
                    k = 0;
                } else if (k >= centres.size()) {
                    k = centres.size() - 1;
                } else {
                    --k;
                    t = (float)((coc - centres[k]) / (centres[k + 1] - centres[k]));
                }
                s->slice[base + x] = (uint16_t)k;
                s->frac[base + x] = t;
                chunkUsed[k] = chunkUsed[k] || (t < 1.f);
                if ((t > 0.f) && (k + 1 < chunkUsed.size())) {
                    chunkUsed[k + 1] = 1;
                }
            }
        }
        const std::lock_guard<std::mutex> lock(usedMutex);
        for (std::size_t k = 0; k < used.size(); ++k) {
            used[k] = used[k] || chunkUsed[k];
        }
    });
    access.reset();
    if (cancel.check()) {
        return eStatusOK;
    }

    if (params.focalPlaneOutput) {
        return runPassesCpu(*this, plan, args, focalPlanePasses(state));
    }
    const PassExtent extent = passListExtent(full);
    const Pass& first = full.passes.front();

    return runPassesCpu(*this, plan, args, resultPasses(extent.haloX, extent.haloY, first.sigma, first.tapsPerSample, state, used));
} // ZDefocus::renderOp

void
ZDefocus::getRegionsOfInterest(double time,
                               const RenderScale& scale,
                               const RectD& outputRoD,
                               const RectD& renderWindow,
                               ViewIdx view,
                               RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(kZDefocusInputKernel);
    if (!input || !isImageKernel(time, view)) {
        return;
    }
    // The whole Kernel image, whatever the window.
    RectD rod;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &rod, &isProjectFormat) != eStatusOK) {
        return;
    }
    // The Kernel may be the same effect as the source or the mask, which then needs both regions.
    RoIMap::iterator found = ret->find(input);
    if (found != ret->end()) {
        found->second.merge(rod);
    } else {
        ret->insert(std::make_pair(input, rod));
    }
}

void
ZDefocus::onEffectCreated(bool /*mayCreateFileDialog*/,
                          const CreateNodeArgs& /*args*/)
{
    refreshKernelWarning();
}

void
ZDefocus::onInputChanged(int /*inputNo*/)
{
    refreshKernelWarning();
}

bool
ZDefocus::knobChanged(KnobI* k,
                      ValueChangedReasonEnum reason,
                      ViewSpec /*view*/,
                      double time,
                      bool originatedFromMainThread)
{
    KnobChoicePtr type = _kernelType.lock();
    if (type && (k == type.get())) {
        refreshKernelWarning();

        return true;
    }
    KnobDoublePtr point = _focalPoint.lock();
    if (point && (k == point.get())) {
        // Only a move the user makes refocuses; restoring a project or an undo of the
        // Focal Plane must not.
        if (originatedFromMainThread && ((reason == eValueChangedReasonUserEdited) || (reason == eValueChangedReasonNatronGuiEdited))) {
            sampleFocalPoint(time);
        }

        return true;
    }

    return false;
}

void
ZDefocus::sampleFocalPoint(double time)
{
    EffectInstancePtr input = getInput(kZDefocusInputSource);
    KnobDoublePtr point = _focalPoint.lock();
    KnobDoublePtr focalPlane = _focalPlane.lock();
    const ViewIdx view(0);
    ImageLayerDesc layer;
    int channel = -1;

    if (!input || !point || !focalPlane || !resolveDepthChannel(time, view, &layer, &channel)) {
        return;
    }
    NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    const double par = metadata ? metadata->getPixelAspectRatio(kZDefocusInputSource) : 1.;
    const int x = (int)std::floor(point->getValue(0) / ((par > 0.) ? par : 1.));
    const int y = (int)std::floor(point->getValue(1));
    RectI rendered;
    ImagePtr image = renderNodeImage(input->getNode(), time, view, 0, RectI(x, y, x + 1, y + 1), layer, &rendered);
    if (!image || PassExec::isEmptyRect(rendered) || !PassExec::isFloatImage(image) || (channel >= (int)image->getComponentsCount())) {
        return;
    }
    float value = 0.f;
    {
        const RectI bounds = image->getBounds();
        Image::ReadAccess access(image.get());
        PassExec::readChannelRow(image.get(), &access, bounds, (int)image->getComponentsCount(), channel, x, y, 1, 0.f, &value);
    }
    if (std::isfinite(value)) {
        focalPlane->setValue((double)value);
    }
}

void
ZDefocus::setKernelWarning(const std::string& message)
{
    const NodePtr node = getNode();

    if (!node) {
        return;
    }
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if (!message.empty()) {
        if (current != QString::fromUtf8(message.c_str())) {
            node->setPersistentMessage(eMessageTypeWarning, message);
        }

        return;
    }
    // Only our own warnings are taken back: anything posted since has replaced them.
    if ((current == QString::fromUtf8(Defocus::kernelNotConnectedMessage().c_str())) || (current == QString::fromUtf8(Defocus::kernelEmptyMessage().c_str()))) {
        node->clearPersistentMessage(false);
    }
}

void
ZDefocus::refreshKernelWarning()
{
    KnobChoicePtr type = _kernelType.lock();
    const bool image = type && (shapeOf(type->getValue()) == BokehShape::Image);

    setKernelWarning((image && !getInput(kZDefocusInputKernel)) ? Defocus::kernelNotConnectedMessage() : std::string());
}

NATRON_NAMESPACE_EXIT
