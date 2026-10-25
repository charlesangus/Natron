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
#include <cassert>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QtCore/QDebug>
#include <QtCore/QString>
#include <QtCore/QThreadPool>

#include "Engine/Gpu/GlInterop.h"
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/ViewerGpuProcess.h"

NATRON_NAMESPACE_ENTER

namespace {

// Small enough to cost nothing, only to find out whether a rung's interop can be built at all.
const VkDeviceSize kProbeCapacity = 4096;

// Ahead of the render tasks and their fork-join helpers, since present() blocks the GL thread on a fill that has not
// finished once the ring comes round to its slot.
const int kFillRunnablePriority = 8;

// Ahead of the render tasks too, since the first frame's render thread waits for the compile in prepare().
const int kPrecompileRunnablePriority = 8;

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

// A precompile() handed to the pool. The device is taken by the worker when it starts, or dropped by the GL thread
// to abandon it before then, so that a queued compile holds no runtime token.
struct Precompile {
    std::mutex mutex;
    std::shared_ptr<Device> device;
    ViewerHandoffProcessorFn processor;
    gpu::GlPixelFormat format = gpu::GlPixelFormat::Rgba32F;

    void abandon()
    {
        // Released outside the lock, as this may be the last reference to the device.
        std::shared_ptr<Device> dev;
        ViewerHandoffProcessorFn fn;
        {
            std::lock_guard<std::mutex> k(mutex);
            dev.swap(device);
            fn.swap(processor);
        }
    }

    void run()
    {
        std::shared_ptr<Device> dev;
        ViewerHandoffProcessorFn fn;
        {
            std::lock_guard<std::mutex> k(mutex);
            dev.swap(device);
            fn.swap(processor);
        }
        if (!dev || dev->runtime->isLost()) {
            return;
        }
        // A failure stays silent here: the process remembers it, and the frame's own prepare() reports it.
        try {
            ProjectColorManagement::DisplayProcessorPtr resolved;
            if ((format == gpu::GlPixelFormat::Bgra8) && fn) {
                resolved = fn();
            }
            dev->process->prepare(resolved, format);
        } catch (...) {
        }
    }
};

struct PendingReadback {
    std::uint64_t token = 0;
    ViewerHandoffFrame frame;
};

// A host copy that present() had to take out of its slot before producing into it again.
struct OwnedReadback {
    std::uint64_t token = 0;
    ViewerHandoffFrame frame;
    std::vector<unsigned char> bytes;
};

// Work on a readback, run once: by a pool thread, or by the first thread to wait for it before the pool started it,
// so that waiting never depends on a free pool thread.
class FillJob {
public:
    explicit FillJob(std::function<void()> work)
        : _work(std::move(work))
    {
    }

    static void start(const std::shared_ptr<FillJob>& job)
    {
        const auto runnable = [job]() {
            if (job->take()) {
                job->run();
            }
        };
        QThreadPool::globalInstance()->start(runnable, kFillRunnablePriority);
    }

    bool finished() const
    {
        std::lock_guard<std::mutex> k(_mutex);

        return _state == State::Finished;
    }

    // Runs the work here if it has not started, else blocks until it has finished.
    void wait()
    {
        if (take()) {
            run();

            return;
        }
        std::unique_lock<std::mutex> k(_mutex);
        _finished.wait(k, [this]() { return _state == State::Finished; });
    }

    // Drops the work if it has not started, else blocks until it has finished.
    void cancel()
    {
        std::function<void()> dropped;
        std::unique_lock<std::mutex> k(_mutex);
        if (_state == State::Queued) {
            _state = State::Finished;
            dropped.swap(_work);
            k.unlock();
            _finished.notify_all();

            return;
        }
        _finished.wait(k, [this]() { return _state == State::Finished; });
    }

private:
    enum class State {
        Queued,
        Running,
        Finished,
    };

    bool take()
    {
        std::lock_guard<std::mutex> k(_mutex);
        if (_state != State::Queued) {
            return false;
        }
        _state = State::Running;

        return true;
    }

    void run()
    {
        std::function<void()> work;
        {
            std::lock_guard<std::mutex> k(_mutex);
            work.swap(_work);
        }
        // Thrown on a pool thread, it would end the process.
        try {
            work();
        } catch (const std::exception& e) {
            qWarning().noquote() << "Viewer hand-off readback work failed:" << e.what();
        } catch (...) {
            qWarning().noquote() << "Viewer hand-off readback work failed";
        }
        work = nullptr;
        {
            std::lock_guard<std::mutex> k(_mutex);
            _state = State::Finished;
        }
        _finished.notify_all();
    }

    mutable std::mutex _mutex;
    std::condition_variable _finished;
    State _state = State::Queued;
    std::function<void()> _work;
};

using FillJobPtr = std::shared_ptr<FillJob>;

struct Ring {
    std::unique_ptr<gpu::GlInterop> interop;
    std::array<GpuDeviceImagePtr, gpu::GlInterop::kSlots> pinned;
    std::array<PendingReadback, gpu::GlInterop::kSlots> readbacks;
    // The work reading the slot's host copy in place; the slot is not produced into or freed until it has finished.
    std::array<FillJobPtr, gpu::GlInterop::kSlots> fills;
    // The id of the frame stage() left in the slot, 0 when none, and the frame it was given.
    std::array<std::uint64_t, gpu::GlInterop::kSlots> staged {};
    std::array<ViewerHandoffFrame, gpu::GlInterop::kSlots> stagedFrames {};

    int stagedSlot(std::uint64_t id) const
    {
        for (int i = 0; i < gpu::GlInterop::kSlots; ++i) {
            if (id && (staged[i] == id)) {
                return i;
            }
        }

        return -1;
    }

    bool holdsStaged() const
    {
        return std::any_of(staged.begin(), staged.end(), [](std::uint64_t id) { return id != 0; });
    }
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
    std::vector<OwnedReadback> owned;
    // The work on readbacks copied out of their slots, which holds no slot.
    std::vector<FillJobPtr> detachedFills;
    // Set while consumeCompletedReadbacks() calls its claim, which must not change the rings it walks.
    bool claiming = false;
    std::size_t wantedCapacity = 0;
    unsigned dispatchesSinceRelease = 0;
    std::uint64_t nextStagedId = 1;

    std::optional<std::pair<std::string, ViewerHandoffFormat>> precompileRequest;
    ViewerHandoffProcessorFn precompileProcessor;
    std::shared_ptr<Precompile> precompiling;
    std::array<std::size_t, kTextureCount> reserved {};
    // Set when a ring could not be built at its reserved size, so that every paint does not try again; cleared by a
    // larger reservation and by a device loss.
    std::array<bool, kTextureCount> reserveFailed {};

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
        QString line = QString::fromLatin1("Viewer hand-off rung: %1").arg(QString::fromLatin1(toString(r)));
        if (!why.empty()) {
            line += QString::fromLatin1(" (%1)").arg(QString::fromStdString(why));
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

    void waitFill(Ring& ring,
                  int slot)
    {
        if (ring.fills[slot]) {
            ring.fills[slot]->wait();
            ring.fills[slot].reset();
        }
    }

    void cancelFills(std::vector<FillJobPtr>& jobs)
    {
        for (const FillJobPtr& job : jobs) {
            job->cancel();
        }
        jobs.clear();
    }

    // GlInterop's destructor waits for its in-flight frames, so the pins can go after it. The fills go first, since
    // the destructor unmaps the memory they read.
    void destroyRing(Ring& ring)
    {
        for (FillJobPtr& job : ring.fills) {
            if (job) {
                job->cancel();
                job.reset();
            }
        }
        ring.interop.reset();
        ring.readbacks = {};
        ring.staged = {};
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

    void abandonPrecompile()
    {
        if (precompiling) {
            precompiling->abandon();
            precompiling.reset();
        }
    }

    void startPrecompile(const std::shared_ptr<Device>& dev)
    {
        abandonPrecompile();
        if (!precompileRequest || !dev || dev->runtime->isLost()) {
            return;
        }
        std::shared_ptr<Precompile> job = std::make_shared<Precompile>();
        job->device = dev;
        job->processor = precompileProcessor;
        job->format = pixelFormatOf(precompileRequest->second);
        precompiling = job;
        QThreadPool::globalInstance()->start([job]() { job->run(); }, kPrecompileRunnablePriority);
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
        abandonPrecompile();
        old.reset();
        owned.clear();
        cancelFills(detachedFills);
        wantedCapacity = 0;
        if (lost) {
            buildFailed = false;
            reserveFailed = {};
        }
    }

    // Reads the host copy of a slot once the device has written it, starting the work \p claim returns for it on the
    // slot in place, or, when \p claim is null, waiting for it and copying it to `owned`. A copy that cannot be read
    // is dropped.
    void harvest(Ring& ring,
                 int slot,
                 const ViewerHandoffClaimFn* claim)
    {
        PendingReadback& pending = ring.readbacks[slot];
        if (!pending.token || !ring.interop) {
            return;
        }
        if (!claim && !ring.interop->waitProduced(slot)) {
            pending = PendingReadback();

            return;
        }
        const unsigned char* data = nullptr;
        VkDeviceSize size = 0;
        const gpu::GpuStatus s = ring.interop->hostCopyView(slot, &data, &size);
        if (s.result == VK_TIMEOUT) {
            return;
        }
        const PendingReadback taken = pending;
        pending = PendingReadback();
        if (!s) {
            return;
        }
        if (claim) {
            ViewerHandoffReadbackFn work = callClaim(*claim, taken.token);
            if (!work) {
                return;
            }
            ViewerHandoffReadback lent;
            lent.token = taken.token;
            lent.frame = taken.frame;
            lent.data = data;
            lent.size = static_cast<std::size_t>(size);
            assert(!ring.fills[slot]);
            ring.fills[slot] = std::make_shared<FillJob>([work, lent]() { work(lent); });
            FillJob::start(ring.fills[slot]);
        } else {
            OwnedReadback copy;
            copy.token = taken.token;
            copy.frame = taken.frame;
            copy.bytes.assign(data, data + size);
            owned.push_back(std::move(copy));
        }
    }

    ViewerHandoffReadbackFn callClaim(const ViewerHandoffClaimFn& claim,
                                      std::uint64_t token)
    {
        claiming = true;
        struct EndClaiming {
            bool* claiming;

            ~EndClaiming() { *claiming = false; }
        } endClaiming { &claiming };

        return claim(token);
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
        startPrecompile(dev);
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

    // What present() and stage() share: checks the frame, builds or grows the slots, and submits the viewer process of
    // root into a free slot, which then keeps root pinned. Stores the slot in *slot. With keepStaged, slots that must
    // first grow are refused rather than rebuilt while they hold a staged frame.
    ViewerHandoffResult produceFrame(std::shared_ptr<Device>& dev,
                                     int textureIndex,
                                     const GpuDeviceImagePtr& root,
                                     const ViewerHandoffParams& params,
                                     const ViewerHandoffFrame& frame,
                                     bool keepStaged,
                                     int* slot)
    {
        if ((textureIndex < 0) || (textureIndex >= kTextureCount)) {
            return refuse("texture index " + std::to_string(textureIndex) + " out of range");
        }
        if (!root) {
            return refuse("no device root");
        }
        if (root->runtime() != dev->runtime) {
            return refuse("root belongs to another GPU runtime");
        }
        if ((frame.width == 0) || (frame.height == 0) || (int(frame.width) != params.rect.width()) || (int(frame.height) != params.rect.height())) {
            return refuse("frame " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + " does not match the rect " + std::to_string(params.rect.width()) + "x" + std::to_string(params.rect.height()));
        }

        const std::size_t bytes = frame.bytes();
        Ring& ring = rings[textureIndex];
        if (keepStaged) {
            for (const Ring& other : rings) {
                const bool mustGrow = other.interop && ((other.interop->capacity() < wantedCapacity) || ((&other == &ring) && (other.interop->capacity() < bytes)));
                if (mustGrow && other.holdsStaged()) {
                    wantedCapacity = std::max(wantedCapacity, bytes);

                    return refuse("slots holding a staged frame must grow first");
                }
            }
        }

        if (ViewerHandoffResult r = grow(*dev); !r) {
            return r;
        }

        if (!ring.interop) {
            const std::size_t capacity = std::max(bytes, wantedCapacity);
            if (ViewerHandoffResult r = buildRing(*dev, textureIndex, capacity); !r) {
                return r;
            }
        } else if (ring.interop->capacity() < bytes) {
            wantedCapacity = std::max(wantedCapacity, bytes);

            return refuse("frame of " + std::to_string(bytes) + " bytes is over the " + std::to_string(ring.interop->capacity()) + "-byte slots, which grow for the next frame");
        }
        const int next = ring.interop->nextSlot();
        if (next < 0) {
            return refuse("every slot holds a staged frame");
        }

        bool lost = false;
        if (!recycleDescriptors(*dev, &lost)) {
            dev.reset();
            drop("device lost", true);

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

        // The slot produce() is about to overwrite may still hold a host copy that a fill is reading or that nobody
        // has collected.
        waitFill(ring, next);
        harvest(ring, next, nullptr);

        const gpu::GlFrame glFrame { frame.width, frame.height, pixelFormatOf(frame.format) };
        ViewerGpuProcess& process = *dev->process;
        const gpu::GpuStatus produced = ring.interop->produce(
            glFrame,
            [&](VkCommandBuffer cmd, VkBuffer dst) { return process.record(cmd, view, dst, glFrame, processParams); },
            {},
            slot,
            params.readbackToken != 0);
        if (!produced) {
            root->unpin();
            if (isLostStatus(produced) || dev->runtime->isLost()) {
                dev.reset();
                drop("device lost", true);

                return refuse("device lost: " + produced.message);
            }

            return refuse("viewer process: " + produced.message);
        }
        unpinSlot(ring, *slot);
        ring.pinned[*slot] = root;
        ring.readbacks[*slot].token = params.readbackToken;
        ring.readbacks[*slot].frame = frame;
        ++dispatchesSinceRelease;

        return accept();
    }

    // Uploads the frame produced into slot, and unpins the roots the device is done with.
    ViewerHandoffResult uploadSlot(std::shared_ptr<Device>& dev,
                                   Ring& ring,
                                   int slot,
                                   unsigned int glTexture,
                                   const ViewerHandoffFrame& frame)
    {
        const gpu::GpuStatus uploaded = ring.interop->upload(slot, glTexture, frame.x, frame.y);
        if (!uploaded) {
            if (isLostStatus(uploaded) || dev->runtime->isLost()) {
                dev.reset();
                drop("device lost", true);

                return refuse("device lost: " + uploaded.message);
            }
            // A slot whose upload failed stays pending in the interop, so the ring is rebuilt.
            destroyRing(ring);

            return refuse("upload: " + uploaded.message);
        }
        bool lost = false;
        collect(ring, false, &lost);
        if (lost) {
            dev.reset();
            drop("device lost", true);

            return refuse("device lost");
        }

        return accept();
    }

    // Builds the ring of index at its reserved size unless it already holds it, or while it holds a staged frame.
    ViewerHandoffResult applyReservation(Device& dev,
                                         int index)
    {
        Ring& ring = rings[index];
        const std::size_t bytes = reserved[index];
        if (!bytes || reserveFailed[index] || (ring.interop && (ring.interop->capacity() >= bytes))) {
            return accept();
        }
        if (ring.interop && ring.holdsStaged()) {
            return accept();
        }
        destroyRing(ring);
        const ViewerHandoffResult r = buildRing(dev, index, std::max(bytes, wantedCapacity));
        if (!r && currentDevice()) {
            reserveFailed[index] = true;
        }

        return r;
    }

    void applyReservations(const std::shared_ptr<Device>& dev)
    {
        for (int i = 0; i < kTextureCount; ++i) {
            if (currentDevice() != dev) {
                return;
            }
            applyReservation(*dev, i);
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
    if (_imp->claiming) {
        return bool(_imp->currentDevice());
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
    } else {
        if (_imp->buildFailed) {
            return false;
        }
        _imp->build();
        dev = _imp->currentDevice();
        if (!dev) {
            return false;
        }
    }
    _imp->applyReservations(dev);

    return bool(_imp->currentDevice());
}

void
ViewerHandoff::precompile(const std::string& key,
                          ViewerHandoffFormat format,
                          const ViewerHandoffProcessorFn& processor)
{
    if (_imp->precompileRequest && (_imp->precompileRequest->first == key) && (_imp->precompileRequest->second == format)) {
        return;
    }
    _imp->precompileRequest.emplace(key, format);
    _imp->precompileProcessor = processor;
    _imp->startPrecompile(_imp->currentDevice());
}

ViewerHandoffResult
ViewerHandoff::reserve(int textureIndex,
                       std::size_t bytes)
{
    if ((textureIndex < 0) || (textureIndex >= kTextureCount)) {
        return refuse("texture index " + std::to_string(textureIndex) + " out of range");
    }
    if (_imp->claiming) {
        return refuse("reserve() while a readback is claimed");
    }
    std::size_t& reserved = _imp->reserved[textureIndex];
    if (reserved >= bytes) {
        return accept();
    }
    reserved = bytes;
    _imp->reserveFailed[textureIndex] = false;
    if (!maintain()) {
        return refuse("rung cpu: " + rungReason());
    }
    const std::size_t built = capacity(textureIndex);
    if (built < bytes) {
        return refuse("slots of " + std::to_string(bytes) + " bytes are not built: "
                      + (_imp->rings[textureIndex].holdsStaged() ? std::string("the ring holds a staged frame") : rungReason()));
    }

    return accept();
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
    if (_imp->claiming) {
        return refuse("present() while a readback is claimed");
    }
    if (!maintain()) {
        return refuse("rung cpu: " + rungReason());
    }
    Imp::NoteRungOnExit note { _imp.get() };
    std::shared_ptr<Device> dev = _imp->currentDevice();
    int slot = -1;
    if (ViewerHandoffResult r = _imp->produceFrame(dev, textureIndex, root, params, frame, false, &slot); !r) {
        return r;
    }

    return _imp->uploadSlot(dev, _imp->rings[textureIndex], slot, glTexture, frame);
}

ViewerHandoffResult
ViewerHandoff::stage(int textureIndex,
                     const GpuDeviceImagePtr& root,
                     const ViewerHandoffParams& params,
                     const ViewerHandoffFrame& frame,
                     std::uint64_t* staged)
{
    if (!staged) {
        return refuse("stage() needs somewhere to store the staged frame");
    }
    *staged = 0;
    if (_imp->claiming) {
        return refuse("stage() while a readback is claimed");
    }
    if (!maintain()) {
        return refuse("rung cpu: " + rungReason());
    }
    Imp::NoteRungOnExit note { _imp.get() };
    std::shared_ptr<Device> dev = _imp->currentDevice();
    int slot = -1;
    if (ViewerHandoffResult r = _imp->produceFrame(dev, textureIndex, root, params, frame, true, &slot); !r) {
        return r;
    }
    Ring& ring = _imp->rings[textureIndex];
    *staged = _imp->nextStagedId++;
    ring.staged[slot] = *staged;
    ring.stagedFrames[slot] = frame;

    return accept();
}

bool
ViewerHandoff::stagedComputing(int textureIndex,
                               std::uint64_t staged)
{
    if ((textureIndex < 0) || (textureIndex >= kTextureCount)) {
        return false;
    }
    Ring& ring = _imp->rings[textureIndex];
    const int slot = ring.stagedSlot(staged);
    if ((slot < 0) || !ring.interop) {
        return false;
    }

    return ring.interop->waitProduced(slot, 0).result == VK_TIMEOUT;
}

ViewerHandoffResult
ViewerHandoff::presentStaged(int textureIndex,
                             std::uint64_t staged,
                             unsigned int glTexture,
                             const ViewerHandoffFrame& frame)
{
    if (_imp->claiming) {
        return refuse("presentStaged() while a readback is claimed");
    }
    if (!maintain()) {
        return refuse("rung cpu: " + rungReason());
    }
    Imp::NoteRungOnExit note { _imp.get() };
    std::shared_ptr<Device> dev = _imp->currentDevice();
    if ((textureIndex < 0) || (textureIndex >= kTextureCount)) {
        return refuse("texture index " + std::to_string(textureIndex) + " out of range");
    }
    Ring& ring = _imp->rings[textureIndex];
    const int slot = ring.stagedSlot(staged);
    if (slot < 0) {
        return refuse("staged frame lost with its slots");
    }
    const ViewerHandoffFrame& was = ring.stagedFrames[slot];
    ring.staged[slot] = 0;
    if ((was.width != frame.width) || (was.height != frame.height) || (was.format != frame.format)) {
        const ViewerHandoffResult refused = refuse("staged frame does not match the texture it is presented into");
        if (!ring.interop->discard(slot)) {
            _imp->destroyRing(ring);

            return refused;
        }
        const gpu::GpuStatus s = ring.interop->waitProduced(slot);
        if (!s) {
            dev.reset();
            _imp->drop("device lost", true);

            return refuse("device lost: " + s.message);
        }
        _imp->unpinSlot(ring, slot);

        return refused;
    }

    return _imp->uploadSlot(dev, ring, slot, glTexture, frame);
}

void
ViewerHandoff::discardStaged(int textureIndex,
                             std::uint64_t staged)
{
    if ((textureIndex < 0) || (textureIndex >= kTextureCount)) {
        return;
    }
    if (_imp->claiming) {
        return;
    }
    Ring& ring = _imp->rings[textureIndex];
    const int slot = ring.stagedSlot(staged);
    if ((slot < 0) || !ring.interop) {
        return;
    }
    ring.staged[slot] = 0;
    if (!ring.interop->discard(slot)) {
        // A slot left pending can never be produced into again.
        _imp->destroyRing(ring);
    }
}

void
ViewerHandoff::unpinRoots()
{
    if (_imp->claiming) {
        return;
    }
    // A loss is left for the next maintain() to drop, since destroying the rings needs the GL context current.
    bool lost = false;
    for (Ring& ring : _imp->rings) {
        if (ring.interop && !lost) {
            _imp->collect(ring, true, &lost);
        }
    }
}

void
ViewerHandoff::consumeCompletedReadbacks(const ViewerHandoffClaimFn& claim)
{
    if (_imp->claiming) {
        return;
    }
    std::vector<FillJobPtr>& detached = _imp->detachedFills;
    detached.erase(std::remove_if(detached.begin(), detached.end(), [](const FillJobPtr& job) { return job->finished(); }),
                   detached.end());

    std::vector<OwnedReadback> owned;
    owned.swap(_imp->owned);
    for (OwnedReadback& o : owned) {
        ViewerHandoffReadbackFn work = _imp->callClaim(claim, o.token);
        if (!work) {
            continue;
        }
        std::shared_ptr<const OwnedReadback> copy = std::make_shared<const OwnedReadback>(std::move(o));
        detached.push_back(std::make_shared<FillJob>([work, copy]() {
            ViewerHandoffReadback lent;
            lent.token = copy->token;
            lent.frame = copy->frame;
            lent.data = copy->bytes.data();
            lent.size = copy->bytes.size();
            work(lent);
        }));
        FillJob::start(detached.back());
    }
    for (Ring& ring : _imp->rings) {
        for (int i = 0; i < gpu::GlInterop::kSlots; ++i) {
            _imp->harvest(ring, i, &claim);
        }
    }
}

ViewerHandoffResult
ViewerHandoff::ensureCapacity(std::size_t bytes)
{
    if (_imp->claiming) {
        return refuse("ensureCapacity() while a readback is claimed");
    }
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
    assert(!_imp->claiming);
    _imp->drop("released", false);
    _imp->precompileRequest.reset();
    _imp->precompileProcessor = ViewerHandoffProcessorFn();
    _imp->reserved = {};
    _imp->reserveFailed = {};
    _imp->created = false;
    _imp->buildFailed = false;
    _imp->loader = GlProcLoader();
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU
