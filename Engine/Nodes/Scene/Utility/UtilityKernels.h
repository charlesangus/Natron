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

// The reconstruction filters, as RayGenParams::filter in Kernels/RayGen.slang numbers them.
enum KernelPixelFilter : std::uint32_t {
    eKernelPixelFilterBox = 0,
    eKernelPixelFilterTriangle = 1,
    eKernelPixelFilterGaussian = 2,
    eKernelPixelFilterBlackmanHarris = 3,
};

// Mirrors RayGenParams in Kernels/RayGen.slang, where each position is a float3 with the scalar after
// it in the same 16 bytes.
struct KernelRayGenParams {
    float nearAtCentre[3];
    float filterWidth;
    float nearPerX[3];
    std::uint32_t sampleIndex;
    float nearPerY[3];
    std::uint32_t seed;
    float farAtCentre[3];
    std::uint32_t samplesPerSide;
    float farPerX[3];
    std::uint32_t filter;
    float farPerY[3];
    std::uint32_t padding;
    std::int32_t windowX;
    std::int32_t windowY;
    std::int32_t windowWidth;
    std::int32_t windowHeight;
    std::int32_t tileX;
    std::int32_t tileY;
    std::int32_t tileWidth;
    std::int32_t tileHeight;
};
static_assert(sizeof(KernelRayGenParams) == 128, "KernelRayGenParams must fit the 128 bytes of push constants every device has");

// Mirrors ShadeParams in Kernels/ShadeFlat.slang.
struct KernelShadeParams {
    float projection[4];
    float viewZAtNear;
    float viewZAtFar;
    std::uint32_t count;
    std::uint32_t padding;
};

// Mirrors GeometryRecord in Kernels/UtilityTypes.slang.
struct KernelGeometryRecord {
    std::uint32_t colorOffset;
    std::uint32_t colorBinding;
    std::int32_t primId;
    std::uint32_t triangleOffset;
};

// The buffers ShadeFlat reads and writes, with the layouts its bindings declare.
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
};

/**
 * @brief Runs the C++ build of RayGen.slang for one tile, writing tileWidth * tileHeight rays in the
 * eight-array layout of Kernels/UtilityTypes.slang, and when weights is not null each ray's filter
 * weight. MT-safe.
 **/
void runRayGenKernel(const KernelRayGenParams& params,
                     float* rays,
                     float* weights = nullptr);

/**
 * @brief Runs the C++ build of ShadeFlat.slang over params.count hits. MT-safe.
 **/
void runShadeFlatKernel(const KernelShadeParams& params,
                        const KernelShadeBuffers& buffers);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityKernels_h
