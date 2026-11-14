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

#include "Wavefront.h"

#include <algorithm>
#include <cmath>

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3i.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
const double kNdcNear = -1.;
const double kNdcFar = 1.;
const float kFallbackGrey = 0.18f;

bool
isFinite(const GfVec3d& v)
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

void
storePosition(const GfVec3d& v,
              float* destination)
{
    destination[0] = static_cast<float>(v[0]);
    destination[1] = static_cast<float>(v[1]);
    destination[2] = static_cast<float>(v[2]);
}

// Whether a geometry's colours cover what its binding will index them with.
bool
colorsFitBinding(const UtilityScene::Geometry& geometry)
{
    const std::size_t colorCount = geometry.colors.size();

    switch (geometry.colorBinding) {
    case UtilityScene::eColorBindingConstant:
        return colorCount >= 1;
    case UtilityScene::eColorBindingTriangle:
        return colorCount >= geometry.triangles.size();
    case UtilityScene::eColorBindingCorner:
        return colorCount >= geometry.triangles.size() * 3;
    case UtilityScene::eColorBindingVertex:
        for (const GfVec3i& triangle : geometry.triangles) {
            for (int corner = 0; corner < 3; ++corner) {
                if (triangle[corner] < 0 || static_cast<std::size_t>(triangle[corner]) >= colorCount) {
                    return false;
                }
            }
        }

        return true;
    }

    return false;
}
} // anon namespace

bool
wavefrontCameraIsUsable(const WavefrontFrame& frame)
{
    const GfMatrix4d viewProjection = frame.worldToView * frame.projection;
    double determinant = 0.;
    const GfMatrix4d inverse = viewProjection.GetInverse(&determinant);

    if (determinant == 0. || !std::isfinite(determinant)) {
        return false;
    }
    for (const double ndcZ : { kNdcNear, kNdcFar }) {
        const GfVec3d corners[] = { inverse.Transform(GfVec3d(0., 0., ndcZ)), inverse.Transform(GfVec3d(1., 0., ndcZ)), inverse.Transform(GfVec3d(0., 1., ndcZ)) };

        for (const GfVec3d& corner : corners) {
            if (!isFinite(corner)) {
                return false;
            }
        }
    }

    return true;
}

KernelRayGenParams
makeRayGenParams(const WavefrontFrame& frame,
                 const WavefrontRect& tile)
{
    const GfMatrix4d inverse = (frame.worldToView * frame.projection).GetInverse();
    const GfVec3d nearAtCentre = inverse.Transform(GfVec3d(0., 0., kNdcNear));
    const GfVec3d farAtCentre = inverse.Transform(GfVec3d(0., 0., kNdcFar));
    KernelRayGenParams params;

    storePosition(nearAtCentre, params.nearAtCentre);
    storePosition(inverse.Transform(GfVec3d(1., 0., kNdcNear)) - nearAtCentre, params.nearPerX);
    storePosition(inverse.Transform(GfVec3d(0., 1., kNdcNear)) - nearAtCentre, params.nearPerY);
    storePosition(farAtCentre, params.farAtCentre);
    storePosition(inverse.Transform(GfVec3d(1., 0., kNdcFar)) - farAtCentre, params.farPerX);
    storePosition(inverse.Transform(GfVec3d(0., 1., kNdcFar)) - farAtCentre, params.farPerY);
    const WavefrontSampling& sampling = frame.sampling;
    params.filterWidth = sampling.filterWidth;
    params.sampleIndex = static_cast<std::uint32_t>(std::max(0, sampling.sampleIndex));
    params.seed = sampling.seed;
    params.samplesPerSide = static_cast<std::uint32_t>(std::max(1, sampling.samplesPerSide));
    params.filter = sampling.filter;
    params.padding = 0;
    params.windowX = frame.window.x;
    params.windowY = frame.window.y;
    params.windowWidth = frame.window.width;
    params.windowHeight = frame.window.height;
    params.tileX = tile.x;
    params.tileY = tile.y;
    params.tileWidth = tile.width;
    params.tileHeight = tile.height;

    return params;
}

KernelShadeParams
makeShadeParams(const WavefrontFrame& frame,
                std::uint32_t count)
{
    const GfMatrix4d& projection = frame.projection;
    const GfMatrix4d inverse = projection.GetInverse();
    KernelShadeParams params;

    params.projection[0] = static_cast<float>(projection[2][2]);
    params.projection[1] = static_cast<float>(projection[3][2]);
    params.projection[2] = static_cast<float>(projection[2][3]);
    params.projection[3] = static_cast<float>(projection[3][3]);
    params.viewZAtNear = static_cast<float>(inverse.Transform(GfVec3d(0., 0., kNdcNear))[2]);
    params.viewZAtFar = static_cast<float>(inverse.Transform(GfVec3d(0., 0., kNdcFar))[2]);
    params.count = count;
    params.padding = 0;

    return params;
}

bool
WavefrontShadeTables::update(const UtilityScene::Snapshot& snapshot,
                             const std::atomic<bool>* stop)
{
    if (_synced && _version == snapshot.version) {
        return true;
    }

    std::vector<SourceVersion> sources;

    sources.reserve(snapshot.geometries.size());
    for (const auto& entry : snapshot.geometries) {
        sources.push_back(SourceVersion { entry.first, entry.second.colorVersion, entry.second.topologyVersion, entry.second.points.size() });
    }

    const bool recopy = !_synced || sources != _sources;

    if (recopy) {
        _colors.clear();
        _triangles.clear();
    }
    _geometries.clear();
    _geometries.reserve(snapshot.geometries.size());

    std::uint32_t colorOffset = 0;
    std::uint32_t triangleOffset = 0;

    _synced = false;
    for (const auto& entry : snapshot.geometries) {
        if (stop && stop->load(std::memory_order_relaxed)) {
            return false;
        }

        const UtilityScene::Geometry& geometry = entry.second;
        const bool fits = colorsFitBinding(geometry);
        const std::uint32_t colorCount = fits ? static_cast<std::uint32_t>(geometry.colors.size()) : 1;
        const std::uint32_t triangleCount = static_cast<std::uint32_t>(geometry.triangles.size());
        const KernelGeometryRecord record {
            colorOffset,
            static_cast<std::uint32_t>(fits ? geometry.colorBinding : UtilityScene::eColorBindingConstant),
            geometry.primId,
            triangleOffset,
        };

        _geometries.push_back(record);
        if (recopy) {
            if (fits) {
                _colors.insert(_colors.end(), geometry.colors.cbegin(), geometry.colors.cend());
            } else {
                _colors.push_back(GfVec4f(kFallbackGrey, kFallbackGrey, kFallbackGrey, 1.f));
            }
            for (const GfVec3i& triangle : geometry.triangles) {
                _triangles.insert(_triangles.end(), { triangle[0], triangle[1], triangle[2] });
            }
        }
        colorOffset += colorCount;
        triangleOffset += triangleCount * 3;
    }

    _instanceIds.clear();
    _instanceIds.reserve(snapshot.instances.size());
    for (const UtilityScene::Instance& instance : snapshot.instances) {
        _instanceIds.push_back(instance.instanceIndex);
    }

    _sources.swap(sources);
    _version = snapshot.version;
    _synced = true;

    return true;
}

KernelShadeBuffers
WavefrontShadeTables::buffers() const
{
    KernelShadeBuffers buffers;

    buffers.geometries = _geometries.data();
    buffers.geometryCount = _geometries.size();
    buffers.colors = reinterpret_cast<const float*>(_colors.data());
    buffers.colorCount = _colors.size();
    buffers.triangles = _triangles.data();
    buffers.triangleIndexCount = _triangles.size();
    buffers.instanceIds = _instanceIds.data();
    buffers.instanceCount = _instanceIds.size();

    return buffers;
}

NATRON_NAMESPACE_EXIT
