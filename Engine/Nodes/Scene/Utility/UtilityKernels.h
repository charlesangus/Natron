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

#ifndef Engine_Nodes_Scene_Utility_UtilityKernels_h
#define Engine_Nodes_Scene_Utility_UtilityKernels_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <cstddef>
#include <cstdint>

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

// Mirrors RayGenParams in Kernels/RayGen.slang.
struct KernelRayGenParams {
    float nearAtCentre[4];
    float nearPerX[4];
    float nearPerY[4];
    float farAtCentre[4];
    float farPerX[4];
    float farPerY[4];
    std::int32_t windowX;
    std::int32_t windowY;
    std::int32_t windowWidth;
    std::int32_t windowHeight;
    std::int32_t tileX;
    std::int32_t tileY;
    std::int32_t tileWidth;
    std::int32_t tileHeight;
};

// Mirrors ShadeParams in Kernels/ShadeSurface.slang.
struct KernelShadeParams {
    float projection[4];
    float viewZAtNear;
    float viewZAtFar;
    std::uint32_t count;
    std::uint32_t padding;
    float pixelFootprint[4];
};

// Mirrors GeometryRecord in Kernels/UtilityTypes.slang.
struct KernelGeometryRecord {
    std::uint32_t colorOffset;
    std::uint32_t colorBinding;
    std::int32_t primId;
    std::uint32_t triangleOffset;
    std::uint32_t materialIndex;
    std::uint32_t stOffset;
    std::uint32_t stBinding;
    std::uint32_t pointOffset;
};

const std::uint32_t kKernelStBindingNone = 0xffffffffu;
// The flags of MaterialRecord::kind.z in Kernels/UtilityTypes.slang.
const std::uint32_t kKernelMaterialDiffuseFromDisplayColor = 1u;
const std::uint32_t kKernelMaterialOpacityFromDisplayOpacity = 2u;
const std::uint32_t kKernelMaterialEmissiveFromDisplayColor = 4u;

// Mirrors MaterialRecord in Kernels/UtilityTypes.slang.
struct KernelMaterialRecord {
    float diffuseColor[4];
    float emissiveColor[4];
    float specularColor[4];
    float surface[4];
    float coverage[4];
    std::uint32_t kind[4];
    std::int32_t textures[12];
};

// Mirrors TextureBinding in Kernels/UtilityTypes.slang.
struct KernelTextureBinding {
    float scale[4];
    float bias[4];
    float fallback[4];
    float uvTransform[4];
    float uvRotation[4];
    std::int32_t image[4];
    std::uint32_t channels[4];
};

// Mirrors ProjectionRecord in Kernels/UtilityTypes.slang.
struct KernelProjectionRecord {
    float worldToProjector[16];
    float geometryFrame[16];
    float geometryFrameInverse[16];
    float plateTransform[4];
    float projectorOrigin[4];
    std::int32_t settings[4];
    std::int32_t plate[4];
};

// The buffers ShadeSurface reads and writes, with the layouts its bindings declare.
struct KernelShadeBuffers {
    const float* rays = nullptr;
    const float* hitSurface = nullptr;
    const std::uint32_t* hitIds = nullptr;
    const KernelGeometryRecord* geometries = nullptr;
    std::size_t geometryCount = 0;
    const float* colors = nullptr; // four per colour
    std::size_t colorCount = 0;
    const std::int32_t* triangles = nullptr; // three per triangle
    std::size_t triangleIndexCount = 0;
    const std::int32_t* instanceIds = nullptr;
    std::size_t instanceCount = 0;
    float* colorOut = nullptr; // four per ray
    float* depthOut = nullptr;
    std::int32_t* primIdOut = nullptr;
    std::int32_t* instanceIdOut = nullptr;
    const KernelMaterialRecord* materials = nullptr;
    std::size_t materialCount = 0;
    const KernelTextureBinding* textureBindings = nullptr;
    std::size_t textureBindingCount = 0;
    const std::uint32_t* images = nullptr; // four per image: width, height, levels, first texel
    std::size_t imageCount = 0;
    const float* texels = nullptr; // four per texel
    std::size_t texelCount = 0;
    const KernelProjectionRecord* projections = nullptr;
    std::size_t projectionCount = 0;
    const float* points = nullptr; // four per point
    std::size_t pointCount = 0;
    const float* instanceTransforms = nullptr; // eight rows of four per instance
    std::size_t instanceTransformRowCount = 0;
    const float* sts = nullptr; // two per value
    std::size_t stCount = 0;
};

/**
 * @brief Runs the C++ build of RayGen.slang for one tile, writing tileWidth * tileHeight rays in the
 * eight-array layout of Kernels/UtilityTypes.slang. MT-safe.
 **/
void runRayGenKernel(const KernelRayGenParams& params,
                     float* rays);

/**
 * @brief Runs the C++ build of ShadeSurface.slang over params.count hits. MT-safe.
 **/
void runShadeSurfaceKernel(const KernelShadeParams& params,
                           const KernelShadeBuffers& buffers);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityKernels_h
