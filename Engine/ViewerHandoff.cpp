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

#include "ViewerHandoff.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>

#include <QtCore/QDebug>
#include <QtCore/QString>

#include "Engine/Gpu/GlInterop.h"
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/ViewerGpuProcess.h"

NATRON_NAMESPACE_ENTER

namespace {

// Small enough to cost nothing, only to find out whether a rung's interop can be built at all.
const VkDeviceSize kProbeCapacity = 4096;

// GpuKernel's descriptor pools hold 1024 sets; on a device without push descriptors they are
// recycled once no frame is in flight, and waited for when frames never stop overlapping.
const unsigned kDispatchesBeforeDrain = 512;

std::optional<ViewerHandoffRung>
overrideRung()
{
    const char* env = std::getenv("NATRON_VIEWER_HANDOFF");
    if (!env) {
        return std::nullopt;
    }
    std::string value(env);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (ViewerHandoffRung r : { ViewerHandoffRung::ZeroCopy, ViewerHandoffRung::HostSync, ViewerHandoffRung::Readback,
                                 ViewerHandoffRung::Cpu }) {
        if (value == toString(r)) {
            return r;
        }
    }

    return std::nullopt;
}

gpu::GlHandoffPath
pathOf(ViewerHandoffRung rung)
{
    switch (rung) {
    case ViewerHandoffRung::ZeroCopy:
        return gpu::GlHandoffPath::ZeroCopy;
    case ViewerHandoffRung::HostSync:
        return gpu::GlHandoffPath::ZeroCopyHostSync;
    default:
        return gpu::GlHandoffPath::Readback;
    }
}

gpu::GlPixelFormat
pixelFormatOf(ViewerHandoffFormat format)
{
    return (format == ViewerHandoffFormat::Bgra8) ? gpu::GlPixelFormat::Bgra8 : gpu::GlPixelFormat::Rgba32F;
}

ViewerHandoffResult
refuse(const std::string& reason)
{
    ViewerHandoffResult r;
    r.reason = reason;

    return r;
}

ViewerHandoffResult
accept()
{
    ViewerHandoffResult r;
    r.ok = true;

    return r;
}

bool
isLostStatus(const gpu::GpuStatus& s)
{
    return s.result == VK_ERROR_DEVICE_LOST;
}

// What the render thread may share with the GL thread. The process is declared after the token so
// that it is destroyed first, before the runtime may be recreated.
struct Device {
    std::shared_ptr<void> token;
    gpu::GpuRuntime* runtime = nullptr;
    std::unique_ptr<ViewerGpuProcess> process;
};

struct PendingReadback {
    std::uint64_t token = 0;
    ViewerHandoffFrame frame;
};

struct Ring {
    std::unique_ptr<gpu::GlInterop> interop;
    std::array<GpuDeviceImagePtr, gpu::GlInterop::kSlots> pinned;
    std::array<PendingReadback, gpu::GlInterop::kSlots> readbacks;
};

} // namespace

const char*
toString(ViewerHandoffRung rung)
{
    switch (rung) {
    case ViewerHandoffRung::ZeroCopy:
        return "zerocopy";
    case ViewerHandoffRung::HostSync:
        return "hostsync";
    case ViewerHandoffRung::Readback:
        return "readback";
    case ViewerHandoffRung::Cpu:
        return "cpu";
    }

    return "?";
}

std::size_t
ViewerHandoffFrame::bytes() const
{
    return std::size_t(width) * height * gpu::bytesPerPixel(pixelFormatOf(format));
}

struct ViewerHandoff::Imp {
    GpuContext* context = nullptr;
    GlProcLoader loader;
    bool created = false;

    // Set when building failed for a reason that a retry cannot fix, until the next create() or a
    // device loss.
    bool buildFailed = false;

    std::atomic<ViewerHandoffRung> rung { ViewerHandoffRung::Cpu };

    mutable std::mutex mutex;
    std::shared_ptr<Device> device;
    std::string reason;

    std::array<Ring, kTextureCount> rings;
    std::vector<ViewerHandoffReadback> completed;
    std::size_t wantedCapacity = 0;
    unsigned dispatchesSinceRelease = 0;

    // The rung last written to the log; unset until the first one is.
    std::optional<ViewerHandoffRung> loggedRung;

    // Logs the rung once when it is first chosen and again whenever it changes, with its reason.
    void noteRung()
    {
        const ViewerHandoffRung r = rung.load(std::memory_order_acquire);
        if (loggedRung && (*loggedRung == r)) {
            return;
        }
        loggedRung = r;
        std::string why;
        {
            std::lock_guard<std::mutex> k(mutex);
            why = reason;
        }
        QString line = QString::fromLatin1("Viewer hand-off rung: %1").arg( QString::fromLatin1( toString(r) ) );
        if (!why.empty()) {
            line += QString::fromLatin1(" (%1)").arg( QString::fromStdString(why) );
        }
        qInfo().noquote() << line;
    }

    struct NoteRungOnExit {
        Imp* imp;

        ~NoteRungOnExit() { imp->noteRung(); }
    };

    void setReason(const std::string& r)
    {
        std::lock_guard<std::mutex> k(mutex);
        reason = r;
    }

    std::shared_ptr<Device> currentDevice() const
    {
        std::lock_guard<std::mutex> k(mutex);

        return device;
    }

    void unpinSlot(Ring& ring,
                   int slot)
    {
        if (ring.pinned[slot]) {
            ring.pinned[slot]->unpin();
            ring.pinned[slot].reset();
        }
    }

    // GlInterop's destructor waits for its in-flight frames, so the pins can go after it.
    void destroyRing(Ring& ring)
    {
        ring.interop.reset();
        ring.readbacks = {};
        for (int i = 0; i < gpu::GlInterop::kSlots; ++i) {
            unpinSlot(ring, i);
        }
    }

    void destroyRings()
    {
        for (Ring& ring : rings) {
            destroyRing(ring);
        }
        dispatchesSinceRelease = 0;
    }

    void drop(const std::string& why,
              bool lost)
    {
        destroyRings();
        std::shared_ptr<Device> old;
        {
            std::lock_guard<std::mutex> k(mutex);
            std::swap(old, device);
            reason = why;
        }
        rung.store(ViewerHandoffRung::Cpu, std::memory_order_release);
        old.reset();
        completed.clear();
        wantedCapacity = 0;
        if (lost) {
            buildFailed = false;
        }
    }

    // Moves the host copy of a slot to `completed` once the device has written it, waiting for it when
    // asked. A copy that cannot be read is dropped.
    void harvest(Ring& ring,
                 int slot,
                 bool wait)
    {
        PendingReadback& pending = ring.readbacks[slot];
        if (!pending.token || !ring.interop) {
            return;
        }
        if (wait && !ring.interop->waitProduced(slot)) {
            pending = PendingReadback();

            return;
        }
        ViewerHandoffReadback out;
        const gpu::GpuStatus s = ring.interop->readHostCopy(slot, out.bytes);
        if (s.result == VK_TIMEOUT) {
            return;
        }
        if (s) {
            out.token = pending.token;
            out.frame = pending.frame;
            completed.push_back(std::move(out));
        }
        pending = PendingReadback();
    }

    // Unpins the root of every slot the device is done with. Returns whether a frame is still in
    // flight, or false after a loss, which it reports in *lost.
    bool collect(Ring& ring,
                 bool wait,
                 bool* lost)
    {
        bool busy = false;
        for (int i = 0; i < gpu::GlInterop::kSlots; ++i) {
            if (!ring.pinned[i]) {
                continue;
            }
            const gpu::GpuStatus s = ring.interop->waitProduced(i, wait ? UINT64_MAX : 0);
            if (s) {
                unpinSlot(ring, i);
            } else if (s.result == VK_TIMEOUT) {
                busy = true;
            } else {
                *lost = true;

                return false;
            }
        }

        return busy;
    }

    // Recycles the viewer kernels' descriptor sets once none of them is in use by a pending frame.
    bool recycleDescriptors(Device& dev,
                            bool* lost)
    {
        bool busy = false;
        for (Ring& ring : rings) {
            if (ring.interop) {
                busy = collect(ring, false, lost) || busy;
            }
        }
        if (*lost) {
            return false;
        }
        if (busy && (dispatchesSinceRelease >= kDispatchesBeforeDrain)) {
            busy = false;
            for (Ring& ring : rings) {
                if (ring.interop) {
                    collect(ring, true, lost);
                }
            }
            if (*lost) {
                return false;
            }
        }
        if (!busy) {
            dev.process->releaseDescriptors();
            dispatchesSinceRelease = 0;
        }

        return true;
    }

    gpu::GpuStatus createInterop(Device& dev,
                                 ViewerHandoffRung r,
                                 VkDeviceSize capacity,
                                 std::unique_ptr<gpu::GlInterop>& out)
    {
        return gpu::GlInterop::create(dev.runtime->device(), loader, capacity, pathOf(r), out);
    }

    void build()
    {
        const std::optional<ViewerHandoffRung> forced = overrideRung();
        if (forced && (*forced == ViewerHandoffRung::Cpu)) {
            buildFailed = true;
            setReason("NATRON_VIEWER_HANDOFF=cpu");

            return;
        }
        if (!context) {
            buildFailed = true;
            setReason("no GPU context");

            return;
        }
        if (!context->isAvailable()) {
            if (!context->isInitializing() && !context->runtime()) {
                buildFailed = true;
            }
            setReason("GPU compute unavailable");

            return;
        }
        std::shared_ptr<Device> dev = std::make_shared<Device>();
        dev->token = context->holdRuntime();
        dev->runtime = context->runtime();
        if (!dev->token || !dev->runtime || dev->runtime->isLost()) {
            setReason("GPU compute unavailable");

            return;
        }

        gpu::GlInteropCaps caps;
        if (gpu::GpuStatus s = gpu::GlInterop::probe(dev->runtime->device(), loader, caps); !s) {
            buildFailed = true;
            setReason("GL interop probe failed: " + s.message);

            return;
        }

        std::string why;
        std::optional<ViewerHandoffRung> picked;
        const ViewerHandoffRung first = forced ? *forced : ViewerHandoffRung::ZeroCopy;
        for (int i = (int)first; i <= (int)ViewerHandoffRung::Readback; ++i) {
            const ViewerHandoffRung r = (ViewerHandoffRung)i;
            if (!caps.supports(pathOf(r))) {
                why += std::string(why.empty() ? "" : "; ") + toString(r) + " unavailable: missing " + caps.missing;
                continue;
            }
            std::unique_ptr<gpu::GlInterop> trial;
            const gpu::GpuStatus s = createInterop(*dev, r, kProbeCapacity, trial);
            if (s) {
                picked = r;
                break;
            }
            if (isLostStatus(s)) {
                setReason("device lost while building the GL interop");

                return;
            }
            why += std::string(why.empty() ? "" : "; ") + toString(r) + " failed: " + s.message;
        }
        if (!picked) {
            buildFailed = true;
            setReason(why.empty() ? std::string("no GL interop rung") : why);

            return;
        }
        if (forced && (*forced != ViewerHandoffRung::ZeroCopy)) {
            why = std::string("NATRON_VIEWER_HANDOFF=") + toString(*forced) + (why.empty() ? "" : "; ") + why;
        }
        dev->process.reset(new ViewerGpuProcess(*dev->runtime));
        {
            std::lock_guard<std::mutex> k(mutex);
            device = dev;
            reason = why;
        }
        rung.store(*picked, std::memory_order_release);
    }

    // Steps every ring down to the next rung that builds. False once none is left.
    bool descend(Device& dev,
                 const std::string& why)
    {
        destroyRings();
        ViewerHandoffRung r = rung.load(std::memory_order_acquire);
        while (r != ViewerHandoffRung::Readback) {
            r = (ViewerHandoffRung)((int)r + 1);
            std::unique_ptr<gpu::GlInterop> trial;
            if (createInterop(dev, r, kProbeCapacity, trial)) {
                rung.store(r, std::memory_order_release);
                setReason(why);

                return true;
            }
        }

        return false;
    }

    // Builds the ring of index for at least `bytes`, stepping down the ladder when the rung's
    // interop cannot be built.
    ViewerHandoffResult buildRing(Device& dev,
                                  int index,
                                  std::size_t bytes)
    {
        for (;;) {
            const ViewerHandoffRung r = rung.load(std::memory_order_acquire);
            const gpu::GpuStatus s = createInterop(dev, r, VkDeviceSize(bytes), rings[index].interop);
            if (s) {
                return accept();
            }
            if (isLostStatus(s) || dev.runtime->isLost()) {
                drop("device lost", true);

                return refuse("device lost: " + s.message);
            }
            const std::string why = std::string(toString(r)) + " slots of " + std::to_string(bytes) + " bytes failed: " + s.message;
            if ((r == ViewerHandoffRung::Readback) || !descend(dev, why)) {
                return refuse(why);
            }
        }
    }

    ViewerHandoffResult grow(Device& dev)
    {
        for (int i = 0; i < kTextureCount; ++i) {
            Ring& ring = rings[i];
            if (!ring.interop || (ring.interop->capacity() >= wantedCapacity)) {
                continue;
            }
            destroyRing(ring);
            if (ViewerHandoffResult r = buildRing(dev, i, wantedCapacity); !r) {
                return r;
            }
        }

        return accept();
    }
};

ViewerHandoff::ViewerHandoff(GpuContext* context)
    : _imp(new Imp)
{
    _imp->context = context;
    _imp->reason = "not created";
}

ViewerHandoff::~ViewerHandoff()
{
    release();
}

ViewerHandoffRung
ViewerHandoff::create(const GlProcLoader& loader)
{
    release();
    _imp->loader = loader;
    _imp->created = true;
    _imp->buildFailed = false;
    maintain();

    return rung();
}

ViewerHandoffRung
ViewerHandoff::rung() const
{
    const ViewerHandoffRung r = _imp->rung.load(std::memory_order_acquire);
    if ((r == ViewerHandoffRung::Cpu) || !_imp->context || !_imp->context->isAvailable()) {
        return ViewerHandoffRung::Cpu;
    }

    return r;
}

std::string
ViewerHandoff::rungReason() const
{
    std::lock_guard<std::mutex> k(_imp->mutex);

    return _imp->reason;
}

bool
ViewerHandoff::maintain()
{
    if (!_imp->created) {
        return false;
    }
    Imp::NoteRungOnExit note { _imp.get() };
    std::shared_ptr<Device> dev = _imp->currentDevice();
    if (dev) {
        bool lost = dev->runtime->isLost();
        for (Ring& ring : _imp->rings) {
            if (!lost && ring.interop) {
                _imp->collect(ring, false, &lost);
            }
        }
        if (lost) {
            dev.reset();
            _imp->drop("device lost", true);

            return false;
        }

        return true;
    }
    if (_imp->buildFailed) {
        return false;
    }
    _imp->build();

    return bool(_imp->currentDevice());
}

ViewerHandoffResult
ViewerHandoff::prepare(const ProjectColorManagement::DisplayProcessorPtr& processor,
                       ViewerHandoffFormat format)
{
    std::shared_ptr<Device> dev = _imp->currentDevice();
    if (!dev) {
        return refuse("rung cpu: " + rungReason());
    }
    if (dev->runtime->isLost()) {
        return refuse("device lost");
    }
    const gpu::GpuStatus s = dev->process->prepare(processor, pixelFormatOf(format));
    if (!s) {
        return refuse("viewer process unavailable: " + s.message);
    }

    return accept();
}

ViewerHandoffResult
ViewerHandoff::present(int textureIndex,
                       const GpuDeviceImagePtr& root,
                       const ViewerHandoffParams& params,
                       unsigned int glTexture,
                       const ViewerHandoffFrame& frame)
{
    if (!maintain()) {
        return refuse("rung cpu: " + rungReason());
    }
    Imp::NoteRungOnExit note { _imp.get() };
    std::shared_ptr<Device> dev = _imp->currentDevice();
    if ((textureIndex < 0) || (textureIndex >= kTextureCount)) {
        return refuse("texture index " + std::to_string(textureIndex) + " out of range");
    }
    if (!root) {
        return refuse("no device root");
    }
    if (root->runtime() != dev->runtime) {
        return refuse("root belongs to another GPU runtime");
    }
    if ((frame.width == 0) || (frame.height == 0) || (int(frame.width) != params.rect.width()) ||
        (int(frame.height) != params.rect.height())) {
        return refuse("frame " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + " does not match the rect " +
                      std::to_string(params.rect.width()) + "x" + std::to_string(params.rect.height()));
    }

    if (ViewerHandoffResult r = _imp->grow(*dev); !r) {
        return r;
    }

    const std::size_t bytes = frame.bytes();
    Ring& ring = _imp->rings[textureIndex];
    if (!ring.interop) {
        const std::size_t capacity = std::max(bytes, _imp->wantedCapacity);
        if (ViewerHandoffResult r = _imp->buildRing(*dev, textureIndex, capacity); !r) {
            return r;
        }
    } else if (ring.interop->capacity() < bytes) {
        _imp->wantedCapacity = std::max(_imp->wantedCapacity, bytes);

        return refuse("frame of " + std::to_string(bytes) + " bytes is over the " + std::to_string(ring.interop->capacity()) +
                      "-byte slots, which grow for the next frame");
    }

    bool lost = false;
    if (!_imp->recycleDescriptors(*dev, &lost)) {
        _imp->drop("device lost", true);

        return refuse("device lost");
    }

    gpu::GpuBuffer* buffer = root->pin();
    if (!buffer) {
        return refuse("root was evicted or downloaded");
    }
    const GpuImageLayout& layout = root->layout();
    GpuImageView view;
    view.buffer = buffer;
    view.byteOffset = 0;
    view.bounds = layout.bounds;
    view.nComps = layout.nComps();
    view.rowStrideFloats = layout.bounds.width() * view.nComps;

    ViewerGpuProcessParams processParams;
    processParams.rect = params.rect;
    processParams.channels = params.channels;
    processParams.alphaChannelIndex = params.alphaChannelIndex;
    processParams.matteOverlay = params.matteOverlay;
    processParams.gain = params.gain;
    processParams.offset = params.offset;
    processParams.gamma = params.gamma;
    processParams.displayProcessor = params.displayProcessor;

    // The slot produce() is about to overwrite may still hold a host copy nobody has collected.
    if (const int next = ring.interop->nextSlot(); next >= 0) {
        _imp->harvest(ring, next, true);
    }

    const gpu::GlFrame glFrame { frame.width, frame.height, pixelFormatOf(frame.format) };
    ViewerGpuProcess& process = *dev->process;
    int slot = -1;
    const gpu::GpuStatus produced = ring.interop->produce(
        glFrame,
        [&](VkCommandBuffer cmd, VkBuffer dst) { return process.record(cmd, view, dst, glFrame, processParams); },
        {},
        &slot,
        params.readbackToken != 0);
    if (!produced) {
        root->unpin();
        if (isLostStatus(produced) || dev->runtime->isLost()) {
            dev.reset();
            _imp->drop("device lost", true);

            return refuse("device lost: " + produced.message);
        }

        return refuse("viewer process: " + produced.message);
    }
    _imp->unpinSlot(ring, slot);
    ring.pinned[slot] = root;
    ring.readbacks[slot].token = params.readbackToken;
    ring.readbacks[slot].frame = frame;
    ++_imp->dispatchesSinceRelease;

    const gpu::GpuStatus uploaded = ring.interop->upload(slot, glTexture, frame.x, frame.y);
    if (!uploaded) {
        if (isLostStatus(uploaded) || dev->runtime->isLost()) {
            dev.reset();
            _imp->drop("device lost", true);

            return refuse("device lost: " + uploaded.message);
        }
        // A slot whose upload failed stays pending in the interop, so the ring is rebuilt.
        _imp->destroyRing(ring);

        return refuse("upload: " + uploaded.message);
    }
    _imp->collect(ring, false, &lost);
    if (lost) {
        dev.reset();
        _imp->drop("device lost", true);

        return refuse("device lost");
    }

    return accept();
}

std::vector<ViewerHandoffReadback>
ViewerHandoff::takeCompletedReadbacks()
{
    for (Ring& ring : _imp->rings) {
        for (int i = 0; i < gpu::GlInterop::kSlots; ++i) {
            _imp->harvest(ring, i, false);
        }
    }
    std::vector<ViewerHandoffReadback> out;
    out.swap(_imp->completed);

    return out;
}

ViewerHandoffResult
ViewerHandoff::ensureCapacity(std::size_t bytes)
{
    if (!maintain()) {
        return refuse("rung cpu: " + rungReason());
    }
    Imp::NoteRungOnExit note { _imp.get() };
    std::shared_ptr<Device> dev = _imp->currentDevice();
    _imp->wantedCapacity = std::max(_imp->wantedCapacity, bytes);

    return _imp->grow(*dev);
}

std::size_t
ViewerHandoff::capacity(int textureIndex) const
{
    if ((textureIndex < 0) || (textureIndex >= kTextureCount) || !_imp->rings[textureIndex].interop) {
        return 0;
    }

    return std::size_t(_imp->rings[textureIndex].interop->capacity());
}

void
ViewerHandoff::release()
{
    _imp->drop("released", false);
    _imp->created = false;
    _imp->buildFailed = false;
    _imp->loader = GlProcLoader();
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU
