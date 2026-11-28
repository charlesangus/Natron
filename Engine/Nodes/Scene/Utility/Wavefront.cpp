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
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/tf/stringUtils.h>

#include "Engine/Nodes/Scene/Utility/UtilityLight.h"
#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"
#include "Engine/Nodes/Scene/Utility/UtilityProfile.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
const double kNdcNear = -1.;
const double kNdcFar = 1.;
const float kFallbackGrey = 0.18f;
// How many triangles or points the shading tables copy between stop checks: one mesh can hold
// millions, which would otherwise copy for hundreds of milliseconds before a stop lands.
const std::size_t kCopyChunk = std::size_t(1) << 16;
// The dome's importance table is a box-filtered copy of the texture no larger than this, which keeps
// its build and its binary searches cheap without losing the bright spots a sun or window makes.
const int kDomeCdfMaxWidth = 512;
const int kDomeCdfMaxHeight = 256;

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

// Whether count values cover what the binding will index them with.
bool
valuesFitBinding(UtilityScene::ColorBinding binding,
                 std::size_t count,
                 const VtVec3iArray& triangles)
{
    switch (binding) {
    case UtilityScene::eColorBindingConstant:
        return count >= 1;
    case UtilityScene::eColorBindingTriangle:
        return count >= triangles.size();
    case UtilityScene::eColorBindingCorner:
        return count >= triangles.size() * 3;
    case UtilityScene::eColorBindingVertex:
        for (const GfVec3i& triangle : triangles) {
            for (int corner = 0; corner < 3; ++corner) {
                if (triangle[corner] < 0 || static_cast<std::size_t>(triangle[corner]) >= count) {
                    return false;
                }
            }
        }

        return true;
    }

    return false;
}

void
store(const GfVec3f& v,
      float w,
      float* destination)
{
    destination[0] = v[0];
    destination[1] = v[1];
    destination[2] = v[2];
    destination[3] = w;
}

void
store(const GfVec4f& v,
      float* destination)
{
    for (int i = 0; i < 4; ++i) {
        destination[i] = v[i];
    }
}

void
storeMatrix(const GfMatrix4d& m,
            float* destination)
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            destination[r * 4 + c] = static_cast<float>(m[r][c]);
        }
    }
}

void
appendRows(const GfMatrix4d& m,
           std::vector<GfVec4f>* rows)
{
    for (int r = 0; r < 4; ++r) {
        rows->push_back(GfVec4f(static_cast<float>(m[r][0]), static_cast<float>(m[r][1]), static_cast<float>(m[r][2]), static_cast<float>(m[r][3])));
    }
}

KernelTextureBinding
makeBinding(const UtilityTextureRef& ref,
            int image,
            const UtilityTextureAsset* asset)
{
    const bool udim = asset && asset->udim;
    const UtilityTextureImage* metadata = nullptr;

    if (asset) {
        for (const UtilityTextureImageConstPtr& tile : asset->tiles) {
            if (tile) {
                metadata = tile.get();
                break;
            }
        }
    }
    const auto resolve = [metadata](UtilityTextureRef::Wrap wrap, int dimension) {
        if (wrap != UtilityTextureRef::eWrapUseMetadata) {
            return wrap;
        }

        return metadata ? metadata->metadataWrap[dimension] : UtilityTextureRef::eWrapBlack;
    };

    KernelTextureBinding binding {};
    const double radians = ref.uvRotationDegrees * M_PI / 180.;

    store(ref.scale, binding.scale);
    store(ref.bias, binding.bias);
    store(ref.fallback, binding.fallback);
    binding.uvTransform[0] = ref.uvScale[0];
    binding.uvTransform[1] = ref.uvScale[1];
    binding.uvTransform[2] = ref.uvTranslation[0];
    binding.uvTransform[3] = ref.uvTranslation[1];
    binding.uvRotation[0] = static_cast<float>(std::cos(radians));
    binding.uvRotation[1] = static_cast<float>(std::sin(radians));
    binding.image[0] = image;
    binding.image[1] = udim ? 1 : 0;
    binding.image[2] = resolve(ref.wrapS, 0);
    binding.image[3] = resolve(ref.wrapT, 1);
    binding.channels[0] = static_cast<std::uint32_t>(std::max(0, std::min(3, ref.firstChannel)));

    return binding;
}
// The unit direction of v, and its length.
float
splitAxis(const GfVec3d& v,
          float* axis)
{
    const double length = v.GetLength();
    const GfVec3d unit = length > 0. ? v / length : GfVec3d(0.);

    axis[0] = static_cast<float>(unit[0]);
    axis[1] = static_cast<float>(unit[1]);
    axis[2] = static_cast<float>(unit[2]);

    return static_cast<float>(length);
}

} // anon namespace

/**
 * @brief Appends to data the dome's importance table for image: a conditional CDF per row, then the
 * CDF over rows, each texel weighted by its luminance and by the solid angle its row covers. Returns
 * false, appending nothing, when the image has no light in it.
 *
 * The bins split the image's uv square evenly, as the kernel maps a bin back to uv, so a bin of an
 * image its size does not divide covers a fractional run of texels: it weighs the mean of every
 * texel it overlaps, and every texel, the last row and column too, lands in some bin.
 **/
bool
wavefrontAppendDomeCdf(const UtilityTextureImage& image,
                       std::vector<float>* data,
                       std::uint32_t* width,
                       std::uint32_t* height)
{
    if (image.width <= 0 || image.height <= 0 || image.texels.size() < static_cast<std::size_t>(image.width) * image.height) {
        return false;
    }

    const int step = std::max({ 1, (image.width + kDomeCdfMaxWidth - 1) / kDomeCdfMaxWidth, (image.height + kDomeCdfMaxHeight - 1) / kDomeCdfMaxHeight });
    const int w = (image.width + step - 1) / step;
    const int h = (image.height + step - 1) / step;
    // The texels [first, last) bin i of n overlaps along an axis of size texels.
    const auto span = [](int i, int n, int size) {
        const std::int64_t first = static_cast<std::int64_t>(i) * size / n;
        const std::int64_t last = (static_cast<std::int64_t>(i + 1) * size + n - 1) / n;

        return std::make_pair(static_cast<int>(first), static_cast<int>(std::min<std::int64_t>(last, size)));
    };
    std::vector<double> rows(h, 0.);
    std::vector<float> table(static_cast<std::size_t>(w) * h + h);

    for (int y = 0; y < h; ++y) {
        const double cosLat = std::cos(((y + 0.5) / h - 0.5) * M_PI);
        const std::pair<int, int> texelRows = span(y, h, image.height);
        double sum = 0.;
        float* conditional = table.data() + static_cast<std::size_t>(y) * w;

        for (int x = 0; x < w; ++x) {
            const std::pair<int, int> texelColumns = span(x, w, image.width);
            double luminance = 0.;

            for (int ty = texelRows.first; ty < texelRows.second; ++ty) {
                for (int tx = texelColumns.first; tx < texelColumns.second; ++tx) {
                    const GfVec4f& t = image.texels[static_cast<std::size_t>(ty) * image.width + tx];

                    luminance += 0.2126 * t[0] + 0.7152 * t[1] + 0.0722 * t[2];
                }
            }
            luminance /= static_cast<double>(texelRows.second - texelRows.first) * (texelColumns.second - texelColumns.first);
            sum += std::max(0., luminance) * cosLat;
            conditional[x] = static_cast<float>(sum);
        }
        for (int x = 0; x < w; ++x) {
            conditional[x] = sum > 0. ? static_cast<float>(conditional[x] / sum) : static_cast<float>(x + 1) / w;
        }
        conditional[w - 1] = 1.f;
        rows[y] = sum;
    }

    double total = 0.;

    for (double r : rows) {
        total += r;
    }
    if (!(total > 0.) || !std::isfinite(total)) {
        return false;
    }

    double running = 0.;
    float* marginal = table.data() + static_cast<std::size_t>(w) * h;

    for (int y = 0; y < h; ++y) {
        running += rows[y];
        marginal[y] = static_cast<float>(running / total);
    }
    marginal[h - 1] = 1.f;
    data->insert(data->end(), table.begin(), table.end());
    *width = static_cast<std::uint32_t>(w);
    *height = static_cast<std::uint32_t>(h);

    return true;
}

namespace {
/**
 * @brief The light in world space as the kernel reads it, its emission divided by its size when it
 * normalizes. Appends its IES table to data.
 **/
KernelLightRecord
makeLightRecord(const UtilityLightDesc& desc,
                std::vector<float>* data)
{
    KernelLightRecord record {};
    const GfMatrix4d& m = desc.transform;
    const GfVec3d origin = m.Transform(GfVec3d(0.));
    const float sx = splitAxis(m.TransformDir(GfVec3d(1., 0., 0.)), record.axisX);
    const float sy = splitAxis(m.TransformDir(GfVec3d(0., 1., 0.)), record.axisY);
    const float sz = splitAxis(m.TransformDir(GfVec3d(0., 0., 1.)), record.axisZ);
    float area = 0.;
    float sizeFactor = 1.;
    std::uint32_t flags = desc.shadowEnable ? kKernelLightCastsShadows : 0u;

    record.position[0] = static_cast<float>(origin[0]);
    record.position[1] = static_cast<float>(origin[1]);
    record.position[2] = static_cast<float>(origin[2]);

    switch (desc.type) {
    case UtilityLightDesc::eTypeDistant: {
        const double halfAngle = std::max(0., std::min(180., static_cast<double>(desc.angleDegrees))) * M_PI / 360.;
        const double sin2 = std::sin(halfAngle) * std::sin(halfAngle);

        record.axisZ[3] = halfAngle > 0. ? static_cast<float>(std::cos(halfAngle)) : 1.f;
        if (halfAngle > 0.) {
            sizeFactor = static_cast<float>(halfAngle <= M_PI / 2. ? M_PI * sin2 : (2. - sin2) * M_PI);
        }
        break;
    }
    case UtilityLightDesc::eTypeSphere: {
        const float radius = std::max(0.f, desc.radius) * std::max({ sx, sy, sz });

        record.position[3] = radius;
        area = static_cast<float>(4. * M_PI) * radius * radius;
        sizeFactor = area;
        if (desc.treatAsPoint || radius <= 0.f) {
            flags |= kKernelLightPoint;
        }
        break;
    }
    case UtilityLightDesc::eTypeRect:
        record.axisX[3] = 0.5f * std::max(0.f, desc.width) * sx;
        record.axisY[3] = 0.5f * std::max(0.f, desc.height) * sy;
        area = 4.f * record.axisX[3] * record.axisY[3];
        sizeFactor = area;
        break;
    case UtilityLightDesc::eTypeDisk:
        record.axisX[3] = std::max(0.f, desc.radius) * sx;
        record.axisY[3] = std::max(0.f, desc.radius) * sy;
        area = static_cast<float>(M_PI) * record.axisX[3] * record.axisY[3];
        sizeFactor = area;
        break;
    case UtilityLightDesc::eTypeCylinder:
        record.axisX[3] = 0.5f * std::max(0.f, desc.length) * sx;
        record.position[3] = std::max(0.f, desc.radius) * std::max(sy, sz);
        area = static_cast<float>(2. * M_PI) * record.position[3] * 2.f * record.axisX[3];
        sizeFactor = area;
        break;
    case UtilityLightDesc::eTypeDome:
        break;
    }

    GfVec3f radiance = desc.emission;

    if (desc.normalize && desc.type != UtilityLightDesc::eTypeDome && sizeFactor > 0.f) {
        radiance /= sizeFactor;
    }
    if ((flags & kKernelLightPoint) != 0) {
        // A point stands for a small sphere of the same power, whose intensity is its radiance times
        // the disc it shows; without a radius the emission is taken as the intensity.
        const float radius = record.position[3];

        if (radius > 0.f) {
            radiance *= static_cast<float>(M_PI) * radius * radius;
        }
    }
    store(radiance, area, record.radiance);

    const bool shapes = desc.type != UtilityLightDesc::eTypeDistant && desc.type != UtilityLightDesc::eTypeDome;

    if (shapes && (desc.coneAngleDegrees < 180.f || desc.focus > 0.f)) {
        flags |= kKernelLightShaped;
    }
    record.shaping[0] = static_cast<float>(std::max(0., std::min(180., static_cast<double>(desc.coneAngleDegrees))) * M_PI / 180.);
    record.shaping[1] = desc.coneSoftness;
    record.shaping[2] = desc.focus;
    record.shaping[3] = desc.iesAngleScale;
    store(desc.focusTint, 1.f, record.focusTint);
    if (shapes && desc.ies && desc.ies->thetaCount >= 2 && desc.ies->phiCount >= 1 && desc.ies->thetaCount <= 0xffff && desc.ies->phiCount <= 0xffff) {
        flags |= kKernelLightIes;
        record.kind[3] = static_cast<std::uint32_t>(data->size());
        record.data[3] = static_cast<std::uint32_t>(desc.ies->thetaCount) | (static_cast<std::uint32_t>(desc.ies->phiCount) << 16);
        data->insert(data->end(), desc.ies->candela.begin(), desc.ies->candela.end());
        if (desc.iesNormalize && desc.ies->mean > 0.f) {
            record.focusTint[3] = 1.f / desc.ies->mean;
        }
    }
    record.kind[0] = static_cast<std::uint32_t>(desc.type);
    record.kind[1] = flags;
    record.kind[2] = static_cast<std::uint32_t>(-1);

    return record;
}

// Whether a geometry's material may let light through it somewhere, as Kernels/Surface.slang's
// surfaceTransmission() decides it per hit.
bool
mayTransmit(const KernelMaterialRecord& material,
            const UtilityScene::Geometry& geometry)
{
    if (material.kind[0] != static_cast<std::uint32_t>(UtilityMaterialDesc::eKindPreviewSurface) || material.coverage[1] > 0.f) {
        return false;
    }
    if (material.textures[UtilityMaterialDesc::eSlotOpacity] >= 0) {
        return true;
    }
    if ((material.kind[2] & kKernelMaterialOpacityFromDisplayOpacity) != 0 && geometry.hasDisplayOpacity) {
        return std::any_of(geometry.colors.cbegin(), geometry.colors.cend(), [](const GfVec4f& color) { return color[3] < 1.f; });
    }

    return material.coverage[0] < 1.f;
}

// The camera as the spherical and cylindrical mappings see it.
struct MappedCamera {
    GfVec3d apex;
    GfVec3d right;
    GfVec3d up;
    GfVec3d forward;
    double nearDistance = 0.;
    double farDistance = 0.;
    // The display window's bottom and top: slopes off the forward axis for a perspective camera,
    // heights above the apex for an orthographic one.
    double bottom = 0.;
    double top = 0.;
    bool orthographic = false;
    WavefrontRect display;
};

WavefrontRect
displayOf(const WavefrontFrame& frame)
{
    return (frame.display.width > 0 && frame.display.height > 0) ? frame.display : frame.window;
}

MappedCamera
mappedCamera(const WavefrontFrame& frame)
{
    MappedCamera camera;
    const GfMatrix4d viewToWorld = frame.worldToView.GetInverse();
    const GfMatrix4d viewFromNdc = frame.projection.GetInverse();

    camera.apex = viewToWorld.Transform(GfVec3d(0.));
    camera.right = viewToWorld.TransformDir(GfVec3d(1., 0., 0.)).GetNormalized();
    camera.up = viewToWorld.TransformDir(GfVec3d(0., 1., 0.)).GetNormalized();
    camera.forward = viewToWorld.TransformDir(GfVec3d(0., 0., -1.)).GetNormalized();
    camera.nearDistance = -viewFromNdc.Transform(GfVec3d(0., 0., kNdcNear))[2];
    camera.farDistance = -viewFromNdc.Transform(GfVec3d(0., 0., kNdcFar))[2];
    camera.orthographic = frame.projection[2][3] == 0.;
    camera.display = displayOf(frame);

    const double height = std::max(1, frame.window.height);
    const auto edge = [&](int row) {
        const double ndcY = 2. * (row - frame.window.y) / height - 1.;
        const GfVec3d p = viewFromNdc.Transform(GfVec3d(0., ndcY, kNdcNear));

        return camera.orthographic ? p[1] : p[1] / -p[2];
    };

    camera.bottom = edge(camera.display.y);
    camera.top = edge(camera.display.y + camera.display.height);

    return camera;
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
    KernelRayGenParams params;
    WavefrontRect window = frame.window;

    if (frame.mapping == WavefrontMapping::Spherical || frame.mapping == WavefrontMapping::Cylindrical) {
        const MappedCamera camera = mappedCamera(frame);

        storePosition(camera.apex, params.nearAtCentre);
        storePosition(camera.right, params.nearPerX);
        storePosition(camera.up, params.nearPerY);
        storePosition(camera.forward, params.farAtCentre);
        storePosition(GfVec3d(camera.nearDistance, camera.farDistance, 0.), params.farPerX);
        storePosition(GfVec3d(camera.bottom, camera.top, camera.orthographic ? 1. : 0.), params.farPerY);
        params.mapping = (frame.mapping == WavefrontMapping::Spherical) ? eKernelRayMappingSpherical : eKernelRayMappingCylindrical;
        window = camera.display;
    } else if (frame.mapping == WavefrontMapping::UvUnwrap) {
        const float zero[3] = { 0.f, 0.f, 0.f };

        std::copy(zero, zero + 3, params.nearAtCentre);
        std::copy(zero, zero + 3, params.nearPerX);
        std::copy(zero, zero + 3, params.nearPerY);
        std::copy(zero, zero + 3, params.farAtCentre);
        std::copy(zero, zero + 3, params.farPerX);
        std::copy(zero, zero + 3, params.farPerY);
        params.mapping = eKernelRayMappingUvUnwrap;
    } else {
        const GfMatrix4d inverse = (frame.worldToView * frame.projection).GetInverse();
        const GfVec3d nearAtCentre = inverse.Transform(GfVec3d(0., 0., kNdcNear));
        const GfVec3d farAtCentre = inverse.Transform(GfVec3d(0., 0., kNdcFar));

        storePosition(nearAtCentre, params.nearAtCentre);
        storePosition(inverse.Transform(GfVec3d(1., 0., kNdcNear)) - nearAtCentre, params.nearPerX);
        storePosition(inverse.Transform(GfVec3d(0., 1., kNdcNear)) - nearAtCentre, params.nearPerY);
        storePosition(farAtCentre, params.farAtCentre);
        storePosition(inverse.Transform(GfVec3d(1., 0., kNdcFar)) - farAtCentre, params.farPerX);
        storePosition(inverse.Transform(GfVec3d(0., 1., kNdcFar)) - farAtCentre, params.farPerY);
        params.mapping = eKernelRayMappingCamera;
    }
    const WavefrontSampling& sampling = frame.sampling;
    params.filterWidth = sampling.filterWidth;
    params.sampleIndex = static_cast<std::uint32_t>(std::max(0, sampling.sampleIndex));
    params.seed = sampling.seed;
    params.samplesPerSide = static_cast<std::uint32_t>(std::max(1, sampling.samplesPerSide));
    params.filter = sampling.filter;
    params.windowX = window.x;
    params.windowY = window.y;
    params.windowWidth = window.width;
    params.windowHeight = window.height;
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
    KernelShadeParams params {};

    params.projection[0] = static_cast<float>(projection[2][2]);
    params.projection[1] = static_cast<float>(projection[3][2]);
    params.projection[2] = static_cast<float>(projection[2][3]);
    params.projection[3] = static_cast<float>(projection[3][3]);
    params.viewZAtNear = static_cast<float>(inverse.Transform(GfVec3d(0., 0., kNdcNear))[2]);
    params.viewZAtFar = static_cast<float>(inverse.Transform(GfVec3d(0., 0., kNdcFar))[2]);
    params.count = count;
    params.padding = 0;

    // A pixel's width on the near and far planes; between them it grows linearly along every ray.
    const GfMatrix4d toWorld = (frame.worldToView * projection).GetInverse();
    const double width = std::max(1, frame.window.width);
    const double height = std::max(1, frame.window.height);

    if (frame.mapping == WavefrontMapping::UvUnwrap) {
        // Every hit lies on the surface the texel looks at, so its depth is the near plane's.
        params.viewZAtFar = params.viewZAtNear;
        params.pixelFootprint[0] = static_cast<float>(frame.unwrap ? frame.unwrap->footprint : 0.);
        params.pixelFootprint[1] = params.pixelFootprint[0];
    } else if (frame.mapping == WavefrontMapping::Spherical || frame.mapping == WavefrontMapping::Cylindrical) {
        const MappedCamera camera = mappedCamera(frame);
        const double across = 2. * M_PI / std::max(1, camera.display.width);
        const double down = ((frame.mapping == WavefrontMapping::Spherical) ? M_PI : std::abs(camera.top - camera.bottom)) / std::max(1, camera.display.height);

        for (int plane = 0; plane < 2; ++plane) {
            const double distance = std::max(0., plane == 0 ? camera.nearDistance : camera.farDistance);
            const double vertical = (frame.mapping == WavefrontMapping::Cylindrical && camera.orthographic) ? down : distance * down;

            params.pixelFootprint[plane] = static_cast<float>(std::sqrt(distance * across * vertical));
        }
    } else {
        for (int plane = 0; plane < 2; ++plane) {
            const double ndcZ = plane == 0 ? kNdcNear : kNdcFar;
            const GfVec3d centre = toWorld.Transform(GfVec3d(0., 0., ndcZ));
            const double perX = (toWorld.Transform(GfVec3d(1., 0., ndcZ)) - centre).GetLength() * 2. / width;
            const double perY = (toWorld.Transform(GfVec3d(0., 1., ndcZ)) - centre).GetLength() * 2. / height;

            params.pixelFootprint[plane] = static_cast<float>(std::sqrt(perX * perY));
        }
    }
    params.pixelFootprint[2] = 0.f;
    params.pixelFootprint[3] = 0.f;
    params.pixels[0] = static_cast<std::uint32_t>(frame.window.x);
    params.pixels[1] = static_cast<std::uint32_t>(frame.window.y);
    params.pixels[2] = static_cast<std::uint32_t>(std::max(1, frame.window.width));
    // Each pixel sample draws its own light samples, so that a pixel's samples average over the lights' areas too.
    params.pixels[3] = frame.sampling.seed * 0x9e3779b9u + static_cast<std::uint32_t>(std::max(0, frame.sampling.sampleIndex));
    // No bounces until an executor sets how many the frame traces, wavefrontBounceLimit().
    params.bounce[0] = 0;
    params.bounce[1] = 0;
    params.bounce[2] = frame.reflectionDepth;
    params.bounce[3] = frame.refractionDepth;

    return params;
}

std::uint32_t
wavefrontBounceLimit(const WavefrontFrame& frame,
                     bool transmission)
{
    if (frame.mapping == WavefrontMapping::UvUnwrap && !frame.unwrapBounces) {
        return 0;
    }

    return transmission ? frame.maxRayDepth : std::min(frame.maxRayDepth, frame.reflectionDepth);
}

namespace {
void
appendMotionStep(const UtilityScene::Decomposition& step,
                 std::vector<float>* rows)
{
    const double* u = step.stretch;
    const double values[16] = {
        u[0],
        u[1],
        u[2],
        0.,
        u[3],
        u[4],
        u[5],
        0.,
        step.translation[0],
        step.translation[1],
        step.translation[2],
        0.,
        step.rotation[1],
        step.rotation[2],
        step.rotation[3],
        step.rotation[0],
    };

    for (double value : values) {
        rows->push_back(static_cast<float>(value));
    }
}

void
appendPosition(const float position[3],
               std::vector<float>* rows)
{
    rows->insert(rows->end(), position, position + 3);
    rows->push_back(0.f);
}
} // anon namespace

bool
WavefrontShadeTables::updateMaterials(const UtilityScene::Snapshot& snapshot,
                                      const std::atomic<bool>* stop)
{
    std::vector<UtilityTextureAssetConstPtr> assets;
    std::map<const UtilityTextureAsset*, int> firstImages;
    std::set<UtilityTexturePool::Key> keys;
    int imageCount = 0;

    _materials.clear();
    _textureBindings.clear();
    _projections.clear();
    _materialIndices.clear();
    _holes = false;

    const auto addBinding = [&](const UtilityTextureRef& ref) {
        UtilityTextureAssetConstPtr asset;

        if (!ref.asset.empty() && snapshot.textures) {
            keys.insert(UtilityTexturePool::Key(ref.asset, static_cast<int>(ref.colorSpace)));
            asset = snapshot.textures->acquire(ref.asset, ref.colorSpace, stop);
        }
        int image = -1;

        if (asset) {
            const auto found = firstImages.find(asset.get());

            if (found == firstImages.end()) {
                image = imageCount;
                firstImages[asset.get()] = image;
                imageCount += static_cast<int>(asset->tiles.size());
                assets.push_back(asset);
            } else {
                image = found->second;
            }
        }
        _textureBindings.push_back(makeBinding(ref, image, asset.get()));

        return asset;
    };
    const auto addMaterial = [&](const UtilityMaterialDesc& desc) {
        KernelMaterialRecord record {};
        const int firstBinding = static_cast<int>(_textureBindings.size());

        for (const UtilityTextureRef& ref : desc.textureRefs) {
            addBinding(ref);
        }

        store(desc.diffuseColor, desc.normal[0], record.diffuseColor);
        store(desc.emissiveColor, desc.normal[1], record.emissiveColor);
        store(desc.specularColor, desc.useSpecularWorkflow ? 1.f : 0.f, record.specularColor);
        store(GfVec4f(desc.metallic, desc.roughness, desc.clearcoat, desc.clearcoatRoughness), record.surface);
        store(GfVec4f(desc.opacity, desc.opacityThreshold, desc.ior, desc.normal[2]), record.coverage);
        record.kind[0] = static_cast<std::uint32_t>(desc.kind);
        record.kind[2] = (desc.diffuseFromDisplayColor ? kKernelMaterialDiffuseFromDisplayColor : 0u)
            | (desc.opacityFromDisplayOpacity ? kKernelMaterialOpacityFromDisplayOpacity : 0u)
            | (desc.emissiveFromDisplayColor ? kKernelMaterialEmissiveFromDisplayColor : 0u)
            | (desc.opacityPresence ? kKernelMaterialOpacityPresence : 0u);
        for (int slot = 0; slot < 12; ++slot) {
            const int t = slot < UtilityMaterialDesc::eSlotCount ? desc.textures[slot] : -1;

            record.textures[slot] = t >= 0 ? firstBinding + t : -1;
        }
        if (desc.kind == UtilityMaterialDesc::eKindProject3D) {
            const UtilityProjection& p = desc.projection;
            KernelProjectionRecord projection {};

            storeMatrix(p.worldToProjector, projection.worldToProjector);
            storeMatrix(p.geometryFrame, projection.geometryFrame);
            storeMatrix(p.geometryFrame.GetInverse(), projection.geometryFrameInverse);
            store(p.plateTransform, projection.plateTransform);
            store(p.projectorOrigin, projection.projectorOrigin);
            projection.settings[0] = p.projectionSpace;
            projection.settings[1] = p.projectOn;
            projection.settings[2] = p.crop;
            projection.settings[3] = p.clip;
            projection.plate[0] = p.outside;
            projection.plate[1] = p.plate >= 0 ? firstBinding + p.plate : -1;
            record.kind[1] = static_cast<std::uint32_t>(_projections.size());
            _projections.push_back(projection);
        }
        _holes = _holes || desc.kind == UtilityMaterialDesc::eKindProject3D
            || (desc.kind == UtilityMaterialDesc::eKindPreviewSurface && desc.opacityThreshold > 0.f);
        _materials.push_back(record);
    };

    const auto halted = [stop]() { return stop && stop->load(std::memory_order_relaxed); };

    addMaterial(UtilityMaterialDesc::fallback());
    for (const auto& entry : snapshot.materials) {
        if (halted()) {
            return false;
        }
        if (entry.second) {
            _materialIndices[entry.first] = static_cast<std::uint32_t>(_materials.size());
            addMaterial(*entry.second);
        }
    }

    _lights.clear();
    _lightData.clear();
    _lightLinks.clear();
    _shadowLinked = false;
    _background = false;
    for (const auto& entry : snapshot.lights) {
        if (halted()) {
            return false;
        }
        if (!entry.second) {
            continue;
        }

        const UtilityLightDesc& desc = *entry.second;
        KernelLightRecord record = makeLightRecord(desc, &_lightData);

        if (desc.type == UtilityLightDesc::eTypeDome && !desc.domeTexture.asset.empty()) {
            record.kind[2] = static_cast<std::uint32_t>(_textureBindings.size());

            const UtilityTextureAssetConstPtr asset = addBinding(desc.domeTexture);

            if (asset && !asset->udim && !asset->tiles.empty() && asset->tiles[0]) {
                record.data[0] = static_cast<std::uint32_t>(_lightData.size());
                if (wavefrontAppendDomeCdf(*asset->tiles[0], &_lightData, &record.data[1], &record.data[2])) {
                    record.kind[1] |= kKernelLightDomeImportance;
                }
            }
        }
        _background = _background || desc.type == UtilityLightDesc::eTypeDome;
        _lights.push_back(record);
        _lightLinks.push_back(std::make_pair(desc.lightLink, desc.shadowLink));
        _shadowLinked = _shadowLinked || !desc.shadowLink.IsEmpty();
    }

    // The last texture's load may have been the one stopped, and retainOnly() must not drop what the
    // materials not reached still read.
    if (halted()) {
        return false;
    }
    if (snapshot.textures) {
        snapshot.textures->retainOnly(keys);
    }
    if (assets == _textureAssets && _texelVersion != 0 && _texelsComplete) {
        return true;
    }

    _textureAssets.swap(assets);

    return buildTexels(stop);
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
        const UtilityScene::Geometry& geometry = entry.second;

        sources.push_back(SourceVersion { entry.first, geometry.colorVersion, geometry.topologyVersion, geometry.pointsVersion, geometry.stVersion, geometry.normalsVersion, geometry.points.size() });
    }

    const bool recopy = !_synced || sources != _sources;

    _synced = false;
    if (!updateMaterials(snapshot, stop)) {
        return false;
    }
    if (recopy) {
        std::size_t colorTotal = 0;
        std::size_t triangleTotal = 0;
        std::size_t pointTotal = 0;
        std::size_t stTotal = 0;
        std::size_t normalTotal = 0;

        for (const auto& entry : snapshot.geometries) {
            const UtilityScene::Geometry& geometry = entry.second;

            colorTotal += std::max<std::size_t>(1, geometry.colors.size());
            triangleTotal += geometry.triangles.size() * 3;
            pointTotal += geometry.points.size() * std::max<std::size_t>(1, geometry.pointSteps.size());
            stTotal += geometry.st.size();
            normalTotal += geometry.normals.size() * std::max<std::size_t>(1, geometry.normalSteps.size());
        }
        _colors.clear();
        _triangles.clear();
        _points.clear();
        _sts.clear();
        _normals.clear();
        _colors.reserve(colorTotal);
        _triangles.reserve(triangleTotal);
        _points.reserve(pointTotal);
        _sts.reserve(stTotal);
        _normals.reserve(normalTotal);
    }
    _geometries.clear();
    _geometries.reserve(snapshot.geometries.size());
    _transmission = false;

