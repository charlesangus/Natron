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

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/Geo/GeoKernelBinding.h"
#include "Engine/Geo/GeoKernelGpuRunner.h"
#include "Engine/GeoDetail.h"
#include "Engine/Kernel/KernelGpuCodegen.h"
#include "Engine/Kernel/KernelParser.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuSizeClass.h"
#endif

NATRON_NAMESPACE_USING

#ifdef NATRON_HAVE_GPU

namespace {

// Several pages and a partial one, so that items, batches and chunks all have tails.
constexpr GeoIndex kPointCount = GeoIndex(3 * kGeoPageSize + 77);

// Relative to the larger magnitude, with an absolute floor for results near zero. Noise and the
// trigonometric builtins run in float on the device against double on the CPU.
constexpr double kRelativeTolerance = 1e-5;
constexpr double kAbsoluteTolerance = 1e-5;

GeoDetail
makePoints(GeoIndex count)
{
    GeoDetailBuilder builder;
    builder.appendPoints(count);
    std::vector<float> p(std::size_t(count) * 3);
    for (std::size_t i = 0; i < std::size_t(count); ++i) {
        p[3 * i] = float(i) * 0.25f;
        p[3 * i + 1] = float(i % 977) * 0.5f - 7.f;
        p[3 * i + 2] = float(i % 13) - 6.5f;
    }
    builder.positionForWriting().scatterDense<float>(0, std::size_t(count), std::span<const float>(p));
    return builder.finish();
}

GeoKernelParameters
testParameters()
{
    GeoKernelParameters parameters;
    GeoKernelParameter amp;
    amp.values = { 2., 0., 0. };
    parameters["amp"] = amp;
    GeoKernelParameter count;
    count.values = { 3., 0., 0. };
    parameters["count"] = count;
    GeoKernelParameter offset;
    offset.components = 3;
    offset.values = { 0.5, -1., 2. };
    parameters["offset"] = offset;
    return parameters;
}

struct Snippet {
    const char* source;
    std::vector<std::string> attributes;
};

const std::vector<Snippet>&
snippets()
{
    static const std::vector<Snippet> list = {
        { "@P.y += 1;", { "P" } },
        { "if (@ptnum >= 1100 && @ptnum < 1200) @P.y += 1; i@id = 7;", { "P", "id" } },
        { "for (int k = 0; k < @ptnum % 7; k++) f@acc += k * 0.5;", { "acc" } },
        { "float n = noise(@P * 0.1); @P.y += sin(@P.x * 0.004) * n + rand(@ptnum);", { "P" } },
        { "v@Cd = noise(@P * 0.3); f@r = rand(@P.x); f@s = noise(@P.y * 0.7);", { "Cd", "r", "s" } },
        { "vector4 q = quaternion(radians(45), {0, 1, 0}); @orient = q; @N = qrotate(q, @P);", { "orient", "N" } },
        { "matrix m = ident(); m = transpose(m) * invert(m); f@det = determinant(m) * @P.x;", { "det" } },
        { "@P.x = ch(\"amp\") * @P.x + chi(\"count\"); @P += chv(\"offset\"); i@n = npoints(0); f@t = @Time + @Frame + @numpt;",
          { "P", "n", "t" } },
        { "int a = @ptnum - 500; i@q = a / 7; i@r = a % 7; i@s = a << 3; i@t = a >> 2; int z = @ptnum * 0; i@u = a / z; i@v = a % -3;",
          { "q", "r", "s", "t", "u", "v" } },
        { "f@m = @P.y % 1.5; f@r = round(@P.y); f@a = atan2(@P.y, @P.x); f@p = pow(abs(@P.y), 1.5); f@l = log10(abs(@P.x) + 1);",
          { "m", "r", "a", "p", "l" } },
        { "int i = @ptnum; @Cd[i % 3] = 1; f@a = @P[i % 3] + getcomp(@P, 1);", { "Cd", "a" } },
        { "int count = 0; for (int i = 0; i < 10; ++i) { if (i % 2 == 0) continue; count += i; } i@odd = count + int(@P.y);",
          { "odd" } },
        { "f@s = sqrt(abs(@P.y)) + exp(-abs(@P.x) * 0.01) + floor(@P.z / 3) + sign(@P.y) + frac(@P.x) + clamp(@P.y, -2, 2);",
          { "s" } },
    };
    return list;
}

bool
compileSnippet(const std::string& source, const GeoKernelParameters& parameters, KernelProgram& program)
{
    const GeoKernelBinding binding(eGeoAttribClassPoint, &parameters);
    KernelCompileError error;
    const bool ok = kernelCompile(source, binding, program, error);
    EXPECT_TRUE(ok) << source << "\n"
                    << error.line << ":" << error.column << ": " << error.message;
    return ok;
}

std::vector<double>
values(const GeoAttribute& a)
{
    std::vector<double> out(a.size() * std::size_t(a.tupleSize()));
    if (a.storage() == eGeoStorageInt32) {
        std::vector<std::int32_t> v(out.size());
        a.copyDense<std::int32_t>(0, a.size(), std::span<std::int32_t>(v));
        std::copy(v.begin(), v.end(), out.begin());
    } else if (a.storage() == eGeoStorageFloat32) {
        std::vector<float> v(out.size());
        a.copyDense<float>(0, a.size(), std::span<float>(v));
        std::copy(v.begin(), v.end(), out.begin());
    } else {
        ADD_FAILURE() << "unexpected storage for this test";
    }
    return out;
}

void
expectSame(const GeoDetail& cpu, const GeoDetail& gpu, const Snippet& s)
{
    ASSERT_EQ(cpu.pointCount(), gpu.pointCount()) << s.source;
    for (const std::string& name : s.attributes) {
        const GeoAttribute* a = cpu.findAttribute(eGeoAttribClassPoint, name);
        const GeoAttribute* b = gpu.findAttribute(eGeoAttribClassPoint, name);
        ASSERT_NE(nullptr, a) << s.source << ": " << name;
        ASSERT_NE(nullptr, b) << s.source << ": " << name;
        ASSERT_EQ(a->storage(), b->storage()) << s.source << ": " << name;
        const std::vector<double> x = values(*a);
        const std::vector<double> y = values(*b);
        ASSERT_EQ(x.size(), y.size()) << s.source << ": " << name;
        const bool exact = a->storage() == eGeoStorageInt32;
        std::size_t mismatches = 0;
        std::size_t first = 0;
        for (std::size_t k = 0; k < x.size(); ++k) {
            bool same;
            if (exact) {
                same = x[k] == y[k];
            } else if (std::isnan(x[k]) || std::isnan(y[k])) {
                same = std::isnan(x[k]) && std::isnan(y[k]);
            } else {
                const double limit = std::max(kAbsoluteTolerance, kRelativeTolerance * std::max(std::fabs(x[k]), std::fabs(y[k])));
                same = std::fabs(x[k] - y[k]) <= limit;
            }
            if (!same && mismatches++ == 0) {
                first = k;
            }
        }
        EXPECT_EQ(0u, mismatches) << s.source << ": " << name << " first differs at " << first << ": cpu " << x[first]
                                  << " gpu " << y[first];
    }
}

GeoKernelRunOptions
runOptions(const GeoKernelParameters& parameters)
{
    GeoKernelRunOptions options;
    options.frame = 12.;
    options.time = 0.5;
    options.parameters = &parameters;
    return options;
}

} // namespace

TEST(GeoWrangleGpuTest, ReferenceSnippetsMatchTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const GeoKernelParameters parameters = testParameters();
    const GeoDetail input = makePoints(kPointCount);
    const GeoDetail* inputs[] = { &input };
    const GeoKernelRunOptions options = runOptions(parameters);
    for (const Snippet& s : snippets()) {
        KernelProgram program;
        if (!compileSnippet(s.source, parameters, program)) {
            continue;
        }
        const KernelGpuPlan plan = kernelGpuPlan(program, geoKernelGpuTarget());
        ASSERT_TRUE(plan.runsOnGpu()) << s.source << ": " << plan.cpuOnlyReason;
        const GeoKernelRunResult cpu = geoKernelRun(program, eGeoAttribClassPoint, std::span<const GeoDetail* const>(inputs), options);
        ASSERT_EQ(KernelRunStatus::eDone, cpu.status) << s.source << ": " << cpu.error;
        const GeoKernelGpuRunResult gpu = geoKernelRunGpu(program, plan, eGeoAttribClassPoint, std::span<const GeoDetail* const>(inputs),
                                                          options, &runtime->runtime());
        ASSERT_EQ(KernelRunStatus::eDone, gpu.result.status) << s.source << ": " << gpu.result.error;
        EXPECT_TRUE(gpu.ranOnGpu) << s.source << ": " << gpu.fallback;
        EXPECT_TRUE(gpu.fallback.empty()) << s.source << ": " << gpu.fallback;
        expectSame(cpu.detail, gpu.result.detail, s);
    }
}

TEST(GeoWrangleGpuTest, AGroupRunsOnlyItsMembers)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const GeoKernelParameters parameters = testParameters();
    const GeoDetail source = makePoints(kPointCount);
    const GeoDetail* first[] = { &source };
    GeoKernelRunOptions options = runOptions(parameters);
    const GeoKernelRunResult grouped = geoKernelRun("if (@ptnum % 3 == 1) setpointgroup(0, \"odd\", @ptnum, 1);", eGeoAttribClassPoint,
                                                    std::span<const GeoDetail* const>(first), options);
    ASSERT_EQ(KernelRunStatus::eDone, grouped.status) << grouped.error;
    const GeoDetail* inputs[] = { &grouped.detail };
    options.group = "odd";
    const Snippet s { "@P.y = @ptnum * 2 + 0.5;", { "P" } };
    KernelProgram program;
    ASSERT_TRUE(compileSnippet(s.source, parameters, program));
    const KernelGpuPlan plan = kernelGpuPlan(program, geoKernelGpuTarget());
    ASSERT_TRUE(plan.runsOnGpu()) << plan.cpuOnlyReason;
    const GeoKernelRunResult cpu = geoKernelRun(program, eGeoAttribClassPoint, std::span<const GeoDetail* const>(inputs), options);
    ASSERT_EQ(KernelRunStatus::eDone, cpu.status) << cpu.error;
    const GeoKernelGpuRunResult gpu = geoKernelRunGpu(program, plan, eGeoAttribClassPoint, std::span<const GeoDetail* const>(inputs),
                                                      options, &runtime->runtime());
    ASSERT_EQ(KernelRunStatus::eDone, gpu.result.status) << gpu.result.error;
    EXPECT_TRUE(gpu.ranOnGpu) << gpu.fallback;
    expectSame(cpu.detail, gpu.result.detail, s);
}

TEST(GeoWrangleGpuTest, ASmallBufferLimitSplitsTheRunIntoPageChunks)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const GeoKernelParameters parameters = testParameters();
    const GeoKernelRunOptions options = runOptions(parameters);
    const Snippet s { "@P.y += @ptnum * 0.5;", { "P" } };
    KernelProgram program;
    ASSERT_TRUE(compileSnippet(s.source, parameters, program));
    const KernelGpuPlan plan = kernelGpuPlan(program, geoKernelGpuTarget());
    ASSERT_TRUE(plan.runsOnGpu()) << plan.cpuOnlyReason;

    const std::uint64_t pageBytes = std::uint64_t(plan.columns.size()) * 4u * kGeoPageSize;
    std::uint64_t limit = pageBytes;
    while (gpu::largestSizeWithin(limit) < pageBytes) {
        limit += pageBytes / 8 + 1;
    }

    const std::uint64_t bytesPerElement = std::uint64_t(plan.columns.size()) * 4u;
    std::uint64_t chunkCount = gpu::largestSizeWithin(limit) / bytesPerElement;
    chunkCount -= chunkCount % kGeoPageSize;
    ASSERT_GE(chunkCount, std::uint64_t(kGeoPageSize));
    // Two whole chunks and a partial page-sized tail, so the last chunk is a remainder.
    const GeoDetail input = makePoints(GeoIndex(2 * chunkCount + kGeoPageSize / 2 + 3));
    ASSERT_GT(std::uint64_t(input.pointCount()), chunkCount);
    const GeoDetail* inputs[] = { &input };

    gpu::GpuRecordContext::setMaxStorageBufferBytesForTests(limit);
    const GeoKernelGpuRunResult gpu = geoKernelRunGpu(program, plan, eGeoAttribClassPoint, std::span<const GeoDetail* const>(inputs),
                                                      options, &runtime->runtime());
    gpu::GpuRecordContext::setMaxStorageBufferBytesForTests(0);

    ASSERT_EQ(KernelRunStatus::eDone, gpu.result.status) << gpu.result.error;
    EXPECT_TRUE(gpu.ranOnGpu) << gpu.fallback;
    EXPECT_GE(gpu.result.batches, 3u);
    const GeoKernelRunResult cpu = geoKernelRun(program, eGeoAttribClassPoint, std::span<const GeoDetail* const>(inputs), options);
    ASSERT_EQ(KernelRunStatus::eDone, cpu.status) << cpu.error;
    expectSame(cpu.detail, gpu.result.detail, s);
}

TEST(GeoWrangleGpuTest, AddPointRunsOnTheCpuAndSaysWhy)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const GeoKernelParameters parameters = testParameters();
    const GeoDetail input = makePoints(kPointCount);
    const GeoDetail* inputs[] = { &input };
    const GeoKernelRunOptions options = runOptions(parameters);
    KernelProgram program;
    ASSERT_TRUE(compileSnippet("if (@ptnum < 10) addpoint(0, @P + {0, 1, 0});", parameters, program));
    const KernelGpuPlan plan = kernelGpuPlan(program, geoKernelGpuTarget());
    EXPECT_FALSE(plan.runsOnGpu());
    EXPECT_EQ("addpoint", plan.cpuOnlyName);

    const GeoKernelGpuRunResult gpu = geoKernelRunGpu(program, plan, eGeoAttribClassPoint, std::span<const GeoDetail* const>(inputs),
                                                      options, &runtime->runtime());
    ASSERT_EQ(KernelRunStatus::eDone, gpu.result.status) << gpu.result.error;
    EXPECT_FALSE(gpu.ranOnGpu);
    EXPECT_NE(std::string::npos, gpu.fallback.find("addpoint")) << gpu.fallback;
    EXPECT_EQ(kPointCount + 10, gpu.result.detail.pointCount());
}

#else

TEST(GeoWrangleGpuTest, ReferenceSnippetsMatchTheCpu)
{
    reportNoGpu("built without GPU support");
}

#endif // NATRON_HAVE_GPU
