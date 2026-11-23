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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoKernelBinding.h"
#include "Engine/Kernel/KernelGpuCodegen.h"
#include "Engine/Kernel/KernelParser.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuShaderCompiler.h"
#endif

NATRON_NAMESPACE_USING

namespace {

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
    GeoKernelParameter label;
    label.kind = GeoKernelParameterKind::eString;
    label.string = "x";
    parameters["label"] = label;
    return parameters;
}

KernelGpuPlan
planFor(const std::string& source, GeoAttribClassEnum runOver = eGeoAttribClassPoint)
{
    const GeoKernelParameters parameters = testParameters();
    const GeoKernelBinding binding(runOver, &parameters);
    KernelProgram program;
    KernelCompileError error;
    EXPECT_TRUE(kernelCompile(source, binding, program, error)) << source << "\n"
                                                                << error.line << ":" << error.column << ": " << error.message;
    return kernelGpuPlan(program, geoKernelGpuTarget());
}

const char* const kGpuSnippets[] = {
    "@P.y += 1;",
    "if (@ptnum >= 1100 && @ptnum < 1200) @P.y += 1; i@id = 7;",
    "for (int k = 0; k < @ptnum % 7; k++) f@acc += k * 0.5;",
    "int i = 0; while (i < 3) { i++; if (i == 2) break; } i@n = i;",
    "int count = 0; for (int i = 0; i < 10; ++i) { if (i % 2 == 0) continue; count += i; } i@odd = count;",
    "float n = noise(@P * 4); vector r = rand(@ptnum); @Cd = r * n;",
    "v@c = noise(@P); f@s = noise(@P.x); f@r = rand(@P.x);",
    "vector4 q = quaternion(radians(45), {0, 1, 0}); @orient = q; @N = qrotate(q, @N);",
    "matrix m = ident(); m = transpose(m) * invert(m); f@det = determinant(m);",
    "@P.x = ch(\"amp\") * @P.x + chi(\"count\"); @P += chv(\"offset\"); i@n = npoints(0); f@t = @Time + @Frame + @numpt;",
    "int a = @ptnum - 500; i@q = a / 7; i@r = a % 7; i@s = a << 3; i@t = a >> 2; int z = @ptnum * 0; i@u = a / z;",
    "f@m = @P.x % 1.5; f@r = round(@P.y); f@a = atan2(@P.y, @P.x); f@p = pow(@P.x, 1.5); f@l = log10(abs(@P.x) + 1);",
    "int i = @ptnum; @Cd[i % 3] = 1; @a = @P[i % 3] + getcomp(@P, 1);",
};

struct CpuOnlyCase {
    const char* source;
    const char* name;
};

const CpuOnlyCase kCpuOnly[] = {
    { "addpoint(0, {0, 0, 0});", "addpoint" },
    { "i@near = nearpoint(0, @P);", "nearpoint" },
    { "@P = v@opinput1_P;", "@opinput1_P" },
    { "s@name = \"a\";", "@name" },
    { "i@x = strlen(chs(\"label\"));", "chs" },
    { "float vals[] = {1, 2, 3}; float sum = 0; foreach (float v; vals) sum += v; @sum = sum;", "array" },
    { "vector p = point(0, \"P\", 0); f@y = p.y;", "point" },
};

} // namespace

TEST(KernelGpuCodegenTest, ReferenceSnippetsLowerToGlsl)
{
    for (const char* source : kGpuSnippets) {
        const KernelGpuPlan plan = planFor(source);
        ASSERT_TRUE(plan.runsOnGpu()) << source << ": " << plan.cpuOnlyReason;
        EXPECT_FALSE(plan.source.empty()) << source;
        EXPECT_FALSE(plan.columns.empty()) << source;
#ifdef NATRON_HAVE_GPU
        const std::shared_ptr<const gpu::ShaderCompileResult> compiled = gpu::compileGlsl(plan.source, gpu::ShaderStage::Compute, "main");
        ASSERT_NE(nullptr, compiled.get());
        EXPECT_TRUE(compiled->ok) << source << "\n"
                                  << compiled->log << "\n"
                                  << plan.source;
        EXPECT_FALSE(compiled->spirv.empty()) << source;
#endif
    }
}

TEST(KernelGpuCodegenTest, UnsupportedBuiltinsNameThemselves)
{
    for (const CpuOnlyCase& c : kCpuOnly) {
        const KernelGpuPlan plan = planFor(c.source);
        EXPECT_FALSE(plan.runsOnGpu()) << c.source;
        EXPECT_EQ(std::string(c.name), plan.cpuOnlyName) << c.source << ": " << plan.cpuOnlyReason;
        EXPECT_NE(std::string::npos, plan.cpuOnlyReason.find(c.name)) << c.source << ": " << plan.cpuOnlyReason;
        EXPECT_TRUE(plan.source.empty()) << c.source;
    }
}

TEST(KernelGpuCodegenTest, ChannelValuesAndCountsBecomeUniforms)
{
    const KernelGpuPlan plan = planFor("@P.x = ch(\"amp\") * @P.x + @numpt;");
    ASSERT_TRUE(plan.runsOnGpu()) << plan.cpuOnlyReason;
    bool call = false;
    bool binding = false;
    for (const KernelGpuUniform& u : plan.uniforms) {
        call = call || u.instruction >= 0;
        binding = binding || u.binding >= 0;
    }
    EXPECT_TRUE(call);
    EXPECT_TRUE(binding);
}

TEST(KernelGpuCodegenTest, OnlyWrittenColumnsAreStored)
{
    const KernelGpuPlan plan = planFor("f@out = @P.x * 2;");
    ASSERT_TRUE(plan.runsOnGpu()) << plan.cpuOnlyReason;
    int loads = 0;
    int stores = 0;
    for (const KernelGpuColumn& c : plan.columns) {
        loads += c.load;
        stores += c.store;
    }
    EXPECT_EQ(3, loads);
    EXPECT_EQ(1, stores);
}
