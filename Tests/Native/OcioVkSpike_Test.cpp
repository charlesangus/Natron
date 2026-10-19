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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <OpenColorIO/OpenColorIO.h>

#include "Tests/Native/NativeGpuParity.h"

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
// OCIO owns binding 0 for its uniform block and counts textures up from kTextureBindingStart.
const unsigned kTextureBindingStart = 1;
const unsigned kInputBinding = 24;
const unsigned kOutputBinding = 25;
const unsigned kWidth = 64;
const unsigned kHeight = 40;
const double kTolerance = 1e-4;

#ifdef NATRON_HAVE_GPU

using Clock = std::chrono::steady_clock;

double
msSince(Clock::time_point begin)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

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

OCIO::ConstProcessorRcPtr
makeLutProcessor(const fs::path& dir,
                 std::string* error)
{
    try {
        writeCube(dir / "shaper.cube", 1, 256);
        writeCube(dir / "grade.cube", 3, 33);
        OCIO::ConfigRcPtr config = OCIO::Config::CreateRaw()->createEditableCopy();
        config->setSearchPath(dir.string().c_str());
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
        space->setTransform(group, OCIO::COLORSPACE_DIR_FROM_REFERENCE);
        config->addColorSpace(space);
        config->addDisplayView("LutDisplay", "LutView", "lutspace", "");
        config->validate();
        OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc("raw");
        transform->setDisplay("LutDisplay");
        transform->setView("LutView");

        return config->getProcessor(transform);
    } catch (const std::exception& e) {
        *error = e.what();

        return OCIO::ConstProcessorRcPtr();
    }
}

std::string
withoutVersionLines(const std::string& text)
{
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        end = (end == std::string::npos) ? text.size() : end + 1;
        if (text.compare(pos, 8, "#version") != 0) {
            out.append(text, pos, end - pos);
        }
        pos = end;
    }

    return out;
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
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        return "sampled image";
    case VK_DESCRIPTOR_TYPE_SAMPLER:
        return "sampler";
    default:
        return "other";
    }
}

const char*
viewTypeName(VkImageViewType type)
{
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_1D:
        return "1D";
    case VK_IMAGE_VIEW_TYPE_2D:
        return "2D";
    case VK_IMAGE_VIEW_TYPE_3D:
        return "3D";
    default:
        return "-";
    }
}

VkImageViewType
viewTypeFor(unsigned dimensions)
{
    return (dimensions == 1) ? VK_IMAGE_VIEW_TYPE_1D : ((dimensions == 2) ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_3D);
}

struct OcioTexture {
    unsigned binding = 0;
    std::string sampler;
    gpu::GpuTextureDesc desc;
    std::vector<float> values;
};

// Writes the uniforms where OCIO says they live in the std140 block. A float array is padded to a
// 16-byte stride in std140, so the stride is taken from the room OCIO left for it.
std::vector<std::byte>
fillUniformBlock(const OCIO::GpuShaderDescRcPtr& desc,
                 std::size_t blockBytes)
{
    std::vector<std::byte> block(std::max<std::size_t>(blockBytes, 16), std::byte(0));
    std::vector<std::size_t> offsets;
    for (unsigned i = 0; i < desc->getNumUniforms(); ++i) {
        OCIO::GpuShaderDesc::UniformData data;
        desc->getUniform(i, data);
        offsets.push_back(data.m_bufferOffset);
    }
    for (unsigned i = 0; i < desc->getNumUniforms(); ++i) {
        OCIO::GpuShaderDesc::UniformData data;
        const char* name = desc->getUniform(i, data);
        std::byte* at = block.data() + data.m_bufferOffset;
        switch (data.m_type) {
        case OCIO::UNIFORM_DOUBLE: {
            const float v = float(data.m_getDouble());
            std::memcpy(at, &v, sizeof(v));
            std::printf("  uniform %-32s double  @%zu = %g\n", name, data.m_bufferOffset, v);
            break;
        }
        case OCIO::UNIFORM_BOOL: {
            const std::uint32_t v = data.m_getBool() ? 1u : 0u;
            std::memcpy(at, &v, sizeof(v));
            std::printf("  uniform %-32s bool    @%zu = %u\n", name, data.m_bufferOffset, v);
            break;
        }
        case OCIO::UNIFORM_FLOAT3: {
            const OCIO::Float3 v = data.m_getFloat3();
            std::memcpy(at, v.data(), sizeof(float) * 3);
            std::printf("  uniform %-32s float3  @%zu\n", name, data.m_bufferOffset);
            break;
        }
        case OCIO::UNIFORM_VECTOR_FLOAT:
        case OCIO::UNIFORM_VECTOR_INT: {
            const bool isFloat = data.m_type == OCIO::UNIFORM_VECTOR_FLOAT;
            const std::size_t count = std::size_t(isFloat ? data.m_vectorFloat.m_getSize() : data.m_vectorInt.m_getSize());
            std::size_t limit = block.size();
            for (std::size_t other : offsets) {
                if (other > data.m_bufferOffset) {
                    limit = std::min(limit, other);
                }
            }
            const std::size_t stride = (count > 0 && limit - data.m_bufferOffset >= count * 16) ? 16 : 4;
            const char* src = isFloat ? reinterpret_cast<const char*>(data.m_vectorFloat.m_getVector())
                                      : reinterpret_cast<const char*>(data.m_vectorInt.m_getVector());
            for (std::size_t k = 0; k < count && data.m_bufferOffset + k * stride + 4 <= block.size(); ++k) {
                std::memcpy(at + k * stride, src + k * 4, 4);
            }
            std::printf("  uniform %-32s %s[%zu] @%zu stride %zu\n", name, isFloat ? "float" : "int", count, data.m_bufferOffset, stride);
            break;
        }
        default:
            ADD_FAILURE() << "unsupported uniform type for " << name;
            break;
        }
    }

    return block;
}

bool
gatherTextures(const OCIO::GpuShaderDescRcPtr& desc,
               std::vector<OcioTexture>* textures)
{
    for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
        const char* name = nullptr;
        const char* sampler = nullptr;
        unsigned width = 0;
        unsigned height = 0;
        OCIO::GpuShaderDesc::TextureType channel;
        OCIO::GpuShaderDesc::TextureDimensions dimensions;
        OCIO::Interpolation interpolation;
        desc->getTexture(i, name, sampler, width, height, channel, dimensions, interpolation);
        const float* values = nullptr;
        desc->getTextureValues(i, values);
        OcioTexture t;
        t.binding = desc->getTextureShaderBindingIndex(i);
        t.sampler = sampler;
        t.desc.dimensions = (dimensions == OCIO::GpuShaderDesc::TEXTURE_1D) ? 1 : 2;
        t.desc.width = width;
        t.desc.height = height;
        t.desc.channels = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL) ? 1 : 3;
        t.desc.filter = (interpolation == OCIO::INTERP_NEAREST) ? gpu::GpuTextureFilter::Nearest : gpu::GpuTextureFilter::Linear;
        t.values.assign(values, values + std::size_t(width) * height * t.desc.channels);
        std::printf("  texture %-28s binding %u %uD %ux%u %uch %s\n", sampler, t.binding, t.desc.dimensions, width, height,
                    t.desc.channels, (t.desc.filter == gpu::GpuTextureFilter::Nearest) ? "nearest" : "linear");
        textures->push_back(std::move(t));
    }
    for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
        const char* name = nullptr;
        const char* sampler = nullptr;
        unsigned edge = 0;
        OCIO::Interpolation interpolation;
        desc->get3DTexture(i, name, sampler, edge, interpolation);
        const float* values = nullptr;
        desc->get3DTextureValues(i, values);
        OcioTexture t;
        t.binding = desc->get3DTextureShaderBindingIndex(i);
        t.sampler = sampler;
        t.desc.dimensions = 3;
        t.desc.width = t.desc.height = t.desc.depth = edge;
        t.desc.channels = 3;
        t.desc.filter = (interpolation == OCIO::INTERP_NEAREST) ? gpu::GpuTextureFilter::Nearest : gpu::GpuTextureFilter::Linear;
        t.values.assign(values, values + std::size_t(edge) * edge * edge * 3);
        std::printf("  texture %-28s binding %u 3D %u^3 3ch %s\n", sampler, t.binding, edge,
                    (t.desc.filter == gpu::GpuTextureFilter::Nearest) ? "nearest" : "linear");
        textures->push_back(std::move(t));
    }

    return true;
}

std::string
wrapperSource(const std::string& ocioText,
              const std::string& tag)
{
    return "#version 460\n// " + tag + "\nlayout(local_size_x = 8, local_size_y = 8) in;\n" + withoutVersionLines(ocioText)
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
        + std::string(kFunctionName)
        + "(inBuf.px[i]);\n"
          "}\n";
}

void
runCase(gpu::GpuRuntime& runtime,
        const char* label,
        const OCIO::ConstProcessorRcPtr& processor)
{
    ASSERT_TRUE(bool(processor));
    std::printf("[ocio-vk] %s\n", label);

    OCIO::GpuShaderDescRcPtr desc = OCIO::GpuShaderDesc::CreateShaderDesc();
    desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_VK_4_6);
    desc->setFunctionName(kFunctionName);
    desc->setResourcePrefix("ocio_");
    desc->setDescriptorSetIndex(0, kTextureBindingStart);
    OCIO::ConstGPUProcessorRcPtr gpuProcessor = processor->getDefaultGPUProcessor();
    gpuProcessor->extractGpuShaderInfo(desc);

    std::vector<OcioTexture> textures;
    gatherTextures(desc, &textures);
    ASSERT_LT(kTextureBindingStart + textures.size(), std::size_t(kInputBinding));

    const std::string tag = "ocio-vk-spike " + std::string(label) + " " + std::to_string(::getpid()) + " "
        + std::to_string(Clock::now().time_since_epoch().count());
    const std::string source = wrapperSource(desc->getShaderText(), tag);
    std::printf("  ocio shader text: %zu bytes, %zu uniforms (block %zu bytes), %zu textures\n", std::strlen(desc->getShaderText()),
                std::size_t(desc->getNumUniforms()), std::size_t(desc->getUniformBufferSize()), textures.size());

    const fs::path cacheDir = fs::temp_directory_path() / ("natron-ocio-vk-spike-" + std::to_string(::getpid()) + "-" + label);
    std::error_code ec;
    fs::remove_all(cacheDir, ec);
    gpu::GpuPipelineCacheOptions cacheOptions;
    cacheOptions.directory = cacheDir.string();

    std::shared_ptr<gpu::GpuKernel> kernel;
    {
        gpu::GpuPipelineCache cold(runtime.device(), cacheOptions);
        const Clock::time_point begin = Clock::now();
        const gpu::GpuStatus s = cold.get(source, gpu::ShaderStage::Compute, "main", kernel);
        const double ms = msSince(begin);
        ASSERT_TRUE(bool(s)) << s.message;
        std::printf("  cold compile: %.1f ms (compiles %zu)\n", ms, cold.stats().compiles);
    }
    {
        gpu::GpuPipelineCache warm(runtime.device(), cacheOptions);
        std::shared_ptr<gpu::GpuKernel> reloaded;
        const Clock::time_point begin = Clock::now();
        const gpu::GpuStatus s = warm.get(source, gpu::ShaderStage::Compute, "main", reloaded);
        const double ms = msSince(begin);
        ASSERT_TRUE(bool(s)) << s.message;
        std::printf("  warm disk load: %.1f ms (disk hits %zu, compiles %zu)\n", ms, warm.stats().diskHits, warm.stats().compiles);
        EXPECT_EQ(1u, warm.stats().diskHits);
    }
    fs::remove_all(cacheDir, ec);

    const gpu::GpuKernelBinding* block = nullptr;
    for (const gpu::GpuKernelBinding& b : kernel->bindings()) {
        std::printf("  binding %-3u %-22s %-10s %-24s block %u\n", b.binding, typeName(b.type), viewTypeName(b.viewType), b.name.c_str(),
                    b.blockBytes);
        if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            EXPECT_EQ(nullptr, block) << "more than one uniform block";
            block = &b;
        }
    }
    if (desc->getUniformBufferSize() > 0) {
        ASSERT_NE(nullptr, block);
        EXPECT_EQ(0u, block->binding);
        EXPECT_GE(std::size_t(block->blockBytes), std::size_t(desc->getUniformBufferSize()));
    }
    for (const OcioTexture& t : textures) {
        const gpu::GpuKernelBinding* b = kernel->findBinding(t.sampler);
        ASSERT_NE(nullptr, b) << t.sampler;
        EXPECT_EQ(t.binding, b->binding) << t.sampler;
        EXPECT_EQ(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, b->type) << t.sampler;
        EXPECT_EQ(viewTypeFor(t.desc.dimensions), b->viewType) << t.sampler;
    }
    EXPECT_EQ(8u, kernel->desc().pushConstantBytes);

    const std::vector<float> input = makeInput();
    const std::size_t pixels = std::size_t(kWidth) * kHeight;
    const VkDeviceSize bytes = input.size() * sizeof(float);
    gpu::GpuBuffer inBuffer;
    gpu::GpuBuffer outBuffer;
    ASSERT_TRUE(bool(gpu::GpuBuffer::createStaging(runtime.device(), bytes, gpu::kGpuBufferDefaultUsage, inBuffer)));
    ASSERT_TRUE(bool(gpu::GpuBuffer::createStaging(runtime.device(), bytes, gpu::kGpuBufferDefaultUsage, outBuffer)));
    std::memcpy(inBuffer.mapped(), input.data(), bytes);

    std::vector<std::byte> blockData;
    if (block) {
        blockData = fillUniformBlock(desc, std::max<std::size_t>(block->blockBytes, desc->getUniformBufferSize()));
    }

    std::vector<float> got(input.size());
    double bestMs = std::numeric_limits<double>::max();
    for (int run = 0; run < 3; ++run) {
        std::memset(outBuffer.mapped(), 0xFF, bytes);
        const Clock::time_point begin = Clock::now();
        std::unique_ptr<gpu::GpuRecordContext> context;
        ASSERT_TRUE(bool(runtime.newRecordContext(context)));
        std::vector<gpu::GpuBinding> bindings;
        bindings.push_back(gpu::GpuBinding::storage(kInputBinding, inBuffer));
        bindings.push_back(gpu::GpuBinding::storage(kOutputBinding, outBuffer));
        if (block) {
            const gpu::GpuUniformBuffer* ubo = nullptr;
            const gpu::GpuStatus s = context->uniform(blockData, ubo);
            ASSERT_TRUE(bool(s)) << s.message;
            bindings.push_back(gpu::GpuBinding::uniform(block->binding, *ubo));
        }
        for (const OcioTexture& t : textures) {
            const gpu::GpuTexture* texture = nullptr;
            const gpu::GpuStatus s = context->texture(t.desc, t.values, texture);
            ASSERT_TRUE(bool(s)) << t.sampler << ": " << s.message;
            bindings.push_back(gpu::GpuBinding::texture(t.binding, *texture));
        }
        const std::uint32_t push[2] = { kWidth, kHeight };
        gpu::GpuStatus s = context->dispatchBindings(kernel, bindings, std::as_bytes(std::span(push)), { kWidth, kHeight, 1 });
        ASSERT_TRUE(bool(s)) << s.message;
        s = context->submitAndWait();
        ASSERT_TRUE(bool(s)) << s.message;
        bestMs = std::min(bestMs, msSince(begin));
        std::memcpy(got.data(), outBuffer.mapped(), bytes);
    }
    std::printf("  dispatch %zu px incl. uniform and LUT upload: best of 3 %.2f ms\n", pixels, bestMs);

    std::vector<float> expected = input;
    processor->getDefaultCPUProcessor()->applyRGBA(expected.data());
    double worst = 0.;
    std::size_t worstAt = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const double d = std::fabs(double(got[i]) - double(expected[i]));
        if (!(d <= worst)) {
            worst = d;
            worstAt = i;
        }
    }
    std::printf("  max abs error vs OCIO CPU: %.3g at pixel %zu channel %zu (input %g, cpu %g, gpu %g)\n", worst, worstAt / 4, worstAt % 4,
                input[worstAt], expected[worstAt], got[worstAt]);
    EXPECT_LE(worst, kTolerance);

    runtime.waitIdle();
}

#endif // NATRON_HAVE_GPU

} // namespace

TEST(OcioVkSpike, StudioConfigDisplayView)
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
    runCase(holder->runtime(), "studio-sdr", display->processor);
#endif
}

TEST(OcioVkSpike, DisplayWithShaperAnd3DLut)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> holder;
    if (!createRuntime(holder)) {
        return;
    }
    const fs::path dir = fs::temp_directory_path() / ("natron-ocio-vk-spike-luts-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::string error;
    OCIO::ConstProcessorRcPtr processor = makeLutProcessor(dir, &error);
    ASSERT_TRUE(bool(processor)) << error;
    runCase(holder->runtime(), "luts", processor);
    fs::remove_all(dir, ec);
#endif
}
