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

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

#ifdef NATRON_HAVE_GPU

KernelRunResult
gpuFailure(const std::string& what, const gpu::GpuStatus& status)
{
    KernelRunResult r;
    r.status = status.aborted ? KernelRunStatus::eAborted : KernelRunStatus::eFailed;
    r.error = what + (status.message.empty() ? std::string() : ": " + status.message);
    return r;
}

// Elements per dispatch: a multiple of the page size whose data buffer the largest bindable
// scratch buffer holds and whose group count fits the 65535 every device allows in x.
std::size_t
chunkElements(std::size_t columns, std::uint64_t maxBufferBytes)
{
    const std::uint64_t bytesPerElement = std::uint64_t(std::max<std::size_t>(columns, 1)) * 4u;
    std::uint64_t n = std::min<std::uint64_t>(maxBufferBytes / bytesPerElement, std::uint64_t(65535) * kKernelGpuGroupSize);
    n -= n % kGeoPageSize;
    return std::size_t(std::max<std::uint64_t>(n, kGeoPageSize));
}

KernelRunResult
runChunk(gpu::GpuRuntime& runtime, const KernelGpuPlan& plan, const GeoKernelRunOptions& options,
         const std::vector<std::uint32_t>& uniforms, std::vector<std::uint32_t>& words, std::size_t count)
{
    std::unique_ptr<gpu::GpuRecordContext> rec;
    gpu::GpuStatus st = runtime.note(runtime.newRecordContext(rec));
    if (!st || !rec) {
        return gpuFailure("the GPU could not record", st);
    }
    if (options.aborted) {
        rec->setAbortCheck(options.aborted);
    }
    std::shared_ptr<gpu::GpuKernel> kernel;
    st = runtime.note(rec->runtimeKernel(plan.source, gpu::ShaderStage::Compute, "main", kernel));
    if (!st || !kernel) {
        return gpuFailure("the GPU compiler rejected the kernel", st);
    }

    const VkDeviceSize dataBytes = VkDeviceSize(std::max<std::size_t>(words.size(), 1) * sizeof(std::uint32_t));
    const VkDeviceSize uniformBytes = VkDeviceSize(std::max<std::size_t>(uniforms.size(), 1) * sizeof(std::uint32_t));
    gpu::GpuBuffer* data = nullptr;
    gpu::GpuBuffer* uni = nullptr;
    gpu::GpuBuffer* status = nullptr;
    if (!(st = runtime.note(rec->scratch(dataBytes, data))) || !data || !(st = runtime.note(rec->scratch(uniformBytes, uni))) || !uni
        || !(st = runtime.note(rec->scratch(sizeof(std::uint32_t), status))) || !status) {
        return gpuFailure("the GPU has no room for the kernel's values", st);
    }

    std::uint32_t runaway = 0;
    std::vector<std::uint32_t> uniformWords = uniforms;
    uniformWords.resize(std::max<std::size_t>(uniformWords.size(), 1), 0u);
    const auto upload = [&](void* src, VkDeviceSize bytes, gpu::GpuBuffer& dst) {
        gpu::TimelineValue done;
        gpu::GpuStatus s = runtime.upload(gpu::HostRange { src, bytes }, dst, 0, done);
        if (s) {
            s = runtime.wait(done);
        }
        return s;
    };
    if (!(st = upload(words.data(), VkDeviceSize(words.size() * sizeof(std::uint32_t)), *data))
        || !(st = upload(uniformWords.data(), uniformBytes, *uni)) || !(st = upload(&runaway, sizeof runaway, *status))) {
        return gpuFailure("the GPU upload failed", st);
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
    const std::array<std::uint32_t, 3> push { std::uint32_t(count), std::uint32_t(count), kKernelGpuJumpBudget };
    static_assert(sizeof(push) == kKernelGpuPushBytes);
    st = rec->dispatchBindings(kernel, bindings, std::as_bytes(std::span(push)), { std::uint32_t(count), 1, 1 });
    if (!st) {
        return gpuFailure("the GPU dispatch failed", runtime.note(st));
    }
    st = runtime.note(rec->submitAndWait(30'000'000'000ull, options.aborted ? options.aborted : std::function<bool()>()));
    if (!st) {
        return gpuFailure("the GPU run failed", st);
    }

    const auto download = [&](gpu::GpuBuffer& src, void* dst, VkDeviceSize bytes) {
        gpu::TimelineValue done;
        gpu::GpuStatus s = runtime.download(src, 0, gpu::HostRange { dst, bytes }, done);
        if (s) {
            s = runtime.wait(done);
        }
        return s;
    };
    if (!(st = download(*status, &runaway, sizeof runaway))) {
        return gpuFailure("the GPU download failed", st);
    }
    if (runaway != 0) {
        KernelRunResult r;
        r.status = KernelRunStatus::eFailed;
        r.error = "a loop ran past the GPU iteration budget";
        return r;
    }
    if (!(st = download(*data, words.data(), VkDeviceSize(words.size() * sizeof(std::uint32_t))))) {
        return gpuFailure("the GPU download failed", st);
    }
    return KernelRunResult();
}

KernelRunResult
runOnGpu(gpu::GpuRuntime& runtime, const KernelGpuPlan& plan, const GeoKernelRunOptions& options, GeoKernelDenseValues& values)
{
    KernelRunResult result;
    const bool stores = std::any_of(plan.columns.begin(), plan.columns.end(), [](const KernelGpuColumn& c) { return c.store; });
    if (!stores) {
        return result;
    }
    std::unique_ptr<gpu::GpuRecordContext> probe;
    gpu::GpuStatus st = runtime.note(runtime.newRecordContext(probe));
    if (!st || !probe) {
        return gpuFailure("the GPU could not record", st);
    }
    const std::size_t columns = plan.columns.size();
    const std::uint64_t maxBytes = probe->maxScratchBytes();
    probe.reset();
    if (maxBytes < std::uint64_t(std::max<std::size_t>(columns, 1)) * 4u * kGeoPageSize) {
        KernelRunResult r;
        r.status = KernelRunStatus::eFailed;
        r.error = "the GPU's storage buffers are too small for a page of elements";
        return r;
    }
    const std::size_t chunk = chunkElements(columns, maxBytes);

    std::vector<std::uint32_t> words;
    for (std::size_t begin = 0; begin < values.count; begin += chunk) {
        const std::size_t n = std::min(chunk, values.count - begin);
        words.assign(n * columns, 0u);
        for (std::size_t c = 0; c < columns; ++c) {
            if (plan.columns[c].load) {
                const std::uint32_t* src = values.words.data() + c * values.count + begin;
                std::copy(src, src + n, words.begin() + std::ptrdiff_t(c * n));
            }
        }
        const KernelRunResult r = runChunk(runtime, plan, options, values.uniforms, words, n);
        if (r.status != KernelRunStatus::eDone) {
            return r;
        }
        for (std::size_t c = 0; c < columns; ++c) {
            if (plan.columns[c].store) {
                std::copy(words.begin() + std::ptrdiff_t(c * n), words.begin() + std::ptrdiff_t((c + 1) * n),
                          values.words.begin() + std::ptrdiff_t(c * values.count + begin));
            }
        }
        ++result.batches;
    }
    return result;
}

#endif // NATRON_HAVE_GPU

} // namespace

GeoKernelGpuRunResult
geoKernelRunGpu(const KernelProgram& program, const KernelGpuPlan& plan, GeoAttribClassEnum runOver,
                std::span<const GeoDetail* const> inputs, const GeoKernelRunOptions& options, gpu::GpuRuntime* runtime)
{
    GeoKernelGpuRunResult out;
#ifdef NATRON_HAVE_GPU
    if (runtime && !runtime->isLost()) {
        if (!plan.runsOnGpu()) {
            out.fallback = plan.cpuOnlyReason;
        } else {
            std::string declined;
            out.result = geoKernelRunDense(
                program, plan, runOver, inputs, options,
                [&](GeoKernelDenseValues& values) { return runOnGpu(*runtime, plan, options, values); }, declined);
            if (declined.empty()) {
                out.ranOnGpu = out.result.status == KernelRunStatus::eDone;
                return out;
            }
            out.fallback = declined;
        }
    }
#else
    (void)plan;
    (void)runtime;
#endif
    out.result = geoKernelRun(program, runOver, inputs, options);
    return out;
}

NATRON_NAMESPACE_EXIT
