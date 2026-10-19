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

#ifndef NATRON_ENGINE_VIEWERGPUPROCESS_H
#define NATRON_ENGINE_VIEWERGPUPROCESS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "Global/Enums.h"

#include "Engine/Gpu/GlInterop.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/OcioVulkanShader.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What the viewer process does to one texture rectangle, as RenderViewerArgs tells the CPU
 * path. The matte overlay reads its matte from the input plane itself; a matte in another image
 * is for the CPU path.
 **/
struct ViewerGpuProcessParams {
    // In pixel coordinates of the input; its size must be the frame's.
    RectI rect;
    DisplayChannelsEnum channels = eDisplayChannelsRGB;
    int alphaChannelIndex = -1;
    bool matteOverlay = false;

    // Gain, offset, gamma and the display processor apply to 8-bit frames only: a float frame
    // leaves them to the GL shader, as the CPU path does.
    double gain = 1.;
    double offset = 0.;
    double gamma = 1.;
    ProjectColorManagement::DisplayProcessorPtr displayProcessor;
};

/**
 * @brief The viewer process (channel selection, luminance, matte overlay and, for 8-bit frames,
 * gain, offset, display transform, gamma, dither and ARGB packing) as one compute dispatch.
 *
 * One pipeline is compiled per output format and display processor, with every other parameter
 * pushed per dispatch. Thread safe. Every kernel, uniform block and LUT texture it hands to
 * recorded work stays alive until the object is destroyed, which must wait for that work.
 **/
class ViewerGpuProcess
{
public:
    static constexpr unsigned kInputBinding = 24;
    static constexpr unsigned kOutputBinding = 25;

    explicit ViewerGpuProcess(gpu::GpuRuntime& runtime);
    ~ViewerGpuProcess();

    ViewerGpuProcess(const ViewerGpuProcess&) = delete;
    ViewerGpuProcess& operator=(const ViewerGpuProcess&) = delete;

    /**
     * @brief Compiles the pipeline of \p format with \p processor (ignored for float frames) and
     * uploads its LUTs, unless an earlier call did. A failed status means the CPU path must
     * render.
     **/
    gpu::GpuStatus prepare(const ProjectColorManagement::DisplayProcessorPtr& processor,
                           gpu::GlPixelFormat format);

    /**
     * @brief Records the dispatch that writes \p frame into \p outBuffer, tightly packed from its
     * offset 0 and from the bottom row of params.rect. Pixels of the rect outside input.bounds read
     * as no pixel at all, as the CPU path's do. Prepares the pipeline first when needed. Fails,
     * recording nothing, on a pipeline that does not compile, a rect of another size than the
     * frame, or an input without a buffer or with 0 or more than 4 components.
     **/
    gpu::GpuStatus record(VkCommandBuffer cmd,
                          const GpuImageView& input,
                          VkBuffer outBuffer,
                          const gpu::GlFrame& frame,
                          const ViewerGpuProcessParams& params);

    /**
     * @brief Recycles the descriptor sets of the kernels, needed on devices without push
     * descriptors. Only once no recorded work is pending.
     **/
    void releaseDescriptors();

    /**
     * @brief The kernel's GLSL for \p format. \p ocioGlsl must declare vec4 OCIODisplay(vec4) with
     * its resources in set 0 below kInputBinding; empty means no display transform. It is unused
     * for float frames.
     **/
    static std::string kernelSource(gpu::GlPixelFormat format, const std::string& ocioGlsl);

    // Compiles kernelSource() through the runtime's pipeline cache.
    static gpu::GpuStatus compile(gpu::GpuRuntime& runtime,
                                  gpu::GlPixelFormat format,
                                  const std::string& ocioGlsl,
                                  std::shared_ptr<gpu::GpuKernel>& out);

private:
    struct Pipeline;

    gpu::GpuStatus pipelineFor(const ProjectColorManagement::DisplayProcessorPtr& processor,
                               gpu::GlPixelFormat format,
                               std::shared_ptr<const Pipeline>& out);

    gpu::GpuStatus buildPipeline(const ProjectColorManagement::DisplayProcessorPtr& processor,
                                 gpu::GlPixelFormat format,
                                 bool withOcio,
                                 std::shared_ptr<const Pipeline>& out);

    gpu::GpuRuntime& _runtime;
    std::mutex _mutex;
    OcioVulkanShader::Resources _resources;
    std::map<std::string, std::shared_ptr<const Pipeline> > _pipelines;

    // A pipeline that cannot be built fails again without a recompile on every frame the CPU then
    // renders; a device that ran out of memory may succeed later, so its failures are not kept.
    std::map<std::string, gpu::GpuStatus> _failures;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // NATRON_ENGINE_VIEWERGPUPROCESS_H
