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

#ifndef Engine_Nodes_Image_GpuRender_h
#define Engine_Nodes_Image_GpuRender_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "Engine/ImageLayerDesc.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"
#include "Global/Enums.h"

// Declared rather than included so engine code can name the GPU types in a build without NatronGpu.
namespace gpu {
class GpuBuffer;
class GpuRecordContext;
}

NATRON_NAMESPACE_ENTER

/**
 * @brief What a node is asked to render, enough for it to say whether the GPU path can take it.
 **/
struct GpuQuery {
    double time;
    ViewIdx view;
    RenderScale mappedScale;
    // The plane the task renders; nComps is its component count.
    ImageLayerDesc plane;
    int nComps;
    ImageBitDepthEnum bitDepth;
    bool hasMask;

    // Whether the channel the mask reads is a channel of the rendered plane, or the mask resolves
    // to no channel and reads zero. The mask itself is handed over in the layer of its channel.
    bool maskInPlane;

    double mix;
    bool processChannels[4];

    // Channel of the input that "(Un)premult by" divides by, or -1 when none is selected.
    int unPremultChannel;

    // Whether that channel is a channel of the rendered plane, so that it can be read from the
    // source pixel being divided.
    bool unPremultInPlane;

    // Whether an input is fetched through transforms concatenated from upstream nodes, which
    // render() folds into its own sampling and renderGpu() cannot.
    bool hasConcatenatedInput;

    // Whether the frame renders in draft quality.
    bool draftMode;

    // The largest scratch buffer the device binds, as GpuRecordContext::maxScratchBytes() reports it;
    // unbounded when placement has no device.
    uint64_t maxScratchBytes;

    GpuQuery()
        : time(0.)
        , view(0)
        , mappedScale()
        , plane()
        , nComps(4)
        , bitDepth(eImageBitDepthFloat)
        , hasMask(false)
        , maskInPlane(false)
        , mix(1.)
        , unPremultChannel(-1)
        , unPremultInPlane(false)
        , hasConcatenatedInput(false)
        , draftMode(false)
        , maxScratchBytes(UINT64_MAX)
    {
        for (int i = 0; i < 4; ++i) {
            processChannels[i] = true;
        }
    }
};

/**
 * @brief A window of interleaved float pixels in a device buffer. The view does not own the
 * buffer.
 **/
struct GpuImageView {
    gpu::GpuBuffer* buffer;
    uint64_t byteOffset;
    RectI bounds;
    int rowStrideFloats;
    int nComps;

    GpuImageView()
        : buffer(nullptr)
        , byteOffset(0)
        , bounds()
        , rowStrideFloats(0)
        , nComps(0)
    {
    }
};

/**
 * @brief A GLSL compute shader that renders a point operator in place of a registered kernel,
 * compiled when it is first asked for.
 *
 * The base runs it between the same divide and finish passes as a spatial node, so the mask, mix,
 * unprocessed channels and "(Un)premult by" are the base's. Its storage buffers are found by
 * name, at whatever bindings of set 0 the shader declares them: the block named
 * kGpuShaderSourceBuffer holds the divided source and the one named kGpuShaderDestinationBuffer
 * is the one the shader writes, both nComps floats per pixel of the output rect, packed row by row
 * from its bottom row. A block's name is its variable name, or its type name when it has none, as
 * for the uniforms. It declares no other storage buffer. Its push constants, if it declares any,
 * are GpuShaderRegion. Every other binding of set 0 is a uniform block or a texture named in
 * GpuShaderInputs, so that generated code which puts its own blocks first, as OpenColorIO's does,
 * needs no renumbering.
 **/
struct GpuRuntimeShader {
    std::string source;
    std::string entry;

    GpuRuntimeShader()
        : source()
        , entry("main")
    {
    }
};

// The names of a runtime shader's source and destination storage buffers.
inline constexpr const char* kGpuShaderSourceBuffer = "Src";
inline constexpr const char* kGpuShaderDestinationBuffer = "Dst";

// Mirrors the push-constant block of a runtime shader.
struct GpuShaderRegion {
    int32_t x1;
    int32_t y1;
    uint32_t width;
    uint32_t height;
    uint32_t nComps;
    // In the bit space of pixelKernelChannelBit: a one-channel pixel is alpha, bit 3.
    uint32_t channelMask;
};

struct GpuShaderUniform {
    // The block's variable name, or its type name when the block has none.
    std::string binding;
    std::vector<std::byte> data;
};

/**
 * @brief A float texture with a clamp-to-edge sampler, texels x fastest, then y, then z.
 *
 * With a cacheKey the texture is kept by the GPU context under that key, shared with every node
 * and the viewer asking for it, and uploaded only when the context does not hold it already. The
 * key must then identify the dimensions, extents, channels, filter and texels, since a texture the
 * context holds under it is used whatever this one carries. Without one it is uploaded for each
 * render.
 **/
struct GpuShaderTexture {
    std::string binding;
    std::string cacheKey;
    // 1, 2 or 3; the unused extents stay 1.
    int dimensions;
    int width;
    int height;
    int depth;
    // 1 to 4 floats per texel.
    int channels;
    bool linear;
    // Shared, so that inputs baked for every render do not copy a large table each time.
    std::shared_ptr<const std::vector<float>> texels;

    GpuShaderTexture()
        : binding()
        , cacheKey()
        , dimensions(2)
        , width(1)
        , height(1)
        , depth(1)
        , channels(4)
        , linear(true)
        , texels()
    {
    }
};

struct GpuShaderInputs {
    std::vector<GpuShaderUniform> uniforms;
    std::vector<GpuShaderTexture> textures;

    template <typename P>
    void addUniform(const std::string& binding,
                    const P& block)
    {
        static_assert(std::is_trivially_copyable<P>::value, "uniform blocks are copied byte for byte");
        GpuShaderUniform u;
        u.binding = binding;
        u.data.resize(sizeof(P));
        std::memcpy(u.data.data(), &block, sizeof(P));
        uniforms.push_back(u);
    }
};

// An input read in a layer beyond the one getGpuInputLayer() names for it.
struct GpuLayerInput {
    int inputNb;
    ImageLayerDesc layer;
    GpuImageView view;

    GpuLayerInput()
        : inputNb(-1)
        , layer()
        , view()
    {
    }
};

struct GpuRenderArgs {
    GpuQuery query;
    RectI outputRect;

    // Indexed by input number; an input that is not connected has a null buffer.
    std::vector<GpuImageView> inputs;

    // The layers of getGpuExtraInputLayers(), one entry per input and layer.
    std::vector<GpuLayerInput> layerInputs;
    GpuImageView output;
    gpu::GpuRecordContext& context;

    explicit GpuRenderArgs(gpu::GpuRecordContext& ctx)
        : query()
        , outputRect()
        , inputs()
        , layerInputs()
        , output()
        , context(ctx)
    {
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_GpuRender_h
