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

#include "GeoKernelGpuRunner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Engine/Geo/GeoPage.h"
#include "Engine/GpuWeight.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

bool
jumpsBack(const KernelProgram& program)
{
    for (std::size_t k = 0; k < program.instructions.size(); ++k) {
        const KernelInstruction& in = program.instructions[k];
        if ((in.op == KernelOp::eJump || in.op == KernelOp::eJumpIfNone) && in.imm >= 0 && std::size_t(in.imm) <= k) {
            return true;
        }
    }
    return false;
}

bool
storesAny(const KernelGpuPlan& plan)
{
    return std::any_of(plan.columns.begin(), plan.columns.end(), [](const KernelGpuColumn& c) { return c.store; });
}

// Instructions of a kernel that weigh as much as one Point node's work on a pixel: a colour op's
// handful of operations on each of four channels.
constexpr double kInstructionsPerPointWeight = 16.;

#ifdef NATRON_HAVE_GPU

// Each element may take at least this many backward jumps; batches shrink until it can.
constexpr std::uint32_t kMinJumpBudget = 4096;

// The backward jumps a batch may take in all, in batchWork() units: a jump costs a few
// instructions, so this keeps a batch of runaway loops well inside the submit timeout.
constexpr std::uint64_t kJumpsPerBatchWork = 4;

bool
stopRequested(const GeoKernelRunOptions& options)
{
    return options.aborted && options.aborted();
}

KernelRunResult
gpuFailure(const std::string& what, const gpu::GpuStatus& status, const GeoKernelRunOptions& options)
{
    KernelRunResult r;
    r.status = status.aborted || stopRequested(options) ? KernelRunStatus::eAborted : KernelRunStatus::eFailed;
    r.error = what + (status.message.empty() ? std::string() : ": " + status.message);
    return r;
}

// Elements per chunk: a multiple of the page size whose data buffer the largest bindable scratch
// buffer holds.
std::size_t
chunkElements(std::size_t columns, std::uint64_t maxBufferBytes)
{
    const std::uint64_t bytesPerElement = std::uint64_t(std::max<std::size_t>(columns, 1)) * 4u;
    std::uint64_t n = maxBufferBytes / bytesPerElement;
    n -= n % kGeoPageSize;
    return std::size_t(std::max<std::uint64_t>(n, kGeoPageSize));
}

struct Batching {
    // Elements per dispatch, a multiple of the group size whose group count fits the 65535 every
    // device allows in x.
    std::size_t elements = kKernelGpuGroupSize;
    std::uint64_t elementWork = 1;
    std::uint32_t jumpBudget = kKernelGpuJumpBudget;
};

Batching
batching(const KernelProgram& program, const KernelGpuPlan& plan, std::uint64_t batchWork)
{
    Batching b;
    b.elementWork = geoKernelGpuElementWork(program, plan);
    std::uint64_t n = std::max<std::uint64_t>(batchWork / b.elementWork, 1);
    const bool loops = jumpsBack(program);
    const std::uint64_t jumps = batchWork * kJumpsPerBatchWork;
    if (loops) {
        n = std::min<std::uint64_t>(n, std::max<std::uint64_t>(jumps / kMinJumpBudget, 1));
    }
    n = std::min<std::uint64_t>(n, std::uint64_t(65535) * kKernelGpuGroupSize);
    n = std::max<std::uint64_t>(n - n % kKernelGpuGroupSize, kKernelGpuGroupSize);
    b.elements = std::size_t(n);
    if (loops) {
        b.jumpBudget = std::uint32_t(std::clamp<std::uint64_t>(jumps / n, kMinJumpBudget, kKernelGpuJumpBudget));
    }
    return b;
}

gpu::GpuStatus
waitAll(gpu::GpuRuntime& runtime, const std::vector<gpu::TimelineValue>& done, gpu::GpuStatus st)
{
    for (const gpu::TimelineValue& v : done) {
        const gpu::GpuStatus w = runtime.wait(v);
        if (st && !w) {
            st = w;
        }
    }
    return st;
}

// Runs elements [begin, begin + n) of values, uploading each loaded column's range and
// downloading each stored one in place.
KernelRunResult
runChunk(gpu::GpuRuntime& runtime, const KernelGpuPlan& plan, const Batching& batch, const GeoKernelRunOptions& options,
         GeoKernelDenseValues& values, std::size_t begin, std::size_t n)
{
    std::unique_ptr<gpu::GpuRecordContext> rec;
    gpu::GpuStatus st = runtime.note(runtime.newRecordContext(rec));
    if (!st || !rec) {
        return gpuFailure("the GPU could not record", st, options);
    }
    if (options.aborted) {
        rec->setAbortCheck(options.aborted);
    }
    std::shared_ptr<gpu::GpuKernel> kernel;
    st = runtime.note(rec->runtimeKernel(plan.source, gpu::ShaderStage::Compute, "main", kernel));
    if (!st || !kernel) {
        return gpuFailure("the GPU compiler rejected the kernel", st, options);
    }

    const std::size_t columns = plan.columns.size();
    const VkDeviceSize columnBytes = VkDeviceSize(n * sizeof(std::uint32_t));
    const VkDeviceSize dataBytes = std::max<VkDeviceSize>(columnBytes * columns, sizeof(std::uint32_t));
    std::vector<std::uint32_t> uniformWords = values.uniforms;
    uniformWords.resize(std::max<std::size_t>(uniformWords.size(), 1), 0u);
    const VkDeviceSize uniformBytes = VkDeviceSize(uniformWords.size() * sizeof(std::uint32_t));
    gpu::GpuBuffer* data = nullptr;
    gpu::GpuBuffer* uni = nullptr;
    gpu::GpuBuffer* status = nullptr;
    if (!(st = runtime.note(rec->scratch(dataBytes, data))) || !data || !(st = runtime.note(rec->scratch(uniformBytes, uni))) || !uni
        || !(st = runtime.note(rec->scratch(sizeof(std::uint32_t), status))) || !status) {
        return gpuFailure("the GPU has no room for the kernel's values", st, options);
    }

    std::uint32_t runaway = 0;
    std::vector<gpu::TimelineValue> done;
    const auto upload = [&](void* src, VkDeviceSize bytes, gpu::GpuBuffer& dst, VkDeviceSize offset) {
        gpu::TimelineValue v;
        const gpu::GpuStatus s = runtime.upload(gpu::HostRange { src, bytes }, dst, offset, v);
        if (s) {
            done.push_back(v);
        }
        return s;
    };
    st = upload(uniformWords.data(), uniformBytes, *uni, 0);
    if (st) {
        st = upload(&runaway, sizeof runaway, *status, 0);
    }
    for (std::size_t c = 0; st && c < columns; ++c) {
        if (plan.columns[c].load) {
            st = upload(values.words.data() + c * values.count + begin, columnBytes, *data, VkDeviceSize(c) * columnBytes);
        }
    }
    st = waitAll(runtime, done, st);
    if (!st) {
        return gpuFailure("the GPU upload failed", st, options);
    }

    std::vector<gpu::GpuBinding> bindings;
    for (const gpu::GpuKernelBinding& b : kernel->bindings()) {
        if (b.binding == 0) {
            bindings.push_back(gpu::GpuBinding::storage(0, *data));
        } else if (b.binding == 1) {
            bindings.push_back(gpu::GpuBinding::storage(1, *uni));
        } else if (b.binding == 2) {
            bindings.push_back(gpu::GpuBinding::storage(2, *status));
        }
    }
    for (std::size_t first = 0; first < n; first += batch.elements) {
        const std::size_t band = std::min(batch.elements, n - first);
        const std::array<std::uint32_t, 4> push { std::uint32_t(first), std::uint32_t(n), std::uint32_t(n), batch.jumpBudget };
        static_assert(sizeof(push) == kKernelGpuPushBytes);
        st = rec->dispatchBindings(kernel, bindings, std::as_bytes(std::span(push)), { std::uint32_t(band), 1, 1 });
        if (st) {
            rec->computeBarrier();
            st = rec->chargeWork(std::uint64_t(band) * batch.elementWork);
        }
        if (!st) {
            return gpuFailure("the GPU dispatch failed", runtime.note(st), options);
        }
    }
    const std::size_t batches = rec->batchCount();
    st = runtime.note(rec->submitAndWait(30'000'000'000ull, options.aborted ? options.aborted : std::function<bool()>()));
    if (!st) {
        return gpuFailure("the GPU run failed", st, options);
    }

    const auto download = [&](gpu::GpuBuffer& src, VkDeviceSize offset, void* dst, VkDeviceSize bytes) {
        gpu::TimelineValue v;
        const gpu::GpuStatus s = runtime.download(src, offset, gpu::HostRange { dst, bytes }, v);
        if (s) {
            done.push_back(v);
        }
        return s;
    };
    done.clear();
    st = waitAll(runtime, done, download(*status, 0, &runaway, sizeof runaway));
    if (!st) {
        return gpuFailure("the GPU download failed", st, options);
    }
    if (runaway != 0) {
        KernelRunResult r;
        r.status = KernelRunStatus::eFailed;
        r.error = "a loop ran past the GPU iteration budget";
        return r;
    }
    done.clear();
    for (std::size_t c = 0; st && c < columns; ++c) {
        if (plan.columns[c].store) {
            st = download(*data, VkDeviceSize(c) * columnBytes, values.words.data() + c * values.count + begin, columnBytes);
        }
    }
    st = waitAll(runtime, done, st);
    if (!st) {
        return gpuFailure("the GPU download failed", st, options);
    }
    KernelRunResult r;
    r.batches = batches;
    return r;
}

KernelRunResult
runOnGpu(gpu::GpuRuntime& runtime, const KernelProgram& program, const KernelGpuPlan& plan, std::uint64_t maxBytes,
         std::uint64_t batchWork, const GeoKernelRunOptions& options, GeoKernelDenseValues& values)
{
    KernelRunResult result;
    const std::size_t chunk = chunkElements(plan.columns.size(), maxBytes);
    const Batching batch = batching(program, plan, batchWork);
    for (std::size_t begin = 0; begin < values.count; begin += chunk) {
        const KernelRunResult r = runChunk(runtime, plan, batch, options, values, begin, std::min(chunk, values.count - begin));
        if (r.status != KernelRunStatus::eDone) {
            return r;
        }
        result.batches += r.batches;
    }
    return result;
}

#endif // NATRON_HAVE_GPU

} // namespace

std::uint64_t
geoKernelGpuElementWork(const KernelProgram& program, const KernelGpuPlan& plan)
{
    return std::max<std::uint64_t>(std::uint64_t(program.instructions.size()) + plan.columns.size(), 1);
}

bool
geoKernelGpuPlaced(const KernelProgram& program, const KernelGpuPlan& plan, std::size_t elements)
{
    if (!plan.runsOnGpu() || !storesAny(plan)) {
        return false;
    }
    const double pixels = double(elements) * double(std::max<std::size_t>(plan.columns.size(), 1)) / 4.;
    const double weight = double(program.instructions.size()) / kInstructionsPerPointWeight;
    return gpuWeightAccepts(pixels, weight, jumpsBack(program));
}

GeoKernelGpuRunResult
geoKernelRunGpu(const KernelProgram& program, const KernelGpuPlan& plan, GeoAttribClassEnum runOver,
                std::span<const GeoDetail* const> inputs, const GeoKernelRunOptions& options, gpu::GpuRuntime* runtime)
{
    GeoKernelGpuRunResult out;
    if (plan.runsOnGpu() && !storesAny(plan)) {
        std::string declined;
        out.result = geoKernelRunDense(
            program, plan, runOver, inputs, options, [](GeoKernelDenseValues&) { return KernelRunResult(); }, declined);
        return out;
    }
#ifdef NATRON_HAVE_GPU
    if (runtime && !runtime->isLost()) {
        if (!plan.runsOnGpu()) {
            out.fallback = plan.cpuOnlyReason;
        } else {
            std::uint64_t maxBytes = 0;
            std::uint64_t batchWork = 0;
            std::unique_ptr<gpu::GpuRecordContext> probe;
            gpu::GpuStatus st = runtime->note(runtime->newRecordContext(probe));
            std::shared_ptr<gpu::GpuKernel> kernel;
            if (!st || !probe) {
                out.fallback = gpuFailure("the GPU could not record", st, options).error;
            } else if (!(st = runtime->note(probe->runtimeKernel(plan.source, gpu::ShaderStage::Compute, "main", kernel))) || !kernel) {
                out.fallback = gpuFailure("the GPU compiler rejected the kernel", st, options).error;
                out.compilerRejected = st.result == VK_ERROR_INITIALIZATION_FAILED && !runtime->isLost();
            } else {
                maxBytes = probe->maxScratchBytes();
                batchWork = probe->batchWork();
                if (maxBytes < std::uint64_t(std::max<std::size_t>(plan.columns.size(), 1)) * 4u * kGeoPageSize) {
                    out.fallback = "the GPU's storage buffers are too small for a page of elements";
                }
            }
            kernel.reset();
            probe.reset();
            if (out.fallback.empty()) {
                std::string declined;
                out.result = geoKernelRunDense(
                    program, plan, runOver, inputs, options,
                    [&](GeoKernelDenseValues& values) { return runOnGpu(*runtime, program, plan, maxBytes, batchWork, options, values); },
                    declined);
                if (declined.empty()) {
                    out.ranOnGpu = out.result.status == KernelRunStatus::eDone;
                    return out;
                }
                out.fallback = declined;
            }
        }
    }
#else
    (void)runtime;
#endif
    out.result = geoKernelRun(program, runOver, inputs, options);
    return out;
}

NATRON_NAMESPACE_EXIT
