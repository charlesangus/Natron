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

#include "OcioVulkanShader.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>

NATRON_NAMESPACE_ENTER

namespace {

namespace OCIO = OCIO_NAMESPACE;

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

gpu::GpuTextureFilter
filterFor(OCIO::Interpolation interpolation)
{
    return (interpolation == OCIO::INTERP_NEAREST) ? gpu::GpuTextureFilter::Nearest : gpu::GpuTextureFilter::Linear;
}

} // namespace

bool
OcioVulkanShader::build(const OCIO::ConstGPUProcessorRcPtr& processor,
                        const std::string& functionName,
                        unsigned firstBinding,
                        OcioVulkanShader* out,
                        std::string* error)
{
    try {
        OCIO::GpuShaderDescRcPtr desc = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_VK_4_6);
        desc->setFunctionName(functionName.c_str());
        desc->setResourcePrefix("ocio_");
        desc->setDescriptorSetIndex(0, firstBinding);
        processor->extractGpuShaderInfo(desc);

        OcioVulkanShader shader;
        shader._functionName = functionName;
        shader._cacheID = desc->getCacheID();
        shader._glsl = withoutVersionLines(desc->getShaderText());
        shader._uniformBytes = desc->getUniformBufferSize();

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
            Texture t;
            t.binding = desc->getTextureShaderBindingIndex(i);
            t.sampler = sampler;
            t.desc.dimensions = (dimensions == OCIO::GpuShaderDesc::TEXTURE_1D) ? 1 : 2;
            t.desc.width = width;
            t.desc.height = height;
            t.desc.channels = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL) ? 1 : 3;
            t.desc.filter = filterFor(interpolation);
            t.texels.assign(values, values + std::size_t(width) * height * t.desc.channels);
            shader._textures.push_back(std::move(t));
        }
        for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
            const char* name = nullptr;
            const char* sampler = nullptr;
            unsigned edge = 0;
            OCIO::Interpolation interpolation;
            desc->get3DTexture(i, name, sampler, edge, interpolation);
            const float* values = nullptr;
            desc->get3DTextureValues(i, values);
            Texture t;
            t.binding = desc->get3DTextureShaderBindingIndex(i);
            t.sampler = sampler;
            t.desc.dimensions = 3;
            t.desc.width = t.desc.height = t.desc.depth = edge;
            t.desc.channels = 3;
            t.desc.filter = filterFor(interpolation);
            t.texels.assign(values, values + std::size_t(edge) * edge * edge * 3);
            shader._textures.push_back(std::move(t));
        }
        shader._desc = desc;
        *out = std::move(shader);

        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }

        return false;
    }
}

bool
OcioVulkanShader::fillUniforms(std::vector<std::byte>& block) const
{
    block.assign(std::max<std::size_t>({ block.size(), _uniformBytes, 16 }), std::byte(0));
    if (!_desc) {
        return true;
    }

    std::vector<std::size_t> offsets;
    for (unsigned i = 0; i < _desc->getNumUniforms(); ++i) {
        OCIO::GpuShaderDesc::UniformData data;
        _desc->getUniform(i, data);
        offsets.push_back(data.m_bufferOffset);
    }
    for (unsigned i = 0; i < _desc->getNumUniforms(); ++i) {
        OCIO::GpuShaderDesc::UniformData data;
        _desc->getUniform(i, data);
        std::byte* at = block.data() + data.m_bufferOffset;
        switch (data.m_type) {
        case OCIO::UNIFORM_DOUBLE: {
            const float v = float(data.m_getDouble());
            std::memcpy(at, &v, sizeof(v));
            break;
        }
        case OCIO::UNIFORM_BOOL: {
            const std::uint32_t v = data.m_getBool() ? 1u : 0u;
            std::memcpy(at, &v, sizeof(v));
            break;
        }
        case OCIO::UNIFORM_FLOAT3: {
            const OCIO::Float3 v = data.m_getFloat3();
            std::memcpy(at, v.data(), sizeof(float) * 3);
            break;
        }
        case OCIO::UNIFORM_VECTOR_FLOAT:
        case OCIO::UNIFORM_VECTOR_INT: {
            const bool isFloat = data.m_type == OCIO::UNIFORM_VECTOR_FLOAT;
            const std::size_t count = std::size_t(isFloat ? data.m_vectorFloat.m_getSize() : data.m_vectorInt.m_getSize());
            // std140 pads array elements to 16 bytes, but OCIO reports only the offset, so the
            // stride is inferred from the room left before the next uniform.
            std::size_t limit = _uniformBytes;
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
            break;
        }
        default:
            return false;
        }
    }

    return true;
}

OCIO::DynamicPropertyRcPtr
OcioVulkanShader::dynamicProperty(OCIO::DynamicPropertyType type) const
{
    if (!_desc || !_desc->hasDynamicProperty(type)) {
        return OCIO::DynamicPropertyRcPtr();
    }

    return _desc->getDynamicProperty(type);
}

OcioVulkanShader::Resources::Resources(gpu::GpuRuntime& runtime)
    : _runtime(runtime)
{
}

gpu::GpuStatus
OcioVulkanShader::Resources::acquire(const OcioVulkanShader& shader,
                                     std::shared_ptr<const TextureList>& out)
{
    const auto it = _entries.find(shader.cacheID());
    if (it != _entries.end()) {
        out = it->second;

        return gpu::GpuStatus();
    }

    auto list = std::make_shared<TextureList>();
    for (const OcioVulkanShader::Texture& t : shader.textures()) {
        std::unique_ptr<gpu::GpuTexture> texture;
        const gpu::GpuStatus s = gpu::GpuTexture::create(_runtime.device(), t.desc, t.texels, texture);
        if (!s) {
            return s;
        }
        list->push_back(std::move(texture));
        ++_texturesCreated;
    }
    out = list;
    _entries.emplace(shader.cacheID(), std::move(list));

    return gpu::GpuStatus();
}

void
OcioVulkanShader::Resources::appendBindings(const OcioVulkanShader& shader,
                                            const TextureList& textures,
                                            std::vector<gpu::GpuBinding>& bindings)
{
    const std::size_t count = std::min(textures.size(), shader.textures().size());
    for (std::size_t i = 0; i < count; ++i) {
        bindings.push_back(gpu::GpuBinding::texture(shader.textures()[i].binding, *textures[i]));
    }
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU
