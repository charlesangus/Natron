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

#include "UtilityKernels.h"

// The Slang prelude's global type names would collide in any file that also sees USD, so these mirror
// its ComputeVaryingInput and StructuredBuffer.
namespace {
struct KernelVarying {
    std::uint32_t startGroup[3];
    std::uint32_t endGroup[3];
};

struct KernelBuffer {
    const void* data;
    std::size_t count;
};

struct RayGenGlobals {
    KernelBuffer rays;
    const NATRON_NAMESPACE::KernelRayGenParams* params;
};

struct ShadeSurfaceGlobals {
    KernelBuffer rays;
    KernelBuffer hitSurface;
    KernelBuffer hitIds;
    KernelBuffer geometries;
    KernelBuffer colors;
    KernelBuffer triangles;
    KernelBuffer instanceIds;
    KernelBuffer colorOut;
    KernelBuffer depthOut;
    KernelBuffer primIdOut;
    KernelBuffer instanceIdOut;
    KernelBuffer materials;
    KernelBuffer textureBindings;
    KernelBuffer images;
    KernelBuffer texels;
    KernelBuffer projections;
    KernelBuffer points;
    KernelBuffer instanceTransforms;
    KernelBuffer sts;
    KernelBuffer lights;
    KernelBuffer lightData;
    KernelBuffer linkMasks;
    KernelBuffer shadowRays;
    KernelBuffer shadowContributions;
    KernelBuffer shadowLights;
    const NATRON_NAMESPACE::KernelShadeParams* params;
};

struct ShadeLightsGlobals {
    KernelBuffer colorOut;
    KernelBuffer shadowContributions;
    KernelBuffer blocked;
    const NATRON_NAMESPACE::KernelShadeLightsParams* params;
};

// numthreads in every kernel.
const std::uint32_t kGroupSize = 64;

KernelVarying
varyingFor(std::size_t count)
{
    return KernelVarying { { 0, 0, 0 }, { static_cast<std::uint32_t>((count + kGroupSize - 1) / kGroupSize), 1, 1 } };
}
} // anon namespace

extern "C" void utilityRayGen(KernelVarying*, void* entryPointParams, void* globalParams);
extern "C" void utilityShadeSurface(KernelVarying*, void* entryPointParams, void* globalParams);
extern "C" void utilityShadeLights(KernelVarying*, void* entryPointParams, void* globalParams);

NATRON_NAMESPACE_ENTER

void
runRayGenKernel(const KernelRayGenParams& params,
                float* rays)
{
    const std::size_t count = static_cast<std::size_t>(params.tileWidth) * params.tileHeight;

    if (count == 0) {
        return;
    }
    KernelVarying varying = varyingFor(count);
    RayGenGlobals globals { { rays, count * 8 }, &params };

    utilityRayGen(&varying, nullptr, &globals);
}

void
runShadeSurfaceKernel(const KernelShadeParams& params,
                      const KernelShadeBuffers& buffers)
{
    const std::size_t count = params.count;

    if (count == 0) {
        return;
    }
    KernelVarying varying = varyingFor(count);
    const std::size_t shadowCount = count * params.lighting[1];
    ShadeSurfaceGlobals globals {
        { buffers.rays, count * 8 },
        { buffers.hitSurface, count * 3 },
        { buffers.hitIds, count * 3 },
        { buffers.geometries, buffers.geometryCount },
        { buffers.colors, buffers.colorCount },
        { buffers.triangles, buffers.triangleIndexCount },
        { buffers.instanceIds, buffers.instanceCount },
        { buffers.colorOut, count },
        { buffers.depthOut, count },
        { buffers.primIdOut, count },
        { buffers.instanceIdOut, count },
        { buffers.materials, buffers.materialCount },
        { buffers.textureBindings, buffers.textureBindingCount },
        { buffers.images, buffers.imageCount },
        { buffers.texels, buffers.texelCount },
        { buffers.projections, buffers.projectionCount },
        { buffers.points, buffers.pointCount },
        { buffers.instanceTransforms, buffers.instanceTransformRowCount },
        { buffers.sts, buffers.stCount },
        { buffers.lights, buffers.lightCount },
        { buffers.lightData, buffers.lightDataCount },
        { buffers.linkMasks, buffers.linkMaskCount },
        { buffers.shadowRays, shadowCount * 8 },
        { buffers.shadowContributions, shadowCount },
        { buffers.shadowLights, shadowCount },
        &params,
    };

    utilityShadeSurface(&varying, nullptr, &globals);
}

void
runShadeLightsKernel(const KernelShadeLightsParams& params,
                     float* colorOut,
                     const float* shadowContributions,
                     const std::uint32_t* blocked)
{
    const std::size_t count = params.count;

    if (count == 0 || params.lightBatch == 0) {
        return;
    }

    KernelVarying varying = varyingFor(count);
    const std::size_t shadowCount = count * params.lightBatch;
    ShadeLightsGlobals globals {
        { colorOut, count },
        { shadowContributions, shadowCount },
        { blocked, shadowCount },
        &params,
    };

    utilityShadeLights(&varying, nullptr, &globals);
}

NATRON_NAMESPACE_EXIT
