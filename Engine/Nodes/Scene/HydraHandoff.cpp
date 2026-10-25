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

#include "HydraHandoff.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuContext.h"

NATRON_NAMESPACE_ENTER

namespace {
// The bytes of the buffers a ladder is tried with.
const VkDeviceSize kProbeBytes = 4096;

const VkBufferUsageFlags kSlotUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

std::mutex gForcedMutex;
std::optional<gpu::GlHandoffPath> gForcedPath;

// The rung the ladder starts at: the test hook's, else NATRON_USDRENDER_HANDOFF's, else ZeroCopy.
gpu::GlHandoffPath
firstPath(bool* forced)
{
    {
        std::lock_guard<std::mutex> l(gForcedMutex);
        if (gForcedPath) {
            *forced = true;

            return *gForcedPath;
        }
    }
    const std::optional<gpu::GlHandoffPath> fromEnv = gpu::parseOverride(std::getenv("NATRON_USDRENDER_HANDOFF"));
    *forced = (bool)fromEnv;

    return fromEnv ? *fromEnv : gpu::GlHandoffPath::ZeroCopy;
}

gpu::GlHandoffPath
nextPath(gpu::GlHandoffPath path)
{
    return (path == gpu::GlHandoffPath::ZeroCopy) ? gpu::GlHandoffPath::ZeroCopyHostSync : gpu::GlHandoffPath::Readback;
}

// Exported Vulkan memory imported into GL as a buffer of capacity bytes.
struct Slot {
    gpu::GpuBuffer buffer;
    VkDeviceSize capacity = 0;
    unsigned int glMemory = 0;
    unsigned int glBuffer = 0;
};
} // anon namespace

/**
 * What the hand-off holds for one runtime of the context: the rung and the GL entry points, which
 * are used on the render thread only. Slots are made for each hand-off and destroyed when its
 * copy is done, not kept: kept ones would hold device memory across a loss, which recoverIfLost()
 * cannot repair while anything of the runtime is alive.
 **/
struct HydraHandoff::State {
    gpu::GpuRuntime* runtime = nullptr;
    std::uint64_t generation = 0;
    gpu::GlHandoffPath first = gpu::GlHandoffPath::ZeroCopy;
    std::atomic<gpu::GlHandoffPath> path { gpu::GlHandoffPath::Readback };
    std::unique_ptr<gpu::GlExternal> gl;

    mutable std::mutex mutex;
    std::string reason;

    void setReason(const std::string& why)
    {
        std::lock_guard<std::mutex> l(mutex);
        reason = why;
    }

    // Steps below the rung that failed; nothing if the rung has already changed.
    void stepDown(gpu::GlHandoffPath failed,
                  const std::string& why)
    {
        gpu::GlHandoffPath expected = failed;
        if (path.compare_exchange_strong(expected, nextPath(failed))) {
            setReason(std::string(gpu::overrideName(failed)) + " failed: " + why);
        }
    }

    // Gives the runtime up: renders read back until the ladder is built for a live runtime.
    void abandon(const std::string& why)
    {
        path.store(gpu::GlHandoffPath::Readback);
        setReason(why);
    }

    // Render thread only.
    void destroySlotGl(Slot& slot)
    {
        if (gl) {
            gl->deleteBuffer(&slot.glMemory, &slot.glBuffer);
        }
    }

    // A slot of at least bytes, imported into GL. Render thread only, with the runtime held.
    gpu::GpuStatus makeSlot(VkDeviceSize bytes,
                            std::unique_ptr<Slot>* out)
    {
        std::unique_ptr<Slot> slot(new Slot);
        slot->capacity = bytes;
        gpu::GpuDevice& device = runtime->device();
        if (gpu::GpuStatus s = gpu::GpuBuffer::createExportable(device, bytes + gpu::GlExternal::kImportSlackBytes, kSlotUsage, slot->buffer); !s) {
            return s;
        }
        if (gpu::GpuStatus s = gl->importBuffer(device, slot->buffer, bytes, &slot->glMemory, &slot->glBuffer); !s) {
            destroySlotGl(*slot);

            return s;
        }
        *out = std::move(slot);

        return {};
    }
};

struct HydraHandoff::Implementation {
    GpuContext* const fixedContext;
    mutable std::mutex mutex;
    std::shared_ptr<State> state;
    std::string reason;

    explicit Implementation(GpuContext* context)
        : fixedContext(context)
    {
    }

    GpuContext* context() const
    {
        if (fixedContext) {
            return fixedContext;
        }

        return appPTR ? appPTR->getGpuContext() : nullptr;
    }

    void setReason(const std::string& why)
    {
        std::lock_guard<std::mutex> l(mutex);
        reason = why;
    }

    // The state of the context's live runtime, built if need be; null while there is none.
    std::shared_ptr<State> current();

    std::shared_ptr<State> build(gpu::GpuRuntime* runtime,
                                 gpu::GlHandoffPath first,
                                 bool forced);
};

std::shared_ptr<HydraHandoff::State>
HydraHandoff::Implementation::current()
{
    GpuContext* gpuContext = context();
    if (!gpuContext || !gpuContext->isAvailable()) {
        std::shared_ptr<State> dropped;
        {
            std::lock_guard<std::mutex> l(mutex);
            dropped.swap(state);
            reason = "GPU compute unavailable";
        }
        if (dropped) {
            dropped->abandon("GPU compute unavailable");
        }

        return std::shared_ptr<State>();
    }
    const std::shared_ptr<void> token = gpuContext->holdRuntime();
    gpu::GpuRuntime* runtime = gpuContext->runtime();
    bool forced = false;
    const gpu::GlHandoffPath first = firstPath(&forced);
    std::shared_ptr<State> dropped;
    {
        std::lock_guard<std::mutex> l(mutex);
        if (!token || !runtime || runtime->isLost()) {
            dropped.swap(state);
            reason = "GPU compute lost";
        } else if (state && (state->runtime == runtime) && (state->generation == runtime->generation()) && (state->first == first)) {
            return state;
        } else {
            dropped.swap(state);
        }
    }
    if (dropped) {
        dropped->abandon("GPU compute lost or replaced");
    }
    if (!token || !runtime || runtime->isLost()) {
        return std::shared_ptr<State>();
    }

    // Built outside the lock: it waits for the render thread, which may be finishing a render that
    // asks for the rung.
    std::shared_ptr<State> built = build(runtime, first, forced);
    {
        std::lock_guard<std::mutex> l(mutex);
        if (!state) {
            state = built;
        }
        built = state;
    }

    return built;
} // HydraHandoff::Implementation::current

std::shared_ptr<HydraHandoff::State>
HydraHandoff::Implementation::build(gpu::GpuRuntime* runtime,
                                    gpu::GlHandoffPath first,
                                    bool forced)
{
    std::shared_ptr<State> built = std::make_shared<State>();
    built->runtime = runtime;
    built->generation = runtime->generation();
    built->first = first;
    const std::string prefix = forced ? std::string("NATRON_USDRENDER_HANDOFF=") + gpu::overrideName(first) : std::string();
    if (first == gpu::GlHandoffPath::Readback) {
        built->setReason(prefix);

        return built;
    }
    if (!HydraRender::packsOnGpu()) {
        built->setReason("the GL context cannot pack Storm's AOVs");

        return built;
    }
    std::string why;
    bool lost = false;
    std::optional<gpu::GlHandoffPath> picked;
    std::string error;
    const bool ran = HydraRender::withContext([&](const std::function<void*(const char*)>& getProcAddress) {
        gpu::GpuDevice& device = runtime->device();
        gpu::GlInteropCaps caps;
        if (gpu::GpuStatus s = gpu::GlInterop::probe(device, getProcAddress, caps); !s) {
            lost = gpu::isDeviceLost(s);
            why = "GL interop probe failed: " + s.message;

            return;
        }
        if (gpu::GpuStatus s = gpu::GlExternal::load(getProcAddress, built->gl); !s) {
            why = s.message;

            return;
        }
        const gpu::GlHandoffChoice choice = gpu::chooseHandoffPath(caps, first, [&](gpu::GlHandoffPath path) -> gpu::GpuStatus {
            if (path == gpu::GlHandoffPath::Readback) {
                return {};
            }
            std::unique_ptr<Slot> slot;
            gpu::GpuStatus s = built->makeSlot(kProbeBytes, &slot);
            VkSemaphore semaphore = VK_NULL_HANDLE;
            unsigned int glSemaphore = 0;
            if (s && (path == gpu::GlHandoffPath::ZeroCopy)) {
                s = gpu::createExportableSemaphore(device, &semaphore);
                if (s) {
                    s = built->gl->importSemaphore(device, semaphore, &glSemaphore);
                }
            }
            built->gl->deleteSemaphore(&glSemaphore);
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device.device(), semaphore, nullptr);
            }
            if (slot) {
                built->destroySlotGl(*slot);
            }

            return s;
        });
        lost = choice.lost;
        why = choice.reasons;
        picked = choice.path;
    },
                                              &error);
    if (!ran) {
        why = error;
    }
    if (lost) {
        built->setReason("device lost while building the GL interop");

        return built;
    }
    if (!picked) {
        picked = gpu::GlHandoffPath::Readback;
    }
    built->path.store(*picked);
    built->setReason(prefix + ((prefix.empty() || why.empty()) ? "" : "; ") + why);

    return built;
} // HydraHandoff::Implementation::build

HydraHandoff::HydraHandoff(GpuContext* context)
    : _imp(std::make_shared<Implementation>(context))
{
}

HydraHandoff::~HydraHandoff()
{
    std::shared_ptr<State> dropped;
    {
        std::lock_guard<std::mutex> l(_imp->mutex);
        dropped.swap(_imp->state);
    }
    if (dropped) {
        dropped->abandon("the hand-off is gone");
    }
}

HydraHandoff&
HydraHandoff::instance()
{
    static HydraHandoff* handoff = new HydraHandoff(nullptr);

    return *handoff;
}

gpu::GlHandoffPath
HydraHandoff::path()
{
    const std::shared_ptr<State> state = _imp->current();

    return state ? state->path.load() : gpu::GlHandoffPath::Readback;
}

std::string
HydraHandoff::reason() const
{
    std::shared_ptr<State> state;
    {
        std::lock_guard<std::mutex> l(_imp->mutex);
        if (!_imp->state) {
            return _imp->reason;
        }
        state = _imp->state;
    }
    std::lock_guard<std::mutex> l(state->mutex);

    return state->reason;
}

void
HydraHandoff::setForcedPathForTests(std::optional<gpu::GlHandoffPath> path)
{
    std::lock_guard<std::mutex> l(gForcedMutex);

    gForcedPath = path;
}

namespace {
/**
 * One hand-off's slot and semaphores, held by the record context until its work is done. Then the
 * render thread takes them back, after glFinish() so that a semaphore the device never waited for
 * (a context that was never submitted) has no GL signal pending when it is destroyed.
 **/
class Use {
public:
    Use(GpuContext* context,
        std::shared_ptr<HydraHandoff::State> state,
        std::unique_ptr<Slot> slot,
        VkSemaphore semaphore,
        unsigned int glSemaphore)
        : _context(context)
        , _state(std::move(state))
        , _slot(std::move(slot))
        , _semaphore(semaphore)
        , _glSemaphore(glSemaphore)
    {
    }

    ~Use()
    {
        GpuContext* context = _context;
        std::shared_ptr<HydraHandoff::State> state = _state;
        std::shared_ptr<Slot> slot(std::move(_slot));
        VkSemaphore semaphore = _semaphore;
        unsigned int glSemaphore = _glSemaphore;
        HydraRender::postToContext([context, state, slot, semaphore, glSemaphore](const std::function<void*(const char*)>* getProcAddress) mutable {
            if (getProcAddress && state->gl) {
                (void)state->gl->finish();
                state->gl->deleteSemaphore(&glSemaphore);
                if (slot) {
                    state->destroySlotGl(*slot);
                }
            }
            // Held while the Vulkan objects go, so that the device cannot be recreated under them. Once it
            // has been, or while work it abandoned may still use them, they are parked instead.
            const std::shared_ptr<void> token = context ? context->holdRuntime() : std::shared_ptr<void>();
            const bool sameDevice = token && (context->runtime() == state->runtime) && (state->runtime->generation() == state->generation) && !state->runtime->device().hasAbandonedWork();
            if (sameDevice) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(state->runtime->device().device(), semaphore, nullptr);
                }
                slot.reset();
            } else if (slot) {
                gpu::GpuDevice::parkForever(slot);
            }
        });
    }

private:
    GpuContext* _context;
    std::shared_ptr<HydraHandoff::State> _state;
    std::unique_ptr<Slot> _slot;
    VkSemaphore _semaphore;
    unsigned int _glSemaphore;
};
} // anon namespace

bool
HydraHandoff::record(const HydraRender::Request& request,
                     const HydraRender::PlaneWindow& window,
                     gpu::GpuRecordContext& context,
                     const GpuImageView& output,
                     const RectI& outputRect,
                     std::string* error)
{
    const std::shared_ptr<State> state = _imp->current();
    const gpu::GlHandoffPath path = state ? state->path.load() : gpu::GlHandoffPath::Readback;
    if (path == gpu::GlHandoffPath::Readback) {
        *error = "USDRender hands its pixels over by readback.";

        return false;
    }
    const int width = window.x2 - window.x1;
    const int height = window.y2 - window.y1;
    if (!output.buffer || (width != outputRect.width()) || (height != outputRect.height()) || (window.nComps != output.nComps) || !output.bounds.contains(outputRect)) {
        *error = "USDRender's GPU output does not match the window it renders.";

        return false;
    }
    if ((width <= 0) || (height <= 0)) {
        return true;
    }
    GpuContext* gpuContext = _imp->context();
    const std::shared_ptr<void> token = gpuContext ? gpuContext->holdRuntime() : std::shared_ptr<void>();
    if (!token || (gpuContext->runtime() != state->runtime) || state->runtime->isLost()) {
        *error = "GPU compute lost";

        return false;
    }
    gpu::GpuDevice& device = state->runtime->device();
    const VkDeviceSize rowBytes = (VkDeviceSize)width * (VkDeviceSize)window.nComps * sizeof(float);
    const VkDeviceSize bytes = rowBytes * (VkDeviceSize)height;

    std::unique_ptr<Slot> slot;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    unsigned int glSemaphore = 0;
    gpu::GpuStatus failed;
    bool packFailed = false;
    const bool rendered = HydraRender::withRender(request, [&](HydraRender::RenderAccess& access, std::string* renderError) -> bool {
        if (gpu::GpuStatus s = state->makeSlot(bytes, &slot); !s) {
            failed = s;

            return false;
        }
        if (!access.pack(window, slot->glBuffer, renderError)) {
            packFailed = true;

            return false;
        }
        if (path == gpu::GlHandoffPath::ZeroCopy) {
            failed = gpu::createExportableSemaphore(device, &semaphore);
            if (failed) {
                failed = state->gl->importSemaphore(device, semaphore, &glSemaphore);
            }
            if (failed) {
                failed = state->gl->signal(glSemaphore, slot->glBuffer);
            }
        } else {
            failed = state->gl->finish();
        }

        return (bool)failed; }, error);

    Slot* const used = slot.get();
    if (slot) {
        // From here the slot and semaphores go back through the render thread, whatever happens, once the
        // context's work is done: the copy below reads the slot before then.
        context.hold(std::make_shared<Use>(gpuContext, state, std::move(slot), semaphore, glSemaphore));
    } else if (semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(device.device(), semaphore, nullptr);
    }
    if (!rendered) {
        if (gpu::isDeviceLost(failed)) {
            state->abandon("device lost during the hand-off");
            *error = "GPU compute lost";
        } else if (!failed) {
            state->stepDown(path, failed.message);
            *error = failed.message;
        } else if (packFailed) {
            state->stepDown(path, *error);
        }

        return false;
    }

    if (path == gpu::GlHandoffPath::ZeroCopy) {
        context.waitExternalSemaphore(semaphore);
    }
    // One copy per row unless the output's rows are the window's.
    const VkDeviceSize floatBytes = sizeof(float);
    const VkDeviceSize origin = output.byteOffset + floatBytes * (VkDeviceSize)(((std::int64_t)(outputRect.y1 - output.bounds.y1) * output.rowStrideFloats) + (std::int64_t)(outputRect.x1 - output.bounds.x1) * output.nComps);
    std::vector<VkBufferCopy> regions;
    if ((VkDeviceSize)output.rowStrideFloats * floatBytes == rowBytes) {
        regions.push_back(VkBufferCopy { 0, origin, bytes });
    } else {
        regions.reserve((std::size_t)height);
        for (int row = 0; row < height; ++row) {
            regions.push_back(VkBufferCopy { rowBytes * (VkDeviceSize)row, origin + floatBytes * (VkDeviceSize)output.rowStrideFloats * (VkDeviceSize)row, rowBytes });
        }
    }
    if (gpu::GpuStatus s = context.copyFromExternal(used->buffer, bytes, *output.buffer, regions); !s) {
        if (gpu::isDeviceLost(s)) {
            state->abandon("device lost during the hand-off");
        } else {
            state->stepDown(path, s.message);
        }
        *error = s.message;

        return false;
    }

    return true;
} // HydraHandoff::record

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU
