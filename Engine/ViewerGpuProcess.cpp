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

#include "ViewerGpuProcess.h"

#ifdef NATRON_HAVE_GPU

#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <span>
#include <vector>

#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuPipelineCache.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kOcioFunction = "OCIODisplay";
const unsigned kOcioFirstTextureBinding = 1;

// Mirrors the push-constant block of kernelSource(), every member 4 bytes so that std430 and C++
// agree on the offsets.
struct ViewerPush {
    int32_t rectX1;
    int32_t rectY1;
    uint32_t width;
    uint32_t height;
    int32_t boundsX1;
    int32_t boundsY1;
    int32_t boundsX2;
    int32_t boundsY2;
    uint32_t inOffset;
    int32_t rowStride;
    int32_t nComps;
    int32_t rOffset;
    int32_t gOffset;
    int32_t bOffset;
    int32_t luminance;
    int32_t matteIndex;
    float gain;
    float offset;
    int32_t gammaMode;
    float invGamma;
};
static_assert(sizeof(ViewerPush) == 80, "ViewerPush must match the shader's push-constant block");

enum GammaModeEnum {
    eGammaModeThreshold = 0,
    eGammaModeIdentity = 1,
    eGammaModePow = 2,
};

const char* const kPushBlock = "layout(push_constant) uniform ViewerPush {\n"
                               "    int rectX1; int rectY1; uint width; uint height;\n"
                               "    int boundsX1; int boundsY1; int boundsX2; int boundsY2;\n"
                               "    uint inOffset; int rowStride; int nComps; int rOffset;\n"
                               "    int gOffset; int bOffset; int luminance; int matteIndex;\n"
                               "    float gain; float offset; int gammaMode; float invGamma;\n"
                               "} pc;\n";

// The fetch follows scaleToTexture8bits_generic and scaleToTexture32bitsGeneric case by case,
// including which components a short pixel leaves at 0 or 1.
const char* const kFetch = "float viewerComp(uint base, int c) {\n"
                           "    return (c < pc.nComps) ? viewerIn.px[base + uint(c)] : 0.0;\n"
                           "}\n"
                           "vec4 viewerFetch(int x, int y) {\n"
                           "    bool inside = x >= pc.boundsX1 && x < pc.boundsX2 && y >= pc.boundsY1 && y < pc.boundsY2;\n"
                           "    if (!inside) {\n"
                           "        return vec4(0.0, 0.0, 0.0, (!kEightBit && pc.nComps < 4) ? 1.0 : 0.0);\n"
                           "    }\n"
                           "    uint base = pc.inOffset + uint(y - pc.boundsY1) * uint(pc.rowStride) + uint((x - pc.boundsX1) * pc.nComps);\n"
                           "    float r = viewerComp(base, pc.rOffset);\n"
                           "    float g = viewerComp(base, pc.gOffset);\n"
                           "    float b = 0.0;\n"
                           "    float a = 1.0;\n"
                           "    if (pc.nComps >= 4) {\n"
                           "        b = viewerComp(base, pc.bOffset);\n"
                           "        a = viewerIn.px[base + 3u];\n"
                           "    } else if (pc.nComps == 3) {\n"
                           "        b = viewerComp(base, pc.bOffset);\n"
                           "    } else if (pc.nComps == 1) {\n"
                           "        g = r;\n"
                           "        b = r;\n"
                           "    }\n"
                           "    return vec4(r, g, b, a);\n"
                           "}\n"
                           "float viewerMatte(vec4 c) {\n"
                           "    switch (pc.matteIndex) {\n"
                           "    case 0: return c.r;\n"
                           "    case 1: return c.g;\n"
                           "    case 2: return c.b;\n"
                           "    case 3: return c.a;\n"
                           "    default: return 0.0;\n"
                           "    }\n"
                           "}\n";

// Unfused like the CPU's float arithmetic (built without FMA), so that a value on a rounding edge
// lands on the same code.
const char* const kEightBitHelpers = "uint viewerFloatToInt0xff01(float v) {\n"
                                     "    if (!(v > 0.0)) return 0u;\n"
                                     "    if (v >= 1.0) return 0xff00u;\n"
                                     "    precise float s = v * 65280.0 + 0.5;\n"
                                     "    return uint(s);\n"
                                     "}\n"
                                     "uint viewerFloatToInt256(float v) {\n"
                                     "    if (!(v > 0.0)) return 0u;\n"
                                     "    if (v >= 1.0) return 255u;\n"
                                     "    precise float s = v * 255.0 + 0.5;\n"
                                     "    return uint(s);\n"
                                     "}\n"
                                     "uint viewerHash(uvec2 p) {\n"
                                     "    uint h = (p.x * 0x9e3779b9u) ^ ((p.y + 0x7f4a7c15u) * 0x85ebca6bu);\n"
                                     "    h ^= h >> 16;\n"
                                     "    h *= 0x7feb352du;\n"
                                     "    h ^= h >> 15;\n"
                                     "    h *= 0x846ca68bu;\n"
                                     "    h ^= h >> 16;\n"
                                     "    return h;\n"
                                     "}\n"
                                     "vec3 viewerGamma(vec3 c) {\n"
                                     "    if (pc.gammaMode == 0) {\n"
                                     "        return vec3(greaterThanEqual(c, vec3(kThreshold)));\n"
                                     "    }\n"
                                     "    if (pc.gammaMode == 2) {\n"
                                     "        return pow(max(c, vec3(0.0)), vec3(pc.invGamma));\n"
                                     "    }\n"
                                     "    return c;\n"
                                     "}\n";

// The CPU diffuses each row's error serially from a random start; a dither that depends only on
// the pixel's position keeps every invocation independent and lands on the same two codes the
// error diffusion can pick, floor(v16 / 256) and the one above.
const char* const kEightBitMain = "void main() {\n"
                                  "    uvec2 gid = gl_GlobalInvocationID.xy;\n"
                                  "    if (gid.x >= pc.width || gid.y >= pc.height) return;\n"
                                  "    int x = pc.rectX1 + int(gid.x);\n"
                                  "    int y = pc.rectY1 + int(gid.y);\n"
                                  "    vec4 c = viewerFetch(x, y);\n"
                                  "    float m = viewerMatte(c);\n"
                                  "    if (pc.luminance != 0) {\n"
                                  "        precise float l = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;\n"
                                  "        c.rgb = vec3(l);\n"
                                  "    }\n"
                                  "    uint alphaCode = viewerFloatToInt256(c.a);\n"
                                  "    precise vec3 graded = c.rgb * pc.gain + pc.offset;\n"
                                  "    vec3 shown = viewerGamma(OCIODisplay(vec4(graded, c.a)).rgb);\n"
                                  "    uint dither = viewerHash(uvec2(ivec2(x, y))) & 0xffu;\n"
                                  "    uint r = (viewerFloatToInt0xff01(shown.r) + dither) >> 8;\n"
                                  "    uint g = (viewerFloatToInt0xff01(shown.g) + dither) >> 8;\n"
                                  "    uint b = (viewerFloatToInt0xff01(shown.b) + dither) >> 8;\n"
                                  "    if (pc.matteIndex >= 0) {\n"
                                  "        float matteShown = OCIODisplay(vec4(m, m, m, 1.0)).r;\n"
                                  "        r = min(255u, r + viewerFloatToInt256(matteShown) / 2u);\n"
                                  "    }\n"
                                  "    viewerOut.px[gid.y * pc.width + gid.x] = (alphaCode << 24) | (r << 16) | (g << 8) | b;\n"
                                  "}\n";

const char* const kFloatMain = "void main() {\n"
                               "    uvec2 gid = gl_GlobalInvocationID.xy;\n"
                               "    if (gid.x >= pc.width || gid.y >= pc.height) return;\n"
                               "    vec4 c = viewerFetch(pc.rectX1 + int(gid.x), pc.rectY1 + int(gid.y));\n"
                               "    if (pc.luminance != 0) {\n"
                               "        c.rgb = vec3(0.299 * c.r + 0.587 * c.g + 0.114 * c.b);\n"
                               "    }\n"
                               "    if (pc.matteIndex >= 0) {\n"
                               "        c.r += viewerMatte(c) * 0.5;\n"
                               "    }\n"
                               "    viewerOut.px[gid.y * pc.width + gid.x] = c;\n"
                               "}\n";

const char* const kPassThroughDisplay = "vec4 OCIODisplay(vec4 c) { return c; }\n";

void
channelOffsets(DisplayChannelsEnum channels,
               int alphaChannelIndex,
               int* r,
               int* g,
               int* b)
{
    int o[3] = { 0, 0, 0 };
    switch (channels) {
    case eDisplayChannelsRGB:
    case eDisplayChannelsY:
    case eDisplayChannelsMatte:
        o[1] = 1;
        o[2] = 2;
        break;
    case eDisplayChannelsG:
        o[0] = o[1] = o[2] = 1;
        break;
    case eDisplayChannelsB:
        o[0] = o[1] = o[2] = 2;
        break;
    case eDisplayChannelsA:
        o[0] = o[1] = o[2] = (alphaChannelIndex >= 0 && alphaChannelIndex <= 2) ? alphaChannelIndex : 3;
        break;
    case eDisplayChannelsR:
    default:
        break;
    }
    *r = o[0];
    *g = o[1];
    *b = o[2];
}

gpu::GpuStatus
failure(const std::string& message)
{
    gpu::GpuStatus s;
    s.result = VK_ERROR_INITIALIZATION_FAILED;
    s.message = message;

    return s;
}

} // namespace

struct ViewerGpuProcess::Pipeline {
    OcioVulkanShader shader;
    bool hasOcio = false;
    std::shared_ptr<gpu::GpuKernel> kernel;
    std::shared_ptr<const OcioVulkanShader::Resources::TextureList> textures;
    gpu::GpuUniformBuffer ubo;
    uint32_t uboBinding = 0;
};

ViewerGpuProcess::ViewerGpuProcess(gpu::GpuRuntime& runtime)
    : _runtime(runtime)
    , _mutex()
    , _resources(runtime)
    , _pipelines()
    , _failures()
{
}

ViewerGpuProcess::~ViewerGpuProcess()
{
}

std::string
ViewerGpuProcess::kernelSource(gpu::GlPixelFormat format,
                               const std::string& ocioGlsl)
{
    const bool eightBit = (format == gpu::GlPixelFormat::Bgra8);
    std::string s = "#version 460\nlayout(local_size_x = 16, local_size_y = 16) in;\n";
    s += eightBit ? "const bool kEightBit = true;\n" : "const bool kEightBit = false;\n";
    if (eightBit) {
        s += ocioGlsl.empty() ? std::string(kPassThroughDisplay) : ocioGlsl;
        // OCIO's GPU renderer lands a transform's clamped white a few ulps under the 1.0 its CPU
        // renderer lands on, which must not flip the gamma 0 threshold.
        s += ocioGlsl.empty() ? "\nconst float kThreshold = 1.0;\n" : "\nconst float kThreshold = 0.99999;\n";
    }
    s += "layout(std430, set = 0, binding = " + std::to_string(kInputBinding) + ") readonly buffer ViewerIn { float px[]; } viewerIn;\n";
    s += "layout(std430, set = 0, binding = " + std::to_string(kOutputBinding) + ") writeonly buffer ViewerOut { "
        + std::string(eightBit ? "uint" : "vec4") + " px[]; } viewerOut;\n";
    s += kPushBlock;
    s += kFetch;
    if (eightBit) {
        s += kEightBitHelpers;
        s += kEightBitMain;
    } else {
        s += kFloatMain;
    }

    return s;
}

gpu::GpuStatus
ViewerGpuProcess::compile(gpu::GpuRuntime& runtime,
                          gpu::GlPixelFormat format,
                          const std::string& ocioGlsl,
                          std::shared_ptr<gpu::GpuKernel>& out)
{
    gpu::GpuStatus s = runtime.pipelines().get(kernelSource(format, ocioGlsl), gpu::ShaderStage::Compute, "main", out);
    if (!s) {
        return s;
    }
    if (out->desc().pushConstantBytes != sizeof(ViewerPush)) {
        out.reset();

        return failure("ViewerGpuProcess: the kernel's push-constant block is not " + std::to_string(sizeof(ViewerPush)) + " bytes");
    }

    return s;
}

gpu::GpuStatus
ViewerGpuProcess::pipelineFor(const ProjectColorManagement::DisplayProcessorPtr& processor,
                              gpu::GlPixelFormat format,
                              std::shared_ptr<const Pipeline>& out)
{
    const bool eightBit = (format == gpu::GlPixelFormat::Bgra8);
    // A no-op processor leaves the CPU's values untouched, so the pass-through kernel matches it exactly, gamma 0's
    // threshold at 1.0 included.
    const bool withOcio = eightBit && processor && processor->processor && !processor->processor->isNoOp();
    std::string key = eightBit ? "8:" : "f:";
    if (withOcio) {
        try {
            key += processor->processor->getCacheID();
        } catch (const std::exception& e) {
            return failure(std::string("ViewerGpuProcess: ") + e.what());
        }
    }

    std::promise<Built> promise;
    {
        std::unique_lock<std::mutex> lock(_mutex);
        const auto found = _pipelines.find(key);
        if (found != _pipelines.end()) {
            out = found->second;

            return gpu::GpuStatus();
        }
        const auto failed = _failures.find(key);
        if (failed != _failures.end()) {
            return failed->second;
        }
        const auto building = _building.find(key);
        if (building != _building.end()) {
            const std::shared_future<Built> pending = building->second;
            lock.unlock();
            const Built& built = pending.get();
            out = built.pipeline;

            return built.status;
        }
        _building.emplace(key, promise.get_future().share());
    }

    Built built;
    try {
        built.status = buildPipeline(processor, format, withOcio, built.pipeline);
    } catch (const std::exception& e) {
        built.status = failure(std::string("ViewerGpuProcess: ") + e.what());
    } catch (...) {
        built.status = failure("ViewerGpuProcess: the pipeline build threw");
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _building.erase(key);
        if (built.status) {
            _pipelines.emplace(key, built.pipeline);
        } else if (built.status.result == VK_ERROR_INITIALIZATION_FAILED) {
            _failures.emplace(key, built.status);
        }
    }
    out = built.pipeline;
    promise.set_value(built);

    return built.status;
}

gpu::GpuStatus
ViewerGpuProcess::buildPipeline(const ProjectColorManagement::DisplayProcessorPtr& processor,
                                gpu::GlPixelFormat format,
                                bool withOcio,
                                std::shared_ptr<const Pipeline>& out)
{
    auto pipeline = std::make_shared<Pipeline>();
    if (withOcio) {
        std::string error;
        try {
            if (!OcioVulkanShader::build(processor->processor->getDefaultGPUProcessor(), kOcioFunction, kOcioFirstTextureBinding, &pipeline->shader, &error)) {
                return failure("ViewerGpuProcess: " + error);
            }
        } catch (const std::exception& e) {
            return failure(std::string("ViewerGpuProcess: ") + e.what());
        }
        for (const OcioVulkanShader::Texture& t : pipeline->shader.textures()) {
            if (t.binding >= kInputBinding) {
                return failure("ViewerGpuProcess: the display transform needs more LUT bindings than fit below the kernel's buffers");
            }
        }
        pipeline->hasOcio = true;
    }

    gpu::GpuStatus s = compile(_runtime, format, pipeline->hasOcio ? pipeline->shader.glsl() : std::string(), pipeline->kernel);
    if (!s) {
        return s;
    }

    if (pipeline->hasOcio) {
        for (const gpu::GpuKernelBinding& b : pipeline->kernel->bindings()) {
            if (b.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                continue;
            }
            std::vector<std::byte> block(b.blockBytes, std::byte(0));
            if (!pipeline->shader.fillUniforms(block)) {
                return failure("ViewerGpuProcess: the display transform has a uniform of an unknown type");
            }
            s = gpu::GpuUniformBuffer::create(_runtime.device(), block, pipeline->ubo);
            if (!s) {
                return s;
            }
            pipeline->uboBinding = b.binding;
        }
        {
            std::lock_guard<std::mutex> lock(_resourcesMutex);
            s = _resources.acquire(pipeline->shader, pipeline->textures);
        }
        if (!s) {
            return s;
        }
    }
    out = pipeline;

    return gpu::GpuStatus();
}

gpu::GpuStatus
ViewerGpuProcess::prepare(const ProjectColorManagement::DisplayProcessorPtr& processor,
                          gpu::GlPixelFormat format)
{
    std::shared_ptr<const Pipeline> pipeline;

    return pipelineFor(processor, format, pipeline);
}

gpu::GpuStatus
ViewerGpuProcess::record(VkCommandBuffer cmd,
                         const GpuImageView& input,
                         VkBuffer outBuffer,
                         const gpu::GlFrame& frame,
                         const ViewerGpuProcessParams& params)
{
    if ((params.rect.width() != (int)frame.width) || (params.rect.height() != (int)frame.height)) {
        return failure("ViewerGpuProcess: the rect is not the frame's size");
    }
    if (!input.buffer || (input.nComps < 1) || (input.nComps > 4)) {
        return failure("ViewerGpuProcess: the input has no buffer or an unsupported component count");
    }
    if ((input.byteOffset % sizeof(float) != 0) || (input.byteOffset / sizeof(float) > std::numeric_limits<uint32_t>::max())) {
        return failure("ViewerGpuProcess: the input's offset is not a float index");
    }
    if ((frame.width == 0) || (frame.height == 0)) {
        return gpu::GpuStatus();
    }

    std::shared_ptr<const Pipeline> pipeline;
    gpu::GpuStatus s = pipelineFor(params.displayProcessor, frame.format, pipeline);
    if (!s) {
        return s;
    }

    ViewerPush push = {};
    push.rectX1 = params.rect.x1;
    push.rectY1 = params.rect.y1;
    push.width = frame.width;
    push.height = frame.height;
    push.boundsX1 = input.bounds.x1;
    push.boundsY1 = input.bounds.y1;
    push.boundsX2 = input.bounds.x2;
    push.boundsY2 = input.bounds.y2;
    push.inOffset = (uint32_t)(input.byteOffset / sizeof(float));
    push.rowStride = input.rowStrideFloats;
    push.nComps = input.nComps;
    channelOffsets(params.channels, params.alphaChannelIndex, &push.rOffset, &push.gOffset, &push.bOffset);
    push.luminance = (params.channels == eDisplayChannelsY) ? 1 : 0;
    push.matteIndex = (params.matteOverlay && (params.alphaChannelIndex >= 0)) ? params.alphaChannelIndex : -1;
    push.gain = (float)params.gain;
    push.offset = (float)params.offset;
    if (params.gamma <= 0.) {
        push.gammaMode = eGammaModeThreshold;
    } else if (params.gamma == 1.) {
        push.gammaMode = eGammaModeIdentity;
    } else {
        push.gammaMode = eGammaModePow;
        push.invGamma = (float)(1. / params.gamma);
    }

    std::vector<gpu::GpuBinding> bindings;
    bindings.push_back(gpu::GpuBinding::storage(kInputBinding, *input.buffer));
    gpu::GpuBinding out;
    out.binding = kOutputBinding;
    out.kind = gpu::GpuBinding::Kind::StorageBuffer;
    out.buffer = outBuffer;
    out.bufferSize = frame.bytes();
    bindings.push_back(out);
    if (pipeline->ubo) {
        bindings.push_back(gpu::GpuBinding::uniform(pipeline->uboBinding, pipeline->ubo));
    }
    if (pipeline->textures) {
        OcioVulkanShader::Resources::appendBindings(pipeline->shader, *pipeline->textures, bindings);
    }

    return pipeline->kernel->recordBindings(cmd,
                                            bindings,
                                            std::as_bytes(std::span<const ViewerPush>(&push, 1)),
                                            pipeline->kernel->groupCount({ frame.width, frame.height, 1 }));
}

void
ViewerGpuProcess::releaseDescriptors()
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& entry : _pipelines) {
        entry.second->kernel->releaseDescriptors();
    }
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU
