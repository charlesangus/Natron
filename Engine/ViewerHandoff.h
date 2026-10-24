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

#ifndef NATRON_ENGINE_VIEWERHANDOFF_H
#define NATRON_ENGINE_VIEWERHANDOFF_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Global/Enums.h"

#include "Engine/GpuContext.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief How a device root reaches the viewer's GL texture, best first. Cpu means the root must be
 * downloaded and run through the CPU viewer process.
 **/
enum class ViewerHandoffRung {
    ZeroCopy,
    HostSync,
    Readback,
    Cpu,
};

// "zerocopy", "hostsync", "readback" or "cpu", the values NATRON_VIEWER_HANDOFF accepts.
const char* toString(ViewerHandoffRung rung);

enum class ViewerHandoffFormat {
    // GL_RGBA / GL_FLOAT into an RGBA32F texture.
    Rgba32F,
    // GL_BGRA / GL_UNSIGNED_INT_8_8_8_8_REV into an RGBA8 texture.
    Bgra8,
};

/**
 * @brief The texture rectangle a present() writes: width x height texels at (x, y) of level 0.
 **/
struct ViewerHandoffFrame {
    unsigned width = 0;
    unsigned height = 0;
    ViewerHandoffFormat format = ViewerHandoffFormat::Bgra8;
    int x = 0;
    int y = 0;

    std::size_t bytes() const;
};

/**
 * @brief The viewer process of one frame, as ViewerGpuProcessParams has it. rect must be the
 * frame's size; gain, offset, gamma and the display processor apply to Bgra8 frames only.
 **/
struct ViewerHandoffParams {
    RectI rect;
    DisplayChannelsEnum channels = eDisplayChannelsRGB;
    int alphaChannelIndex = -1;
    bool matteOverlay = false;
    double gain = 1.;
    double offset = 0.;
    double gamma = 1.;
    ProjectColorManagement::DisplayProcessorPtr displayProcessor;

    // Nonzero asks for a copy of the display buffer in host memory, collected later by
    // consumeCompletedReadbacks() under this token. The frame is shown without waiting for it.
    std::uint64_t readbackToken = 0;
};

/**
 * @brief The display buffer of a present() whose params carried a readbackToken: the texels the
 * texture received, tightly packed rows, size == frame.bytes(), lent for one call of the
 * ViewerHandoffReadbackFn it is passed to.
 **/
struct ViewerHandoffReadback {
    std::uint64_t token = 0;
    ViewerHandoffFrame frame;
    const unsigned char* data = nullptr;
    std::size_t size = 0;
};

/**
 * @brief Work on one readback, run on the global thread pool, or on the GL thread when present() needs its slot
 * before the pool started it. It must not need the GL context, the GL thread or the ViewerHandoff.
 **/
using ViewerHandoffReadbackFn = std::function<void(const ViewerHandoffReadback&)>;

/**
 * @brief Called on the GL thread with the token of a completed readback, returns the work to run on it, or an empty
 * function to drop it.
 **/
using ViewerHandoffClaimFn = std::function<ViewerHandoffReadbackFn(std::uint64_t token)>;

/**
 * @brief The display processor a precompile() compiles for, called on the worker that compiles it, since building an
 * OCIO processor can take long enough to be felt on the GL thread. It must not need the GL thread or the ViewerHandoff.
 **/
using ViewerHandoffProcessorFn = std::function<ProjectColorManagement::DisplayProcessorPtr()>;

/**
 * @brief Success, or a refusal whose reason completes "viewer handoff: " for
 * RenderStats::incLegacyFallbacks(). A refused frame must take the CPU path.
 **/
struct ViewerHandoffResult {
    bool ok = false;
    std::string reason;

    explicit operator bool() const { return ok; }
};

/**
 * @brief Hands GPU-rendered viewer roots to the viewer's GL textures: runs the viewer process on
 * the device and uploads its output through GlInterop, one ring of slots per texture index (the
 * A and B inputs of a wipe).
 *
 * The rung is the best that both the GL context and the device reach, ZeroCopy, then HostSync,
 * then Readback; NATRON_VIEWER_HANDOFF names a rung to start the descent from, and "cpu" turns the
 * hand-off off. A GpuContext::holdRuntime() token is held exactly while device resources exist,
 * which a precompile() still running when they are released keeps until it finishes. Once the runtime is lost they are all released and the rung reads Cpu; maintain() builds them
 * again once the context has recovered.
 *
 * Threads: rung() and prepare() are safe from any thread. Every other call, destruction included,
 * runs on the thread of the GL context, with that context current. The work consumeCompletedReadbacks() is handed
 * runs on the global thread pool while its slot is held: present() waits for it before producing into that slot
 * again, and rebuilding a ring, losing the device and release() cancel it if it has not started and wait for it if
 * it has.
 **/
class ViewerHandoff
{
public:
    using GlProcLoader = std::function<void*(const char*)>;

    static constexpr int kTextureCount = 2;

    // Null or unavailable leaves the rung at Cpu.
    explicit ViewerHandoff(GpuContext* context);

    ~ViewerHandoff();

    ViewerHandoff(const ViewerHandoff&) = delete;
    ViewerHandoff& operator=(const ViewerHandoff&) = delete;

    /**
     * @brief Probes the current GL context with \p loader, which must resolve its entry points for
     * as long as this object uses them, and picks the rung. Replaces anything an earlier create()
     * built.
     **/
    ViewerHandoffRung create(const GlProcLoader& loader);

    /**
     * @brief Cpu also while the GPU context is unavailable, even before maintain() noticed.
     **/
    ViewerHandoffRung rung() const;

    /**
     * @brief Why the rung is below ZeroCopy; empty at ZeroCopy.
     **/
    std::string rungReason() const;

    /**
     * @brief Releases the device resources once the runtime is lost and builds them again once the
     * context is available, for a viewer to call every time it draws. Returns whether the rung is
     * above Cpu.
     **/
    bool maintain();

    /**
     * @brief Compiles the viewer process of \p processor for \p format ahead of present(), so the GL
     * thread need not.
     **/
    ViewerHandoffResult prepare(const ProjectColorManagement::DisplayProcessorPtr& processor,
                                ViewerHandoffFormat format);

    /**
     * @brief Starts compiling the viewer process of \p format, with the display processor that \p processor returns
     * for Bgra8 frames, on the global thread pool, so that the first frame finds it compiled. A prepare() or present()
     * that needs it meanwhile waits for this compile rather than starting another. \p key names the request, and a
     * call with the key and format of the last one does nothing. The last request is compiled again whenever the
     * device is built again, as after a loss, until release().
     *
     * Never waits for the compile. release() and a device loss drop one the pool has not started yet; one already
     * running keeps the device it compiles on, and so the runtime's recovery, only until it finishes.
     **/
    void precompile(const std::string& key,
                    ViewerHandoffFormat format,
                    const ViewerHandoffProcessorFn& processor);

    /**
     * @brief Builds the ring of \p textureIndex now, with slots of at least \p bytes, rather than at its first
     * present(), and builds it again at that size after a device loss, until release(). A ring holding a staged frame
     * grows once it holds none, and no ring shrinks. A ring that cannot be built at that size is left to present().
     **/
    ViewerHandoffResult reserve(int textureIndex,
                                std::size_t bytes);

    /**
     * @brief Runs the viewer process of \p root into a slot of the ring of \p textureIndex and
     * uploads it into \p frame's rectangle of \p glTexture, which must already be large enough.
     * The root stays pinned until the device has read it. A frame over the slot capacity is
     * refused, and the slots grow before the next present().
     **/
    ViewerHandoffResult present(int textureIndex,
                                const GpuDeviceImagePtr& root,
                                const ViewerHandoffParams& params,
                                unsigned int glTexture,
                                const ViewerHandoffFrame& frame);

    /**
     * @brief Runs the viewer process of \p root into a slot of the ring of \p textureIndex as present() does, without
     * uploading it, so that the device computes it while the GL thread does other work. *staged receives the id that
     * presentStaged() or discardStaged() then takes; a ring holds at most kSlots staged frames, and present() needs a
     * slot free of them. A frame the slots must first grow for is refused, so that no staged frame is lost to the
     * growth.
     **/
    ViewerHandoffResult stage(int textureIndex,
                              const GpuDeviceImagePtr& root,
                              const ViewerHandoffParams& params,
                              const ViewerHandoffFrame& frame,
                              std::uint64_t* staged);

    /**
     * @brief Whether the device is still computing the staged frame, without waiting; false once it has finished or
     * was lost, as presentStaged() then tells.
     **/
    bool stagedComputing(int textureIndex,
                         std::uint64_t staged);

    /**
     * @brief Uploads the staged frame into \p frame's rectangle of \p glTexture, waiting for the device if it has not
     * finished. \p frame must be the one stage() was given. A refusal, such as for a frame lost when its ring was
     * rebuilt or the device was, leaves the root unpinned, so that the frame can take the CPU path.
     **/
    ViewerHandoffResult presentStaged(int textureIndex,
                                      std::uint64_t staged,
                                      unsigned int glTexture,
                                      const ViewerHandoffFrame& frame);

    /**
     * @brief Frees the slot of a staged frame that will not be shown. Its readback, if it asked for one, still
     * completes. Nothing happens for a frame already lost.
     **/
    void discardStaged(int textureIndex,
                       std::uint64_t staged);

    /**
     * @brief Passes \p claim the token of each readback whose device copy has finished since the last
     * call, without waiting for the others, and starts the work it returns on the global thread pool.
     * A readback is lost when its ring is rebuilt or the device is, and so is its work if it has not
     * started by then.
     *
     * The work reads the texels in place from the slot's mapped memory, which stays valid until it
     * returns. Any present() or ensureCapacity() \p claim itself makes is refused.
     **/
    void consumeCompletedReadbacks(const ViewerHandoffClaimFn& claim);

    /**
     * @brief Grows every slot to hold \p bytes, waiting for the frames they hold.
     **/
    ViewerHandoffResult ensureCapacity(std::size_t bytes);

    // The slot capacity of the ring of textureIndex; 0 before its first present() or reserve().
    std::size_t capacity(int textureIndex) const;

    /**
     * @brief Releases every GL object, device resource and pinned root, after cancelling or waiting for
     * the readback work still pending, and forgets the precompile() and reserve() requests. The rung is
     * Cpu until the next create().
     **/
    void release();

private:
    struct Imp;
    std::unique_ptr<Imp> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // NATRON_ENGINE_VIEWERHANDOFF_H
