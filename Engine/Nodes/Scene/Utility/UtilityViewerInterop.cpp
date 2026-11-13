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

#include "UtilityViewerInterop.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <optional>

#include "Engine/Gpu/GlInterop.h"
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuDevice.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"

NATRON_NAMESPACE_ENTER

namespace {
// What NATRON_VIEWER_HANDOFF asks of every GL hand-off: a path to start the ladder from, or none
// ("cpu").
struct Override {
    bool refuse = false;
    std::optional<gpu::GlHandoffPath> first;
};

Override
handoffOverride()
{
    Override out;
    const char* env = std::getenv("NATRON_VIEWER_HANDOFF");

    if (!env) {
        return out;
    }
    std::string value(env);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    out.refuse = value == "cpu";
    out.first = gpu::parseOverride(env);

    return out;
}
} // anon namespace

struct UtilityViewerInterop::Implementation {
    explicit Implementation(const ProcLoader& loader)
        : loader(loader)
    {
    }

    const ProcLoader loader;
    gpu::GpuDevice* device = nullptr;
    // Keeps the device the interop is made on from being recreated under it.
    std::shared_ptr<void> runtimeHold;
    std::optional<gpu::GlHandoffPath> path;
    bool unsupported = false;
    // The pixels each slot was last produced from, so that they outlive the copy the device makes of them.
    std::array<gpu::GpuBufferHandle, gpu::GlInterop::kSlots> slotPixels;
    // Last, so that it goes before the pixels and the hold.
    std::unique_ptr<gpu::GlInterop> interop;

    void release()
    {
        interop.reset();
        slotPixels.fill(gpu::GpuBufferHandle());
        device = nullptr;
        path.reset();
        runtimeHold.reset();
    }

    bool build(VkDeviceSize bytes,
               std::string* error)
    {
        const gpu::GlProcLoader gl = loader;

        if (path) {
            interop.reset();
            if (gpu::GpuStatus s = gpu::GlInterop::create(*device, gl, bytes, *path, interop); !s) {
                *error = "The GL hand-off could not be made again: " + s.message;
                interop.reset();

                return false;
            }

            return true;
        }

        const Override forced = handoffOverride();
        if (forced.refuse) {
            *error = "NATRON_VIEWER_HANDOFF=cpu";
            unsupported = true;

            return false;
        }
        gpu::GlInteropCaps caps;
        if (gpu::GpuStatus s = gpu::GlInterop::probe(*device, gl, caps); !s) {
            *error = "GL interop probe failed: " + s.message;

            return false;
        }
        const gpu::GlHandoffChoice choice = gpu::chooseHandoffPath(caps, forced.first ? *forced.first : gpu::GlHandoffPath::ZeroCopy, [&](gpu::GlHandoffPath trial) {
            interop.reset();

            return gpu::GlInterop::create(*device, gl, bytes, trial, interop);
        });
        if (!choice.path) {
            interop.reset();
            *error = choice.lost ? "The device was lost while the GL hand-off was made." : "No GL hand-off path builds: " + choice.reasons;
            unsupported = !choice.lost;

            return false;
        }
        path = choice.path;

        return true;
    }
};

UtilityViewerInterop::UtilityViewerInterop(const ProcLoader& loader)
    : _imp(new Implementation(loader))
{
}

UtilityViewerInterop::~UtilityViewerInterop()
{
    _imp->release();
}

bool
UtilityViewerInterop::upload(const std::shared_ptr<const UtilityDeviceResource>& film,
                             unsigned int texture,
                             std::string* error)
{
    _imp->unsupported = false;
    releaseIfLost();

    gpu::GpuBufferHandle pixels;
    std::shared_ptr<void> runtimeHold;

    if (!film || !film->lease(&pixels, &runtimeHold) || !film->device() || (film->width() == 0) || (film->height() == 0)) {
        *error = "The Utility renderer holds no frame on the device.";

        return false;
    }
    if (film->device() != _imp->device) {
        _imp->release();
        _imp->device = film->device();
        _imp->runtimeHold = runtimeHold;
    }

    const gpu::GlFrame frame { film->width(), film->height(), gpu::GlPixelFormat::Rgba32F };
    const VkDeviceSize bytes = frame.bytes();

    if (pixels.size() < bytes) {
        *error = "The Utility renderer's film is smaller than its frame.";

        return false;
    }
    if ((!_imp->interop || (_imp->interop->capacity() < bytes)) && !_imp->build(bytes, error)) {
        return false;
    }

    const VkBuffer source = pixels.vkBuffer();
    const gpu::GlRecordFn record = [source, bytes](VkCommandBuffer cmd, VkBuffer dst) {
        // The film was written by compute work submitted earlier on the same queue.
        VkBufferMemoryBarrier2 toCopy { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        toCopy.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toCopy.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toCopy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.buffer = source;
        toCopy.size = VK_WHOLE_SIZE;
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &toCopy;
        vkCmdPipelineBarrier2(cmd, &dep);
        const VkBufferCopy region { 0, 0, bytes };
        vkCmdCopyBuffer(cmd, source, dst, 1, &region);

        return gpu::GpuStatus();
    };

    int slot = -1;
    if (gpu::GpuStatus s = _imp->interop->produce(frame, record, {}, &slot); !s) {
        *error = "The GL hand-off could not copy the frame: " + s.message;
        _imp->release();

        return false;
    }
    _imp->slotPixels[slot] = pixels;
    if (gpu::GpuStatus s = _imp->interop->upload(slot, texture, 0, 0); !s) {
        *error = "The GL hand-off could not upload the frame: " + s.message;
        (void)_imp->interop->discard(slot);
        _imp->release();

        return false;
    }

    return true;
}

std::string
UtilityViewerInterop::path() const
{
    return _imp->path ? std::string(gpu::overrideName(*_imp->path)) : std::string();
}

bool
UtilityViewerInterop::unsupported() const
{
    return _imp->unsupported;
}

void
UtilityViewerInterop::release()
{
    _imp->release();
}

void
UtilityViewerInterop::releaseIfLost()
{
    if (_imp->device && _imp->device->isLost()) {
        _imp->release();
    }
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU
