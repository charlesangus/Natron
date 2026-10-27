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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <OpenColorIO/OpenColorIO.h>

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/OcioVulkanShader.h"
#include "Engine/ProjectColorManagement.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuPipelineCache.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuTexture.h"
#endif

NATRON_NAMESPACE_USING

namespace {

namespace OCIO = OCIO_NAMESPACE;
namespace fs = std::filesystem;

const char* const kStudioURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kWorkingSpace = "ACEScg";
const char* const kDisplay = "sRGB - Display";
const char* const kSDRView = "ACES 2.0 - SDR 100 nits (Rec.709)";

const char* const kFunctionName = "OCIODisplay";
const unsigned kTextureBindingStart = 1;
const unsigned kInputBinding = 24;
const unsigned kOutputBinding = 25;
const unsigned kWidth = 64;
const unsigned kHeight = 40;
const double kTolerance = 1e-4;

#ifdef NATRON_HAVE_GPU

std::vector<float>
makeInput()
{
    std::vector<float> rgba;
    rgba.reserve(std::size_t(kWidth) * kHeight * 4);
    auto push = [&rgba](float r, float g, float b, float a) {
        rgba.push_back(r);
        rgba.push_back(g);
        rgba.push_back(b);
        rgba.push_back(a);
    };
    for (int i = 0; i < 256; ++i) {
        const float t = float(i) / 255.f;
        const float v = -0.5f + 12.f * t * t;
        push(v, v, v, 1.f);
    }
    for (int r = 0; r < 9; ++r) {
        for (int g = 0; g < 9; ++g) {
            for (int b = 0; b < 9; ++b) {
                push(-0.1f + 0.2f * float(r), -0.1f + 0.2f * float(g), -0.1f + 0.2f * float(b), 1.f);
            }
        }
    }
    std::uint32_t state = 12345u;
    auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return -0.25f + 2.75f * float(state >> 8) / float(1u << 24);
    };
    while (rgba.size() < std::size_t(kWidth) * kHeight * 4) {
        push(next(), next(), next(), 0.5f);
    }

    return rgba;
}

void
writeCube(const fs::path& path,
          int dimensions,
          int size)
{
    std::ofstream out(path);
    out << std::setprecision(9);
    out << "LUT_" << dimensions << "D_SIZE " << size << "\n";
    const float last = float(size - 1);
    if (dimensions == 1) {
        for (int i = 0; i < size; ++i) {
            const float x = float(i) / last;
            out << std::pow(x, 1.f / 2.2f) << " " << std::pow(x, 1.f / 2.0f) << " " << std::pow(x, 1.f / 2.4f) << "\n";
        }

        return;
    }
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                const float fr = float(r) / last;
                const float fg = float(g) / last;
                const float fb = float(b) / last;
                out << 0.85f * fr + 0.1f * fg * fb + 0.02f << " " << 0.8f * fg + 0.15f * fr * fr + 0.05f * fb << " "
                    << 0.9f * fb + 0.1f * fr * fg << "\n";
            }
        }
    }
}

// The source space is scene-referred, not data: OCIO skips the whole display transform for a data
// source, which would leave no LUT to bind.
OCIO::ConstProcessorRcPtr
makeLutProcessor(const fs::path& dir,
                 std::string* error)
{
    try {
        writeCube(dir / "shaper.cube", 1, 256);
        writeCube(dir / "grade.cube", 3, 33);
        OCIO::ConfigRcPtr config = OCIO::Config::CreateRaw()->createEditableCopy();
        config->setSearchPath(dir.string().c_str());
        OCIO::ColorSpaceRcPtr source = OCIO::ColorSpace::Create();
        source->setName("scenesrc");
        source->setIsData(false);
        config->addColorSpace(source);
        OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
        OCIO::FileTransformRcPtr shaper = OCIO::FileTransform::Create();
        shaper->setSrc("shaper.cube");
        shaper->setInterpolation(OCIO::INTERP_LINEAR);
        group->appendTransform(shaper);
        OCIO::FileTransformRcPtr grade = OCIO::FileTransform::Create();
        grade->setSrc("grade.cube");
        grade->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
        group->appendTransform(grade);
        OCIO::ColorSpaceRcPtr space = OCIO::ColorSpace::Create();
        space->setName("lutspace");
        space->setIsData(false);
        space->setTransform(group, OCIO::COLORSPACE_DIR_FROM_REFERENCE);
        config->addColorSpace(space);
        config->addDisplayView("LutDisplay", "LutView", "lutspace", "");
        config->validate();
        OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc("scenesrc");
        transform->setDisplay("LutDisplay");
        transform->setView("LutView");

        return config->getProcessor(transform);
    } catch (const std::exception& e) {
        *error = e.what();

        return OCIO::ConstProcessorRcPtr();
    }
}

const char*
typeName(VkDescriptorType type)
{
    switch (type) {
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        return "storage buffer";
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        return "uniform buffer";
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        return "combined image sampler";
    default:
        return "other";
    }
}

VkImageViewType
viewTypeFor(unsigned dimensions)
{
    return (dimensions == 1) ? VK_IMAGE_VIEW_TYPE_1D : ((dimensions == 2) ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_3D);
}

std::string
wrapperSource(const OcioVulkanShader& shader)
{
    return "#version 460\nlayout(local_size_x = 8, local_size_y = 8) in;\n" + shader.glsl()
        + "\nlayout(std430, set = 0, binding = " + std::to_string(kInputBinding)
        + ") readonly buffer InBuf { vec4 px[]; } inBuf;\n"
          "layout(std430, set = 0, binding = "
        + std::to_string(kOutputBinding)
        + ") writeonly buffer OutBuf { vec4 px[]; } outBuf;\n"
          "layout(push_constant) uniform Push { uint width; uint height; } pc;\n"
          "void main() {\n"
          "    if (gl_GlobalInvocationID.x >= pc.width || gl_GlobalInvocationID.y >= pc.height) return;\n"
          "    uint i = gl_GlobalInvocationID.y * pc.width + gl_GlobalInvocationID.x;\n"
          "    outBuf.px[i] = "
        + shader.functionName()
        + "(inBuf.px[i]);\n"
          "}\n";
}

struct Prepared {
    OcioVulkanShader shader;
    std::shared_ptr<gpu::GpuKernel> kernel;
    std::shared_ptr<const OcioVulkanShader::Resources::TextureList> textures;
    const gpu::GpuKernelBinding* block = nullptr;
};

void
prepare(gpu::GpuRuntime& runtime,
        OcioVulkanShader::Resources& resources,
        const OCIO::ConstGPUProcessorRcPtr& gpuProcessor,
        Prepared* out)
{
    std::string error;
    ASSERT_TRUE(OcioVulkanShader::build(gpuProcessor, kFunctionName, kTextureBindingStart, &out->shader, &error)) << error;
    ASSERT_LT(kTextureBindingStart + out->shader.textures().size(), std::size_t(kInputBinding));

    const gpu::GpuStatus compiled = runtime.pipelines().get(wrapperSource(out->shader), gpu::ShaderStage::Compute, "main", out->kernel);
    ASSERT_TRUE(bool(compiled)) << compiled.message;

    for (const gpu::GpuKernelBinding& b : out->kernel->bindings()) {
        std::printf("  binding %-3u %-22s block %u %s\n", b.binding, typeName(b.type), b.blockBytes, b.name.c_str());
        if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            EXPECT_EQ(nullptr, out->block) << "more than one uniform block";
            out->block = &b;
        }
    }
    if (out->shader.uniformBytes() > 0) {
        ASSERT_NE(nullptr, out->block);
        EXPECT_EQ(OcioVulkanShader::kUniformBinding, out->block->binding);
        EXPECT_GE(std::size_t(out->block->blockBytes), out->shader.uniformBytes());
    } else {
        EXPECT_EQ(nullptr, out->block);
    }
    for (const OcioVulkanShader::Texture& t : out->shader.textures()) {
        const gpu::GpuKernelBinding* b = out->kernel->findBinding(t.sampler);
        ASSERT_NE(nullptr, b) << t.sampler;
        EXPECT_EQ(t.binding, b->binding) << t.sampler;
        EXPECT_EQ(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, b->type) << t.sampler;
        EXPECT_EQ(viewTypeFor(t.desc.dimensions), b->viewType) << t.sampler;
    }
    EXPECT_EQ(8u, out->kernel->desc().pushConstantBytes);

    const gpu::GpuStatus acquired = resources.acquire(out->shader, out->textures);
    ASSERT_TRUE(bool(acquired)) << acquired.message;
    ASSERT_EQ(out->shader.textures().size(), out->textures->size());
}

void
dispatch(gpu::GpuRuntime& runtime,
         const Prepared& prepared,
         const std::vector<float>& input,
         std::vector<float>* got)
{
    const VkDeviceSize bytes = input.size() * sizeof(float);
    gpu::GpuBuffer inBuffer;
    gpu::GpuBuffer outBuffer;
    ASSERT_TRUE(bool(gpu::GpuBuffer::createStaging(runtime.device(), bytes, gpu::kGpuBufferDefaultUsage, inBuffer)));
    ASSERT_TRUE(bool(gpu::GpuBuffer::createStaging(runtime.device(), bytes, gpu::kGpuBufferDefaultUsage, outBuffer)));
    std::memcpy(inBuffer.mapped(), input.data(), bytes);
    std::memset(outBuffer.mapped(), 0xFF, bytes);

    std::unique_ptr<gpu::GpuRecordContext> context;
    ASSERT_TRUE(bool(runtime.newRecordContext(context)));
    std::vector<gpu::GpuBinding> bindings;
    bindings.push_back(gpu::GpuBinding::storage(kInputBinding, inBuffer));
    bindings.push_back(gpu::GpuBinding::storage(kOutputBinding, outBuffer));
    if (prepared.block) {
        std::vector<std::byte> blockData(prepared.block->blockBytes, std::byte(0));
        ASSERT_TRUE(prepared.shader.fillUniforms(blockData));
        const gpu::GpuUniformBuffer* ubo = nullptr;
        const gpu::GpuStatus s = context->uniform(blockData, ubo);
        ASSERT_TRUE(bool(s)) << s.message;
        bindings.push_back(gpu::GpuBinding::uniform(prepared.block->binding, *ubo));
    }
    OcioVulkanShader::Resources::appendBindings(prepared.shader, *prepared.textures, bindings);
    const std::uint32_t push[2] = { kWidth, kHeight };
    gpu::GpuStatus s = context->dispatchBindings(prepared.kernel, bindings, std::as_bytes(std::span(push)), { kWidth, kHeight, 1 });
    ASSERT_TRUE(bool(s)) << s.message;
    s = context->submitAndWait();
    ASSERT_TRUE(bool(s)) << s.message;
    got->resize(input.size());
    std::memcpy(got->data(), outBuffer.mapped(), bytes);
    runtime.waitIdle();
}

std::vector<float>
cpuReference(const OCIO::ConstCPUProcessorRcPtr& cpu,
             const std::vector<float>& input)
{
    std::vector<float> expected = input;
    OCIO::PackedImageDesc image(expected.data(), long(kWidth), long(kHeight), OCIO::CHANNEL_ORDERING_RGBA);
    cpu->apply(image);

    return expected;
}

double
maxError(const std::vector<float>& got,
         const std::vector<float>& expected)
{
    double worst = 0.;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const double d = std::fabs(double(got[i]) - double(expected[i]));
        if (!(d <= worst)) {
            worst = d;
        }
    }

    return worst;
}

// OCIO leaves a 1D LUT's interpolation to the sampler's linear filter, whose weights the device
// quantizes to subTexelPrecisionBits: 8 on RADV, where OCIO's CPU interpolates in float. The error
// is up to the largest step between neighbouring texels over 2^bits.
double
linearFilterError(gpu::GpuRuntime& runtime,
                  const OcioVulkanShader& shader)
{
    VkPhysicalDeviceProperties properties {};
    vkGetPhysicalDeviceProperties(runtime.device().physicalDevice(), &properties);
    double largestStep = 0.;
    for (const OcioVulkanShader::Texture& t : shader.textures()) {
        if ((t.desc.dimensions == 3) || (t.desc.filter != gpu::GpuTextureFilter::Linear)) {
            continue;
        }
        const std::size_t channels = t.desc.channels;
        const std::vector<float>& texels = *t.texels;
        for (std::size_t i = channels; i < texels.size(); ++i) {
            largestStep = std::max(largestStep, std::fabs(double(texels[i]) - double(texels[i - channels])));
        }
    }

    return std::ldexp(largestStep, -int(properties.limits.subTexelPrecisionBits));
}

void
runCase(gpu::GpuRuntime& runtime,
        const char* label,
        const OCIO::ConstProcessorRcPtr& processor,
        OcioVulkanShader::Resources& resources,
        Prepared* prepared,
        bool filteredLut = false)
{
    ASSERT_TRUE(bool(processor));
    std::printf("[ocio-vk] %s\n", label);
    prepare(runtime, resources, processor->getDefaultGPUProcessor(), prepared);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }

    const std::vector<float> input = makeInput();
    std::vector<float> got;
    dispatch(runtime, *prepared, input, &got);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }
    ASSERT_EQ(input.size(), got.size());
    const double worst = maxError(got, cpuReference(processor->getDefaultCPUProcessor(), input));
    const double tolerance = filteredLut ? kTolerance + linearFilterError(runtime, prepared->shader) : kTolerance;
    std::printf("  max abs error vs OCIO CPU: %.3g (tolerance %.3g)\n", worst, tolerance);
    EXPECT_LE(worst, tolerance);
}

#endif // NATRON_HAVE_GPU

} // namespace

TEST(OcioVulkanShader, StudioConfigDisplayView)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> holder;
    if (!createRuntime(holder)) {
        return;
    }
    ProjectColorManagement colorManagement;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, colorManagement.load(kStudioURI, std::string(), &error)) << error;
    colorManagement.setWorkingSpace(kWorkingSpace);
    ProjectColorManagement::DisplayProcessorPtr display = colorManagement.getDisplayProcessor(kWorkingSpace, kDisplay, kSDRView, std::string(), &error);
    ASSERT_TRUE(display != nullptr) << error;
    OcioVulkanShader::Resources resources(holder->runtime());
    Prepared prepared;
    runCase(holder->runtime(), "studio-sdr", display->processor, resources, &prepared);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }
#endif
}

TEST(OcioVulkanShader, ShaperAnd3DLutLook)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> holder;
    if (!createRuntime(holder)) {
        return;
    }
    const fs::path dir = fs::temp_directory_path() / ("natron-ocio-vk-luts-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::string error;
    OCIO::ConstProcessorRcPtr processor = makeLutProcessor(dir, &error);
    ASSERT_TRUE(bool(processor)) << error;

    OcioVulkanShader::Resources resources(holder->runtime());
    // Another user of the same runtime: the uploads below are not its own.
    OcioVulkanShader::Resources bystander(holder->runtime());
    Prepared prepared;
    runCase(holder->runtime(), "luts", processor, resources, &prepared, true);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }
    fs::remove_all(dir, ec);

    bool has3D = false;
    bool has1D = false;
    for (const OcioVulkanShader::Texture& t : prepared.shader.textures()) {
        has3D = has3D || t.desc.dimensions == 3;
        has1D = has1D || t.desc.dimensions < 3;
    }
    EXPECT_TRUE(has3D);
    EXPECT_TRUE(has1D);

    // The same tables find the textures the runtime's cache already made.
    const std::size_t created = resources.texturesCreated();
    EXPECT_EQ(prepared.shader.textures().size(), created);
    Prepared again;
    prepare(holder->runtime(), resources, processor->getDefaultGPUProcessor(), &again);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(created, resources.texturesCreated());
    EXPECT_EQ(0u, bystander.texturesCreated());
    // Taken from the cache the first object filled, the same tables upload nothing for the bystander.
    Prepared shared;
    prepare(holder->runtime(), bystander, processor->getDefaultGPUProcessor(), &shared);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(0u, bystander.texturesCreated());
    EXPECT_EQ(created, resources.texturesCreated());
    ASSERT_EQ(prepared.textures->size(), again.textures->size());
    for (std::size_t i = 0; i < prepared.textures->size(); ++i) {
        EXPECT_EQ((*prepared.textures)[i].get(), (*again.textures)[i].get());
    }
    EXPECT_EQ(prepared.shader.cacheID(), again.shader.cacheID());
#endif
}

TEST(OcioVulkanShader, DynamicExposureNeedsNoNewPipeline)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> holder;
    if (!createRuntime(holder)) {
        return;
    }
    gpu::GpuRuntime& runtime = holder->runtime();
    OCIO::ConfigRcPtr config = OCIO::Config::CreateRaw()->createEditableCopy();
    OCIO::ExposureContrastTransformRcPtr transform = OCIO::ExposureContrastTransform::Create();
    transform->setStyle(OCIO::EXPOSURE_CONTRAST_LINEAR);
    transform->setExposure(0.5);
    transform->makeExposureDynamic();
    OCIO::ConstProcessorRcPtr processor = config->getProcessor(transform);
    OCIO::ConstGPUProcessorRcPtr gpuProcessor = processor->getDefaultGPUProcessor();
    OCIO::ConstCPUProcessorRcPtr cpu = processor->getDefaultCPUProcessor();

    OcioVulkanShader::Resources resources(runtime);
    Prepared prepared;
    prepare(runtime, resources, gpuProcessor, &prepared);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }
    EXPECT_GT(prepared.shader.uniformBytes(), 0u);
    ASSERT_NE(nullptr, prepared.block);

    const std::vector<float> input = makeInput();
    std::vector<float> first;
    dispatch(runtime, prepared, input, &first);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }
    EXPECT_LE(maxError(first, cpuReference(cpu, input)), kTolerance);

    const gpu::GpuPipelineCache::Stats before = runtime.pipelines().stats();
    OCIO::DynamicPropertyRcPtr gpuExposure = prepared.shader.dynamicProperty(OCIO::DYNAMIC_PROPERTY_EXPOSURE);
    OCIO::DynamicPropertyRcPtr cpuExposure = cpu->getDynamicProperty(OCIO::DYNAMIC_PROPERTY_EXPOSURE);
    ASSERT_TRUE(gpuExposure != nullptr);
    OCIO::DynamicPropertyValue::AsDouble(gpuExposure)->setValue(1.5);
    OCIO::DynamicPropertyValue::AsDouble(cpuExposure)->setValue(1.5);

    std::vector<float> changed;
    dispatch(runtime, prepared, input, &changed);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }
    EXPECT_LE(maxError(changed, cpuReference(cpu, input)), kTolerance);
    EXPECT_GT(maxError(changed, first), 0.1);

    Prepared rebuilt;
    prepare(runtime, resources, gpuProcessor, &rebuilt);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(prepared.kernel.get(), rebuilt.kernel.get());
    EXPECT_EQ(prepared.shader.glsl(), rebuilt.shader.glsl());
    EXPECT_EQ(prepared.shader.cacheID(), rebuilt.shader.cacheID());

    const gpu::GpuPipelineCache::Stats after = runtime.pipelines().stats();
    EXPECT_EQ(before.compiles, after.compiles);
    EXPECT_EQ(before.diskHits, after.diskHits);
#endif
}

TEST(OcioVulkanShader, IdentityProcessor)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> holder;
    if (!createRuntime(holder)) {
        return;
    }
    OCIO::ConstConfigRcPtr config = OCIO::Config::CreateRaw();
    OCIO::ConstProcessorRcPtr processor = config->getProcessor("raw", "raw");
    OcioVulkanShader::Resources resources(holder->runtime());
    Prepared prepared;
    runCase(holder->runtime(), "identity", processor, resources, &prepared);
    if (::testing::Test::HasFatalFailure()) {
        return;
    }
    EXPECT_TRUE(prepared.shader.textures().empty());
    EXPECT_EQ(0u, prepared.shader.uniformBytes());
    EXPECT_EQ(0u, resources.texturesCreated());
#endif
}
