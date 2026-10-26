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

#ifndef NATRON_ENGINE_OCIOVULKANSHADER_H
#define NATRON_ENGINE_OCIOVULKANSHADER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <OpenColorIO/OpenColorIO.h>

#include "Engine/Gpu/GpuDevice.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuTexture.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An OCIO GPU processor turned into GLSL for the Vulkan compute backend. The text declares
 * `vec4 <functionName>(vec4)` plus the uniform block and the LUT samplers it needs, all in
 * descriptor set 0, and carries no #version line so that the caller's wrapper owns the
 * version, local size, kernel buffers and push constants.
 *
 * Binding scheme: the uniform block is at kUniformBinding, the LUT samplers count up from the
 * firstBinding given to build(), and a wrapper must keep its own bindings clear of both.
 *
 * Dynamic properties (exposure, contrast, ...) are uniforms, not part of the text or cacheID(),
 * so changing one needs fillUniforms() again and no new pipeline.
 **/
class OcioVulkanShader {
public:
    static constexpr unsigned kUniformBinding = 0;

    struct Texture {
        std::string sampler;
        unsigned binding = 0;
        gpu::GpuTextureDesc desc;
        // Shared, so that copies of the shader, and the inputs a node bakes from it for each
        // render, do not copy the table.
        std::shared_ptr<const std::vector<float>> texels;
        // Identifies desc and texels, for the runtime's GpuTextureCache: equal tables share one
        // device texture whatever shader or processor they came from.
        std::string cacheKey;
    };

    /**
     * @brief Extracts the shader from \p processor. Returns false with \p error set when OCIO
     * cannot produce one or when a uniform has a type the layout code does not know.
     **/
    static bool build(const OCIO_NAMESPACE::ConstGPUProcessorRcPtr& processor,
                      const std::string& functionName,
                      unsigned firstBinding,
                      OcioVulkanShader* out,
                      std::string* error);

    const std::string& glsl() const { return _glsl; }
    const std::string& cacheID() const { return _cacheID; }
    const std::string& functionName() const { return _functionName; }
    const std::vector<Texture>& textures() const { return _textures; }

    // The type name of OCIO's uniform block, which has no instance name, so the name a compiled
    // kernel reflects for it; empty when the shader has no uniforms.
    const std::string& uniformBlockName() const { return _uniformBlockName; }

    // Bytes OCIO lays out in the uniform block; 0 when the shader has no uniforms.
    std::size_t uniformBytes() const { return _uniformBytes; }

    /**
     * @brief Writes the current value of every OCIO uniform at its std140 offset. \p block is grown
     * to at least uniformBytes() (and 16, as a uniform buffer is never empty) and zeroed first, so
     * a caller may pre-size it to the block the compiled kernel reflects. Returns false for a
     * uniform of an unknown type.
     **/
    bool fillUniforms(std::vector<std::byte>& block) const;

    /**
     * @brief The dynamic property of \p type that fillUniforms() reads, or null when the
     * processor has none. Each build() owns independent properties, so a caller changes values on
     * the shader object it keeps, not on the processor it was built from.
     **/
    OCIO_NAMESPACE::DynamicPropertyRcPtr dynamicProperty(OCIO_NAMESPACE::DynamicPropertyType type) const;

    /**
     * @brief The GpuTextures of OCIO shaders on one runtime's device, taken from the runtime's
     * GpuTextureCache under each Texture::cacheKey, so that a table is uploaded once and shared by
     * the viewer and the OCIO nodes alike. The textures must be released before the runtime is
     * recreated or destroyed, and any recorded work that samples them must be finished first.
     **/
    class Resources {
    public:
        explicit Resources(gpu::GpuRuntime& runtime);

        typedef std::vector<std::shared_ptr<const gpu::GpuTexture>> TextureList;

        /**
         * @brief The textures of \p shader, in textures() order, uploading those the runtime's
         * cache does not hold. Thread safe.
         **/
        gpu::GpuStatus acquire(const OcioVulkanShader& shader, std::shared_ptr<const TextureList>& out);

        // Appends the combined-image-sampler bindings of \p shader backed by \p textures.
        static void appendBindings(const OcioVulkanShader& shader,
                                   const TextureList& textures,
                                   std::vector<gpu::GpuBinding>& bindings);

        // Textures this object's acquire() calls uploaded because the runtime's cache did not hold
        // them; tests use it to tell a cache hit from an upload.
        std::size_t texturesCreated() const;

    private:
        gpu::GpuRuntime& _runtime;
        std::atomic<std::size_t> _created;
    };

private:
    OCIO_NAMESPACE::GpuShaderDescRcPtr _desc;
    std::string _glsl;
    std::string _cacheID;
    std::string _functionName;
    std::vector<Texture> _textures;
    std::string _uniformBlockName;
    std::size_t _uniformBytes = 0;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // NATRON_ENGINE_OCIOVULKANSHADER_H