    std::uint32_t colorOffset = 0;
    std::uint32_t triangleOffset = 0;
    std::uint32_t pointOffset = 0;
    std::uint32_t stOffset = 0;
    std::uint32_t normalOffset = 0;

    for (const auto& entry : snapshot.geometries) {
        if (stop && stop->load(std::memory_order_relaxed)) {
            return false;
        }

        const UtilityScene::Geometry& geometry = entry.second;
        const bool fits = valuesFitBinding(geometry.colorBinding, geometry.colors.size(), geometry.triangles);
        const bool stFits = geometry.hasSt && valuesFitBinding(geometry.stBinding, geometry.st.size(), geometry.triangles);
        const std::uint32_t colorCount = fits ? static_cast<std::uint32_t>(geometry.colors.size()) : 1;
        const std::uint32_t stCount = stFits ? static_cast<std::uint32_t>(geometry.st.size()) : 0;
        const bool normalsFit = !geometry.normals.empty() && valuesFitBinding(geometry.normalBinding, geometry.normals.size(), geometry.triangles);
        const std::uint32_t normalCount = normalsFit ? static_cast<std::uint32_t>(geometry.normals.size()) : 0;
        const std::uint32_t triangleCount = static_cast<std::uint32_t>(geometry.triangles.size());
        const std::uint32_t pointCount = static_cast<std::uint32_t>(geometry.points.size());
        const std::uint32_t pointSteps = static_cast<std::uint32_t>(std::max<std::size_t>(1, geometry.pointSteps.size()));
        const std::uint32_t normalSteps = normalsFit ? static_cast<std::uint32_t>(std::max<std::size_t>(1, geometry.normalSteps.size())) : 1u;
        const auto material = geometry.materialId.IsEmpty() ? _materialIndices.end() : _materialIndices.find(geometry.materialId);
        const KernelGeometryRecord record {
            colorOffset,
            static_cast<std::uint32_t>(fits ? geometry.colorBinding : UtilityScene::eColorBindingConstant)
                | (geometry.hasDisplayColor ? 0u : kKernelColorNoDisplayColor)
                | (geometry.hasDisplayOpacity ? 0u : kKernelColorNoDisplayOpacity),
            geometry.primId,
            triangleOffset,
            material == _materialIndices.end() ? 0u : material->second,
            stOffset,
            stFits ? static_cast<std::uint32_t>(geometry.stBinding) : kKernelStBindingNone,
            pointOffset,
            normalOffset,
            normalsFit ? static_cast<std::uint32_t>(geometry.normalBinding) : kKernelNormalBindingNone,
            pointSteps,
            pointCount,
            normalSteps,
            normalCount,
        };

        _geometries.push_back(record);
        _transmission = _transmission || mayTransmit(_materials[record.materialIndex], geometry);
        if (recopy) {
            if (fits) {
                _colors.insert(_colors.end(), geometry.colors.cbegin(), geometry.colors.cend());
            } else {
                _colors.push_back(GfVec4f(kFallbackGrey, kFallbackGrey, kFallbackGrey, 1.f));
            }
            const GfVec3i* triangles = geometry.triangles.cdata();

            for (std::size_t first = 0; first < geometry.triangles.size(); first += kCopyChunk) {
                if (stop && stop->load(std::memory_order_relaxed)) {
                    return false;
                }

                const std::size_t last = std::min(first + kCopyChunk, geometry.triangles.size());

                for (std::size_t t = first; t < last; ++t) {
                    _triangles.insert(_triangles.end(), { triangles[t][0], triangles[t][1], triangles[t][2] });
                }
            }

            for (std::uint32_t step = 0; step < pointSteps; ++step) {
                const GfVec3f* points = geometry.pointSteps.empty() ? geometry.points.cdata() : geometry.pointSteps[step].cdata();

                for (std::size_t first = 0; first < pointCount; first += kCopyChunk) {
                    if (stop && stop->load(std::memory_order_relaxed)) {
                        return false;
                    }

                    const std::size_t last = std::min<std::size_t>(first + kCopyChunk, pointCount);

                    for (std::size_t p = first; p < last; ++p) {
                        _points.push_back(GfVec4f(points[p][0], points[p][1], points[p][2], 1.f));
                    }
                }
            }
            if (stFits) {
                _sts.insert(_sts.end(), geometry.st.cbegin(), geometry.st.cend());
            }

            for (std::uint32_t step = 0; normalsFit && step < normalSteps; ++step) {
                const GfVec3f* normals = geometry.normalSteps.empty() ? geometry.normals.cdata() : geometry.normalSteps[step].cdata();

                for (std::size_t first = 0; first < normalCount; first += kCopyChunk) {
                    if (stop && stop->load(std::memory_order_relaxed)) {
                        return false;
                    }

                    const std::size_t last = std::min<std::size_t>(first + kCopyChunk, normalCount);

                    for (std::size_t n = first; n < last; ++n) {
                        _normals.push_back(GfVec4f(normals[n][0], normals[n][1], normals[n][2], 0.f));
                    }
                }
            }
        }
        colorOffset += colorCount;
        triangleOffset += triangleCount * 3;
        pointOffset += pointCount * pointSteps;
        stOffset += stCount;
        normalOffset += normalCount * normalSteps;
    }

    _instanceIds.clear();
    _instanceIds.reserve(snapshot.instances.size());
    _instanceTransforms.clear();
    _instanceTransforms.reserve(snapshot.instances.size() * 8);
    _instanceMotion.assign(snapshot.instances.size(), kKernelNoMotion);
    _motionRows.clear();
    _linkWords = static_cast<std::uint32_t>((_lights.size() + 31) / 32);
    _linkMasks.assign(snapshot.instances.size() * 2 * _linkWords, 0u);
    for (std::size_t i = 0; i < snapshot.instances.size(); ++i) {
        const UtilityScene::Instance& instance = snapshot.instances[i];
        const auto geometry = snapshot.geometries.find(instance.geometry);
        static const TfTokenVector noCategories;
        const TfTokenVector& categories = geometry == snapshot.geometries.end() ? noCategories : geometry->second.categories;
        const auto inCategory = [&](const TfToken& category) {
            return category.IsEmpty() || std::find(categories.begin(), categories.end(), category) != categories.end()
                || std::find(instance.categories.begin(), instance.categories.end(), category) != instance.categories.end();
        };
        std::uint32_t* lightMask = _linkMasks.data() + i * 2 * _linkWords;
        std::uint32_t* shadowMask = lightMask + _linkWords;

        _instanceIds.push_back(instance.instanceIndex);
        appendRows(instance.transform, &_instanceTransforms);
        appendRows(instance.transform.GetInverse(), &_instanceTransforms);
        if (instance.transformSteps.size() > 1) {
            _instanceMotion[i] = static_cast<std::uint32_t>(_motionRows.size() / 4);
            _motionRows.push_back(std::bit_cast<float>(static_cast<std::uint32_t>(instance.transformSteps.size())));
            _motionRows.insert(_motionRows.end(), 3, 0.f);
            for (const UtilityScene::Decomposition& step : instance.transformSteps) {
                appendMotionStep(step, &_motionRows);
            }
        }
        for (std::size_t l = 0; l < _lightLinks.size(); ++l) {
            const std::uint32_t bit = 1u << (l % 32);

            if (inCategory(_lightLinks[l].first)) {
                lightMask[l / 32] |= bit;
            }
            if (inCategory(_lightLinks[l].second)) {
                shadowMask[l / 32] |= bit;
            }
        }
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
    buffers.materials = _materials.data();
    buffers.materialCount = _materials.size();
    buffers.textureBindings = _textureBindings.data();
    buffers.textureBindingCount = _textureBindings.size();
    buffers.images = _images.data();
    buffers.imageCount = _images.size() / 4;
    buffers.texels = reinterpret_cast<const float*>(_texels.data());
    buffers.texelCount = _texels.size();
    buffers.projections = _projections.data();
    buffers.projectionCount = _projections.size();
    buffers.points = reinterpret_cast<const float*>(_points.data());
    buffers.pointCount = _points.size();
    buffers.instanceTransforms = reinterpret_cast<const float*>(_instanceTransforms.data());
    buffers.instanceTransformRowCount = _instanceTransforms.size();
    buffers.sts = reinterpret_cast<const float*>(_sts.data());
    buffers.stCount = _sts.size();
    buffers.normals = reinterpret_cast<const float*>(_normals.data());
    buffers.normalCount = _normals.size();
    buffers.lights = _lights.data();
    buffers.lightCount = _lights.size();
    buffers.lightData = _lightData.data();
    buffers.lightDataCount = _lightData.size();
    buffers.linkMasks = _linkMasks.data();
    buffers.linkMaskCount = _linkMasks.size();
    buffers.instanceMotion = _instanceMotion.data();
    buffers.instanceMotionCount = _instanceMotion.size();
    buffers.motionRows = _motionRows.data();
    buffers.motionRowCount = _motionRows.size() / 4;

    return buffers;
}

std::uint64_t
WavefrontShadeTables::texelVersion() const
{
    return _texelVersion;
}

std::uint32_t
WavefrontShadeTables::lightCount() const
{
    return static_cast<std::uint32_t>(_lights.size());
}

// The texels go to the device as one table it reads through its address, so no binding limit bounds it and
// every image keeps all of its levels.
bool
WavefrontShadeTables::buildTexels(const std::atomic<bool>* stop)
{
    std::size_t total = 0;

    for (const UtilityTextureAssetConstPtr& asset : _textureAssets) {
        for (const UtilityTextureImageConstPtr& tile : asset->tiles) {
            total += tile ? tile->texels.size() : 0;
        }
    }

    _texelsComplete = false;
    _images.clear();
    _texels.clear();
    _texels.reserve(total);
    for (const UtilityTextureAssetConstPtr& asset : _textureAssets) {
        for (const UtilityTextureImageConstPtr& tile : asset->tiles) {
            if (!tile) {
                _images.insert(_images.end(), { 0u, 0u, 0u, 0u });
                continue;
            }

            const std::size_t count = tile->texels.size();

            _images.insert(_images.end(), { static_cast<std::uint32_t>(tile->width), static_cast<std::uint32_t>(tile->height), static_cast<std::uint32_t>(tile->levels), static_cast<std::uint32_t>(_texels.size()) });
            for (std::size_t first = 0; first < count; first += kCopyChunk) {
                if (stop && stop->load(std::memory_order_relaxed)) {
                    return false;
                }

                const auto from = tile->texels.begin() + static_cast<std::ptrdiff_t>(first);

                _texels.insert(_texels.end(), from, from + static_cast<std::ptrdiff_t>(std::min(kCopyChunk, count - first)));
            }
        }
    }
    ++_texelVersion;
    _texelsComplete = true;

    return true;
}

bool
WavefrontShadeTables::hasBackground() const
{
    return _background;
}

bool
wavefrontShowsBackground(const WavefrontFrame& frame,
                         const WavefrontShadeTables& tables)
{
    return tables.hasBackground() && frame.mapping != WavefrontMapping::UvUnwrap;
}

bool
WavefrontShadeTables::hasHoles() const
{
    return _holes;
}

bool
WavefrontShadeTables::hasTransmission() const
{
    return _transmission;
}

std::uint32_t
WavefrontShadeTables::linkWords() const
{
    return _linkWords;
}

OcclusionFilter
WavefrontShadeTables::occlusionFilter(const std::uint32_t* rayLights) const
{
    OcclusionFilter filter;

    filter.rayLights = rayLights;
    filter.casterMasks = _shadowLinked && _linkWords ? _linkMasks.data() + _linkWords : nullptr;
    filter.stride = 2 * _linkWords;
    filter.casterCount = _instanceIds.size();

    return filter;
}

namespace {
void
cameraAt(const WavefrontMotion& motion,
         double time,
         GfMatrix4d* view,
         GfMatrix4d* projection)
{
    const std::vector<double>& times = motion.cameraTimes;
    std::size_t next = 0;

    while (next < times.size() && times[next] < time) {
        ++next;
    }
    if (next == 0 || next == times.size()) {
        const std::size_t at = (next == 0) ? 0 : times.size() - 1;

        *view = motion.cameraViews[at];
        *projection = motion.cameraProjections[at];

        return;
    }
    const double span = times[next] - times[next - 1];
    const double u = (span > 0.) ? (time - times[next - 1]) / span : 0.;

    // The camera's own transform turns about its centre, which its inverse would not.
    *view = UtilityScene::interpolate(motion.cameraViews[next - 1].GetInverse(), motion.cameraViews[next].GetInverse(), u).GetInverse();
    *projection = motion.cameraProjections[next - 1] * (1. - u) + motion.cameraProjections[next] * u;
}
} // anon namespace

namespace {
std::uint32_t
pcgHash(std::uint32_t value)
{
    const std::uint32_t state = value * 747796405u + 2891336453u;
    const std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;

    return (word >> 22u) ^ word;
}

float
unitFloat(std::uint32_t bits)
{
    return static_cast<float>(bits >> 8u) * (1.f / 16777216.f);
}

// PixelSampling.slang's permute(): index's place in a shuffle of [0, length) that pattern chooses.
std::uint32_t
permute(std::uint32_t index,
        std::uint32_t length,
        std::uint32_t pattern)
{
    std::uint32_t mask = length - 1u;

    mask |= mask >> 1u;
    mask |= mask >> 2u;
    mask |= mask >> 4u;
    mask |= mask >> 8u;
    mask |= mask >> 16u;

    std::uint32_t i = index;

    do {
        i ^= pattern;
        i *= 0xe170893du;
        i ^= pattern >> 16u;
        i ^= (i & mask) >> 4u;
        i ^= pattern >> 8u;
        i *= 0x0929eb3fu;
        i ^= pattern >> 23u;
        i ^= (i & mask) >> 1u;
        i *= 1u | pattern >> 27u;
        i *= 0x6935fa69u;
        i ^= (i & mask) >> 11u;
        i *= 0x74dcb303u;
        i ^= (i & mask) >> 2u;
        i *= 0x9e501cc3u;
        i ^= (i & mask) >> 2u;
        i *= 0xc860a3dfu;
        i &= mask;
        i ^= i >> 5u;
    } while (i >= length);

    return (i + pattern) % length;
}

// PixelSampling.slang's filterWeight().
float
filterWeight(float t,
             KernelPixelFilter filter)
{
    const float x = 2.f * t - 1.f;
    const float pi = static_cast<float>(M_PI);

    switch (filter) {
    case eKernelPixelFilterTriangle:
        return std::max(0.f, 1.f - std::fabs(x));
    case eKernelPixelFilterGaussian:
        return std::exp(-4.5f * x * x);
    case eKernelPixelFilterBlackmanHarris:
        return std::max(0.f, 0.35875f - 0.48829f * std::cos(2.f * pi * t) + 0.14128f * std::cos(4.f * pi * t) - 0.01168f * std::cos(6.f * pi * t));
    default:
        return 1.f;
    }
}

float
radicalInverse(std::uint32_t index,
               std::uint32_t base)
{
    const double inverseBase = 1. / base;
    double factor = inverseBase;
    double value = 0.;

    while (index > 0) {
        value += (index % base) * factor;
        index /= base;
        factor *= inverseBase;
    }

    return static_cast<float>(value);
}

float
rotated(float value,
        float by)
{
    const float sum = value + by;

    return sum >= 1.f ? sum - 1.f : sum;
}

// Maps u in [0, 1) through the inverse of the filter's cumulative weight over [0, 1]. A pass's jitter is the same
// for the whole frame and its pass is also its time sample, so weighting passes by the filter would weight the
// shutter's times unevenly; drawing the jitter from the filter instead lets every pass count the same.
float
sampleFilter(float u,
             KernelPixelFilter filter)
{
    constexpr int kSteps = 256;
    std::array<float, kSteps + 1> cumulative;

    cumulative[0] = 0.f;
    for (int k = 0; k < kSteps; ++k) {
        cumulative[k + 1] = cumulative[k] + filterWeight((k + 0.5f) / kSteps, filter);
    }

    const float target = u * cumulative[kSteps];
    const int k = std::clamp(static_cast<int>(std::upper_bound(cumulative.begin() + 1, cumulative.end(), target) - cumulative.begin()) - 1, 0, kSteps - 1);
    const float span = cumulative[k + 1] - cumulative[k];
    const float within = span > 0.f ? std::clamp((target - cumulative[k]) / span, 0.f, 1.f) : 0.5f;

    return (k + within) / kSteps;
}
} // anon namespace

WavefrontRasterPass
wavefrontRasterPass(const WavefrontSampling& sampling,
                    int index,
                    int passes)
{
    WavefrontRasterPass pass;
    const std::uint32_t n = static_cast<std::uint32_t>(std::max(1, passes));
    const std::uint32_t i = static_cast<std::uint32_t>(std::clamp(index, 0, static_cast<int>(n) - 1));
    const std::uint32_t seedHash = pcgHash(sampling.seed);

    pass.time = (static_cast<float>(permute(i, n, pcgHash(seedHash ^ 0x2c1b3c6du))) + 0.5f) / static_cast<float>(n);
    if (n == 1) {
        return pass;
    }

    const float tx = sampleFilter(rotated(radicalInverse(i, 2), unitFloat(pcgHash(seedHash ^ 0x68e31da4u))), sampling.filter);
    const float ty = sampleFilter(rotated(radicalInverse(i, 3), unitFloat(pcgHash(seedHash ^ 0xb5297a4du))), sampling.filter);

    pass.jitter[0] = (tx - 0.5f) * sampling.filterWidth;
    pass.jitter[1] = (ty - 0.5f) * sampling.filterWidth;

    return pass;
}

WavefrontFrame
wavefrontFrameAt(const WavefrontFrame& frame,
                 const WavefrontMotion& motion,
                 double time)
{
    WavefrontFrame out = frame;

    if (motion.cameraTimes.empty() || motion.cameraViews.size() != motion.cameraTimes.size() || motion.cameraProjections.size() != motion.cameraTimes.size()) {
        return out;
    }
    GfMatrix4d viewAtFrame;
    GfMatrix4d projectionAtFrame;
    GfMatrix4d view;
    GfMatrix4d projection;

    cameraAt(motion, 0., &viewAtFrame, &projectionAtFrame);
    cameraAt(motion, time, &view, &projection);
    // Row vectors: the host's framing applies after the projection, and anything ahead of the view before it.
    out.worldToView = frame.worldToView * viewAtFrame.GetInverse() * view;
    out.projection = projection * projectionAtFrame.GetInverse() * frame.projection;

    return out;
}

WavefrontUnwrapConstPtr
wavefrontUnwrap(const UtilityScene::Snapshot& snapshot,
                const WavefrontFrame& frame,
                const std::atomic<bool>& stop,
                std::string* error)
{
    const std::string prim = frame.unwrapPrim.GetAsString();

    if (frame.unwrapPrim.IsEmpty()) {
        *error = "The Utility renderer's uvUnwrap projection needs a UV Prim";

        return WavefrontUnwrapConstPtr();
    }

    const UtilityScene::Geometry* geometry = nullptr;
    UtilityScene::GeometryId id = 0;
    std::size_t geometryIndex = 0;

    for (const auto& entry : snapshot.geometries) {
        if (entry.second.path == frame.unwrapPrim) {
            geometry = &entry.second;
            id = entry.first;
            break;
        }
        ++geometryIndex;
    }

    const auto instance = std::find_if(snapshot.instances.begin(), snapshot.instances.end(), [id](const UtilityScene::Instance& candidate) { return candidate.geometry == id; });

    if (!geometry || instance == snapshot.instances.end()) {
        *error = "The Utility renderer's UV Prim " + prim + " is not a mesh the render draws";

        return WavefrontUnwrapConstPtr();
    }
    if (!geometry->hasUnwrapSt) {
        *error = "The Utility renderer's UV Prim " + prim + " has no UV Primvar \"" + frame.unwrapPrimvar + "\"";

        return WavefrontUnwrapConstPtr();
    }
    if (geometry->unwrapStBinding != UtilityScene::eColorBindingVertex && geometry->unwrapStBinding != UtilityScene::eColorBindingCorner) {
        *error = "The Utility renderer's UV Prim " + prim + " has a constant or uniform UV Primvar \"" + frame.unwrapPrimvar + "\": a bake needs it per vertex or face-varying";

        return WavefrontUnwrapConstPtr();
    }

    const WavefrontRect window = frame.window;
    const WavefrontRect display = displayOf(frame);
    const int tile = std::max(0, frame.udimTile - 1001);
    const GfVec2d tileOrigin(tile % 10, tile / 10);
    const double texelsPerU = std::max(1, display.width);
    const double texelsPerV = std::max(1, display.height);
    const VtVec2fArray& st = geometry->unwrapSt;
    const VtVec3fArray& points = geometry->points;
    const VtVec3iArray& triangles = geometry->triangles;
    const UtilityScene::ColorBinding binding = geometry->unwrapStBinding;
    auto unwrap = std::make_shared<WavefrontUnwrap>();

    unwrap->window = window;
    unwrap->triangles.assign(triangles.size() * kWavefrontUnwrapTriangleFloats, 0.f);

    // A sample lands up to half the filter's width from its pixel's centre.
    const int margin = static_cast<int>(std::ceil(std::max(1.f, frame.sampling.filterWidth) * 0.5f)) + 1;
    const double gridLeft = window.x - margin;
    const double gridBottom = window.y - margin;
    const double gridRight = window.x + std::max(0, window.width) + margin;
    const double gridTop = window.y + std::max(0, window.height) + margin;

    const auto texelOf = [&](const GfVec2f& uv) {
        return GfVec2d(display.x + (uv[0] - tileOrigin[0]) * texelsPerU, display.y + (uv[1] - tileOrigin[1]) * texelsPerV);
    };
    const auto cross2 = [](const GfVec2d& a, const GfVec2d& b) { return a[0] * b[1] - a[1] * b[0]; };

    struct Placed {
        std::uint32_t triangle;
        GfVec4d bounds;
    };
    std::vector<Placed> placed;
    double footprintSum = 0.;
    double areaSum = 0.;

    for (std::size_t t = 0; t < triangles.size(); ++t) {
        if (stop.load(std::memory_order_relaxed)) {
            return WavefrontUnwrapConstPtr();
        }

        const GfVec3i& corners = triangles[t];
        GfVec2d uv[3];
        GfVec3d p[3];
        bool usable = true;

        for (int c = 0; c < 3 && usable; ++c) {
            const std::size_t point = static_cast<std::size_t>(corners[c]);
            const std::size_t coordinate = (binding == UtilityScene::eColorBindingVertex) ? point : t * 3 + c;

            usable = corners[c] >= 0 && point < points.size() && coordinate < st.size();
            if (usable) {
                uv[c] = texelOf(st[coordinate]);
                p[c] = instance->transform.Transform(GfVec3d(points[point]));
            }
        }

        const double area = usable ? cross2(uv[1] - uv[0], uv[2] - uv[0]) : 0.;
        GfVec3d normal = GfCross(p[1] - p[0], p[2] - p[0]);

        if (!usable || std::abs(area) < 1e-12 || normal.Normalize() <= 0.) {
            continue;
        }

        const GfVec4d bounds(std::min({ uv[0][0], uv[1][0], uv[2][0] }), std::min({ uv[0][1], uv[1][1], uv[2][1] }),
                             std::max({ uv[0][0], uv[1][0], uv[2][0] }), std::max({ uv[0][1], uv[1][1], uv[2][1] }));

        if (!(bounds[2] >= gridLeft && bounds[0] < gridRight && bounds[3] >= gridBottom && bounds[1] < gridTop)) {
            continue;
        }

        const GfVec2d ab = uv[1] - uv[0];
        const GfVec2d ac = uv[2] - uv[0];
        const GfVec3d perX = ((p[1] - p[0]) * ac[1] - (p[2] - p[0]) * ab[1]) / area;
        const GfVec3d perY = ((p[2] - p[0]) * ab[0] - (p[1] - p[0]) * ac[0]) / area;
        double magnitude = 0.;

        for (const GfVec3d& corner : p) {
            magnitude = std::max({ magnitude, std::abs(corner[0]), std::abs(corner[1]), std::abs(corner[2]) });
        }

        // Nothing is traced to the surface, so the ray's start need only stay clear of its float error.
        const double reach = 1e-4 * (1. + magnitude);
        float* record = unwrap->triangles.data() + t * kWavefrontUnwrapTriangleFloats;
        const double values[kWavefrontUnwrapTriangleFloats] = {
            uv[0][0],
            uv[0][1],
            uv[1][0],
            uv[1][1],
            uv[2][0],
            uv[2][1],
            reach,
            0.,
            p[0][0],
            p[0][1],
            p[0][2],
            normal[0],
            p[1][0],
            p[1][1],
            p[1][2],
            normal[1],
            p[2][0],
            p[2][1],
            p[2][2],
            normal[2],
        };

        for (std::size_t v = 0; v < kWavefrontUnwrapTriangleFloats; ++v) {
            record[v] = static_cast<float>(values[v]);
        }
        placed.push_back(Placed { static_cast<std::uint32_t>(t), bounds });
        footprintSum += std::sqrt(GfCross(perX, perY).GetLength()) * std::abs(area);
        areaSum += std::abs(area);
    }

    // About a triangle a cell, so that a sample tests few candidates whether the mesh is coarse or dense.
    const int cellSize = placed.empty() ? 1 : static_cast<int>(std::clamp(std::round(std::sqrt(areaSum / placed.size())), 1., 16.));
    const int columns = static_cast<int>((gridRight - gridLeft + cellSize - 1) / cellSize);
    const int rows = static_cast<int>((gridTop - gridBottom + cellSize - 1) / cellSize);
    const std::size_t cells = static_cast<std::size_t>(columns) * rows;
    const std::size_t offsets = eWavefrontUnwrapGridHeader;
    const auto cellRange = [&](double low, double high, double origin, int count) {
        const int first = static_cast<int>(std::floor((low - origin) / cellSize));
        const int last = static_cast<int>(std::floor((high - origin) / cellSize));

        return std::make_pair(std::max(0, first), std::min(count - 1, last));
    };
    std::vector<std::int32_t>& grid = unwrap->grid;
    std::size_t candidates = 0;

    grid.assign(offsets + cells + 1, 0);
    grid[eWavefrontUnwrapGridX] = static_cast<std::int32_t>(gridLeft);
    grid[eWavefrontUnwrapGridY] = static_cast<std::int32_t>(gridBottom);
    grid[eWavefrontUnwrapGridColumns] = columns;
    grid[eWavefrontUnwrapGridRows] = rows;
    grid[eWavefrontUnwrapGridCellSize] = cellSize;
    grid[eWavefrontUnwrapGridGeometry] = static_cast<std::int32_t>(geometryIndex);
    grid[eWavefrontUnwrapGridInstance] = static_cast<std::int32_t>(std::distance(snapshot.instances.begin(), instance));

    // Counts each cell's candidates into its offset, so that the running sum leaves every offset at its
    // cell's end, and filling from the last triangle back walks it down to the start in triangle order.
    for (const Placed& triangle : placed) {
        const std::pair<int, int> across = cellRange(triangle.bounds[0], triangle.bounds[2], gridLeft, columns);
        const std::pair<int, int> up = cellRange(triangle.bounds[1], triangle.bounds[3], gridBottom, rows);

        for (int row = up.first; row <= up.second; ++row) {
            if (stop.load(std::memory_order_relaxed)) {
                return WavefrontUnwrapConstPtr();
            }
            for (int column = across.first; column <= across.second; ++column) {
                ++grid[offsets + static_cast<std::size_t>(row) * columns + column];
            }
            candidates += static_cast<std::size_t>(across.second - across.first + 1);
        }
    }
    if (offsets + cells + 1 + candidates > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        *error = "The Utility renderer's UV Prim " + prim + " lays " + std::to_string(candidates) + " triangle entries over the bake's " + std::to_string(cells) + " cells, more than a bake can index";

        return WavefrontUnwrapConstPtr();
    }
    for (std::size_t cell = 1; cell < cells; ++cell) {
        if (cell % static_cast<std::size_t>(columns) == 0 && stop.load(std::memory_order_relaxed)) {
            return WavefrontUnwrapConstPtr();
        }
        grid[offsets + cell] += grid[offsets + cell - 1];
    }
    grid[offsets + cells] = static_cast<std::int32_t>(candidates);
    grid.resize(offsets + cells + 1 + candidates);

    const std::size_t listed = offsets + cells + 1;

    for (auto triangle = placed.rbegin(); triangle != placed.rend(); ++triangle) {
        const std::pair<int, int> across = cellRange(triangle->bounds[0], triangle->bounds[2], gridLeft, columns);
        const std::pair<int, int> up = cellRange(triangle->bounds[1], triangle->bounds[3], gridBottom, rows);

        for (int row = up.first; row <= up.second; ++row) {
            if (stop.load(std::memory_order_relaxed)) {
                return WavefrontUnwrapConstPtr();
            }
            for (int column = across.first; column <= across.second; ++column) {
                std::int32_t& offset = grid[offsets + static_cast<std::size_t>(row) * columns + column];

                grid[listed + static_cast<std::size_t>(--offset)] = static_cast<std::int32_t>(triangle->triangle);
            }
        }
    }
    unwrap->footprint = areaSum > 0. ? footprintSum / areaSum : 0.;

    return unwrap;
}

void
wavefrontRequireUnwrap(const WavefrontFrame& frame)
{
    if (frame.mapping == WavefrontMapping::UvUnwrap && !frame.unwrap) {
        throw std::runtime_error("The Utility renderer was asked for a UV Unwrap frame of " + frame.unwrapPrim.GetString() + " without its unwrap");
    }
}

std::vector<float>
wavefrontCameraSteps(const WavefrontFrame& frame,
                     const WavefrontMotion& motion)
{
    std::vector<float> rows;

    if (!motion.isOpen() || motion.cameraTimes.empty() || frame.mapping == WavefrontMapping::UvUnwrap) {
        return rows;
    }
    const int steps = std::max(1, motion.segments) + 1;
    std::vector<WavefrontFrame> ends;

    ends.reserve(static_cast<std::size_t>(steps));
    for (int k = 0; k < steps; ++k) {
        ends.push_back(wavefrontFrameAt(frame, motion, motion.open + (motion.close - motion.open) * k / (steps - 1)));
    }
    const bool still = std::all_of(ends.cbegin(), ends.cend(), [&](const WavefrontFrame& end) {
        return end.worldToView == ends.front().worldToView && end.projection == ends.front().projection;
    });

    if (still) {
        return rows;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(steps);

    rows.reserve(4 * (1 + static_cast<std::size_t>(steps) * kKernelCameraRowsPerStep));
    rows.push_back(std::bit_cast<float>(count));
    rows.insert(rows.end(), 3, 0.f);

    GfVec4d previous(1., 0., 0., 0.);

    for (WavefrontFrame& end : ends) {
        const GfMatrix4d viewToWorld = end.worldToView.GetInverse();
        // The frustum in the camera's own space, which its view-to-world transform then places.
        end.worldToView = GfMatrix4d(1.);

        const KernelRayGenParams local = makeRayGenParams(end, end.window);
        UtilityScene::Decomposition place = UtilityScene::decompose(viewToWorld);

        // Each end turns the short way from the one before it.
        if (GfDot(place.rotation, previous) < 0.) {
            place.rotation = -place.rotation;
        }
        previous = place.rotation;
        appendPosition(local.nearAtCentre, &rows);
        appendPosition(local.nearPerX, &rows);
        appendPosition(local.nearPerY, &rows);
        appendPosition(local.farAtCentre, &rows);
        appendPosition(local.farPerX, &rows);
        appendPosition(local.farPerY, &rows);
        appendMotionStep(place, &rows);
    }

    return rows;
}

bool
wavefrontDrawSamples(WavefrontExecutor& executor,
                     const UtilityScene::Snapshot& snapshot,
                     const WavefrontFrame& frame,
                     const WavefrontMotion& motion,
                     WavefrontFilm& film,
                     const std::atomic<bool>& stop,
                     int threads,
                     const std::function<void(std::size_t)>& progress)
{
    const bool unwrapping = frame.mapping == WavefrontMapping::UvUnwrap;
    const bool blurred = !unwrapping && motion.isOpen() && UtilityScene::moves(snapshot);
    UtilityScene::Snapshot shuttered;

    if (blurred) {
        const utilityProfile::Scope timed(utilityProfile::Stage::ShutterSnapshot);

        if (!UtilityScene::atShutter(snapshot, motion.open, motion.close, std::max(1, motion.segments), stop, &shuttered)) {
            return false;
        }
    }
    const UtilityScene::Snapshot& scene = blurred ? shuttered : snapshot;
    WavefrontFrame drawn = frame;

    if (unwrapping) {
        std::string error;
        WavefrontUnwrapConstPtr unwrap = wavefrontUnwrap(scene, drawn, stop, &error);

        if (!error.empty()) {
            throw std::runtime_error(error);
        }
        if (!unwrap) {
            return false;
        }
        drawn.unwrap = unwrap;
    } else if (frame.raster) {
        const int passes = std::max(1, frame.rasterSamples);

        for (int i = 0; i < passes; ++i) {
            const utilityProfile::Pass timedPass;
            WavefrontRasterPass pass = wavefrontRasterPass(frame.sampling, i, passes);

            if (!motion.isOpen()) {
                pass.time = 0.5f;
            }

            WavefrontFrame atPass = motion.isOpen() ? wavefrontFrameAt(frame, motion, motion.open + pass.time * (motion.close - motion.open)) : frame;

            atPass.rasterPass = pass;
            atPass.cameraSteps.clear();
            if (!executor.render(scene, atPass, film, stop, threads)) {
                return false;
            }
            {
                const utilityProfile::Scope timed(utilityProfile::Stage::PublishPass);

                film.passDrawn();
            }
            if (progress) {
                progress(static_cast<std::size_t>(i) + 1);
            }
        }

        return true;
    } else {
        drawn.cameraSteps = wavefrontCameraSteps(frame, motion);
    }

    const int samples = std::max(1, frame.sampling.samplesPerPixel());

    for (int i = 0; i < samples; ++i) {
        const utilityProfile::Pass timedPass;

        drawn.sampling.sampleIndex = i;
        if (!executor.render(scene, drawn, film, stop, threads)) {
            return false;
        }
        {
            const utilityProfile::Scope timed(utilityProfile::Stage::PublishPass);

            film.passDrawn();
        }
        if (progress) {
            progress(static_cast<std::size_t>(i) + 1);
        }
    }

    return true;
}

NATRON_NAMESPACE_EXIT
