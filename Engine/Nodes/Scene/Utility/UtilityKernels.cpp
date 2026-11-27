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

#include <array>
#include <vector>

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
    KernelBuffer weights;
    KernelBuffer cameraSteps;
    const NATRON_NAMESPACE::KernelRayGenParams* params;
};

struct RayGenUvGlobals {
    KernelBuffer rays;
    KernelBuffer weights;
    KernelBuffer hitSurface;
    KernelBuffer hitIds;
    KernelBuffer unwrapAddresses;
    const NATRON_NAMESPACE::KernelRayGenParams* params;
};

struct ShadeSurfaceGlobals {
    KernelBuffer rays;
    KernelBuffer hitSurface;
    KernelBuffer hitIds;
    KernelBuffer tableAddresses;
    KernelBuffer colorOut;
    KernelBuffer depthOut;
    KernelBuffer primIdOut;
    KernelBuffer instanceIdOut;
    KernelBuffer materials;
    KernelBuffer textureBindings;
    KernelBuffer images;
    KernelBuffer projections;
    KernelBuffer lights;
    KernelBuffer shadowRays;
    KernelBuffer shadowContributions;
    KernelBuffer shadowLights;
    KernelBuffer paths;
    KernelBuffer nextRays;
    KernelBuffer nextPaths;
    KernelBuffer liveBounces;
    const NATRON_NAMESPACE::KernelShadeParams* params;
};

struct ResolveHitsGlobals {
    KernelBuffer rays;
    KernelBuffer hitSurface;
    KernelBuffer hitIds;
    KernelBuffer nextHitSurface;
    KernelBuffer nextHitIds;
    KernelBuffer retrace;
    KernelBuffer blocked;
    KernelBuffer rayLights;
    KernelBuffer tableAddresses;
    KernelBuffer materials;
    KernelBuffer textureBindings;
    KernelBuffer images;
    KernelBuffer projections;
    KernelBuffer shadowContributions;
    KernelBuffer pending;
    const NATRON_NAMESPACE::KernelResolveParams* params;
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
extern "C" void utilityRayGenUv(KernelVarying*, void* entryPointParams, void* globalParams);
extern "C" void utilityShadeSurface(KernelVarying*, void* entryPointParams, void* globalParams);
extern "C" void utilityResolveHits(KernelVarying*, void* entryPointParams, void* globalParams);
extern "C" void utilityShadeLights(KernelVarying*, void* entryPointParams, void* globalParams);

NATRON_NAMESPACE_ENTER

namespace {
KernelTableAddress
hostAddress(const void* table)
{
    return kernelTableAddress(reinterpret_cast<std::uintptr_t>(table));
}

std::array<KernelTableAddress, eKernelTableCount>
tableAddresses(const KernelShadeBuffers& buffers)
{
    std::array<KernelTableAddress, eKernelTableCount> addresses {};

    addresses[eKernelTableGeometries] = hostAddress(buffers.geometries);
    addresses[eKernelTableColors] = hostAddress(buffers.colors);
    addresses[eKernelTableTriangles] = hostAddress(buffers.triangles);
    addresses[eKernelTableInstanceIds] = hostAddress(buffers.instanceIds);
    addresses[eKernelTablePoints] = hostAddress(buffers.points);
    addresses[eKernelTableInstanceTransforms] = hostAddress(buffers.instanceTransforms);
    addresses[eKernelTableSts] = hostAddress(buffers.sts);
    addresses[eKernelTableNormals] = hostAddress(buffers.normals);
    addresses[eKernelTableLightData] = hostAddress(buffers.lightData);
    addresses[eKernelTableLinkMasks] = hostAddress(buffers.linkMasks);
    addresses[eKernelTableTexels] = hostAddress(buffers.texels);
    addresses[eKernelTableInstanceMotion] = hostAddress(buffers.instanceMotion);
    addresses[eKernelTableMotionRows] = hostAddress(buffers.motionRows);

    return addresses;
}
} // anon namespace

void
runRayGenKernel(const KernelRayGenParams& params,
                float* rays,
                float* weights,
                const float* cameraSteps,
                std::size_t cameraStepRows)
{
    const std::size_t count = static_cast<std::size_t>(params.tileWidth) * params.tileHeight;

    if (count == 0) {
        return;
    }

    std::vector<float> unwanted;

    if (!weights) {
        unwanted.resize(count);
        weights = unwanted.data();
    }
    KernelVarying varying = varyingFor(count);
    // The kernel reads the step count from the first row; zero is a camera that does not move.
    static const float kStill[4] = { 0.f, 0.f, 0.f, 0.f };

    if (!cameraSteps || cameraStepRows == 0) {
        cameraSteps = kStill;
        cameraStepRows = 1;
    }
    RayGenGlobals globals { { rays, count * kKernelRayArrays }, { weights, count }, { cameraSteps, cameraStepRows }, &params };

    utilityRayGen(&varying, nullptr, &globals);
}

void
runRayGenUvKernel(const KernelRayGenParams& params,
                  const std::int32_t* grid,
                  const float* triangles,
                  float* rays,
                  float* hitSurface,
                  std::uint32_t* hitIds,
                  float* weights)
{
    const std::size_t count = static_cast<std::size_t>(params.tileWidth) * params.tileHeight;

    if (count == 0) {
        return;
    }

    std::vector<float> unwanted;

    if (!weights) {
        unwanted.resize(count);
        weights = unwanted.data();
    }
    KernelVarying varying = varyingFor(count);
    const KernelTableAddress unwrap[2] = { hostAddress(grid), hostAddress(triangles) };
    RayGenUvGlobals globals { { rays, count * kKernelRayArrays }, { weights, count }, { hitSurface, count * 3 }, { hitIds, count * 3 }, { unwrap, 2 }, &params };

    utilityRayGenUv(&varying, nullptr, &globals);
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
    const bool continues = params.lighting[0] == 0 && params.bounce[0] < params.bounce[1];
    const std::size_t pathCount = params.bounce[0] > 0 ? count : 0;
    // The tile's own scan of the continuations tells which paths go on, so the flags are not read.
    std::vector<std::uint32_t> liveBounces(continues ? params.bounce[1] : 0);
    const std::array<KernelTableAddress, eKernelTableCount> addresses = tableAddresses(buffers);
    ShadeSurfaceGlobals globals {
        { buffers.rays, count * kKernelRayArrays },
        { buffers.hitSurface, count * 3 },
        { buffers.hitIds, count * 3 },
        { addresses.data(), addresses.size() },
        { buffers.colorOut, count },
        { buffers.depthOut, count },
        { buffers.primIdOut, count },
        { buffers.instanceIdOut, count },
        { buffers.materials, buffers.materialCount },
        { buffers.textureBindings, buffers.textureBindingCount },
        { buffers.images, buffers.imageCount },
        { buffers.projections, buffers.projectionCount },
        { buffers.lights, buffers.lightCount },
        { buffers.shadowRays, shadowCount * kKernelRayArrays },
        { buffers.shadowContributions, shadowCount },
        { buffers.shadowLights, shadowCount },
        { buffers.paths, pathCount },
        { buffers.nextRays, continues ? count * kKernelRayArrays : 0 },
        { buffers.nextPaths, continues ? count : 0 },
        { liveBounces.data(), liveBounces.size() },
        &params,
    };

    utilityShadeSurface(&varying, nullptr, &globals);
}

void
runResolveHitsKernel(const KernelResolveParams& params,
                     const KernelShadeBuffers& tables,
                     const KernelResolveBuffers& buffers)
{
    const std::size_t count = params.count;

    if (count == 0) {
        return;
    }
    KernelVarying varying = varyingFor(count);
    const bool shadow = (params.flags & kKernelResolveShadow) != 0;
    // The caller's own scan of the retraced rays tells which are pending, so the flags are not read.
    std::vector<std::uint32_t> pending(params.passIndex + 1);
    const std::array<KernelTableAddress, eKernelTableCount> addresses = tableAddresses(tables);
    ResolveHitsGlobals globals {
        { buffers.rays, count * kKernelRayArrays },
        { buffers.hitSurface, count * 3 },
        { buffers.hitIds, count * 3 },
        { buffers.nextHitSurface, count * 3 },
        { buffers.nextHitIds, count * 3 },
        { buffers.retrace, count * kKernelRayArrays },
        { buffers.blocked, shadow ? count : 0 },
        { buffers.rayLights, shadow ? count : 0 },
        { addresses.data(), addresses.size() },
        { tables.materials, tables.materialCount },
        { tables.textureBindings, tables.textureBindingCount },
        { tables.images, tables.imageCount },
        { tables.projections, tables.projectionCount },
        { buffers.shadowContributions, shadow ? count : 0 },
        { pending.data(), pending.size() },
        &params,
    };

    utilityResolveHits(&varying, nullptr, &globals);
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
