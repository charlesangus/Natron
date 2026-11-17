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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numeric>
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

    return params;
}

void
WavefrontShadeTables::updateMaterials(const UtilityScene::Snapshot& snapshot)
{
    std::vector<UtilityTextureAssetConstPtr> assets;
    std::vector<std::string> assetNames;
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
            asset = snapshot.textures->acquire(ref.asset, ref.colorSpace);
        }
        int image = -1;

        if (asset) {
            const auto found = firstImages.find(asset.get());

            if (found == firstImages.end()) {
                image = imageCount;
                firstImages[asset.get()] = image;
                imageCount += static_cast<int>(asset->tiles.size());
                assets.push_back(asset);
                assetNames.push_back(ref.asset);
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
            | (desc.emissiveFromDisplayColor ? kKernelMaterialEmissiveFromDisplayColor : 0u);
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

    addMaterial(UtilityMaterialDesc::fallback());
    for (const auto& entry : snapshot.materials) {
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

    if (snapshot.textures) {
        snapshot.textures->retainOnly(keys);
    }
    if (assets == _textureAssets && _texelVersion != 0) {
        return;
    }

    _textureAssets.swap(assets);
    _textureAssetNames.swap(assetNames);
    buildTexels();
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
    updateMaterials(snapshot);
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
            pointTotal += geometry.points.size();
            stTotal += geometry.st.size();
            normalTotal += geometry.normals.size();
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
        };

        _geometries.push_back(record);
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

            const GfVec3f* points = geometry.points.cdata();

            for (std::size_t first = 0; first < geometry.points.size(); first += kCopyChunk) {
                if (stop && stop->load(std::memory_order_relaxed)) {
                    return false;
                }

                const std::size_t last = std::min(first + kCopyChunk, geometry.points.size());

                for (std::size_t p = first; p < last; ++p) {
                    _points.push_back(GfVec4f(points[p][0], points[p][1], points[p][2], 1.f));
                }
            }
            if (stFits) {
                _sts.insert(_sts.end(), geometry.st.cbegin(), geometry.st.cend());
            }

            const GfVec3f* normals = normalsFit ? geometry.normals.cdata() : nullptr;

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
        colorOffset += colorCount;
        triangleOffset += triangleCount * 3;
        pointOffset += pointCount;
        stOffset += stCount;
        normalOffset += normalCount;
    }

    _instanceIds.clear();
    _instanceIds.reserve(snapshot.instances.size());
    _instanceTransforms.clear();
    _instanceTransforms.reserve(snapshot.instances.size() * 8);
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

namespace {
// The texels of a mip chain from level `first` on, and that level's size.
std::size_t
texelsFrom(const UtilityTextureImage& image,
           int first,
           std::size_t* offset,
           int* width,
           int* height)
{
    int w = image.width;
    int h = image.height;

    *offset = 0;
    for (int level = 0; level < first; ++level) {
        *offset += static_cast<std::size_t>(w) * h;
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    *width = w;
    *height = h;

    return image.texels.size() - *offset;
}
} // anon namespace

bool
WavefrontShadeTables::fitTexels(std::uint64_t limitBytes)
{
    if (limitBytes != _texelLimit) {
        _texelLimit = limitBytes;
        buildTexels();
    }

    return _texelsFit;
}

const std::vector<std::string>&
WavefrontShadeTables::texelWarnings() const
{
    return _texelWarnings;
}

// The texels go to the device as one buffer, so past its limit the largest images give up their
// finest levels, already in their mip chains, until the whole set fits.
void
WavefrontShadeTables::buildTexels()
{
    struct Tile {
        const UtilityTextureImage* image;
        std::size_t asset;
        int dropped;
    };
    std::vector<Tile> tiles;
    std::size_t total = 0;

    for (std::size_t a = 0; a < _textureAssets.size(); ++a) {
        for (const UtilityTextureImageConstPtr& tile : _textureAssets[a]->tiles) {
            tiles.push_back(Tile { tile.get(), a, 0 });
            total += tile ? tile->texels.size() : 0;
        }
    }

    const std::uint64_t limitTexels = _texelLimit / sizeof(GfVec4f);
    const auto finest = [](const Tile& t) -> std::size_t {
        if (!t.image) {
            return 0;
        }
        std::size_t offset = 0;
        int width = 0;
        int height = 0;

        texelsFrom(*t.image, t.dropped, &offset, &width, &height);

        return static_cast<std::size_t>(width) * height;
    };
    const auto less = [&](std::size_t a, std::size_t b) { return finest(tiles[a]) < finest(tiles[b]); };
    std::vector<std::size_t> heap;

    for (std::size_t t = 0; t < tiles.size(); ++t) {
        if (tiles[t].image && tiles[t].image->levels > 1) {
            heap.push_back(t);
        }
    }

    std::make_heap(heap.begin(), heap.end(), less);
    _texelsFit = true;
    while (_texelLimit != 0 && total > limitTexels) {
        if (heap.empty()) {
            _texelsFit = false;
            break;
        }
        std::pop_heap(heap.begin(), heap.end(), less);
        Tile& tile = tiles[heap.back()];

        total -= finest(tile);
        ++tile.dropped;
        if (tile.dropped + 1 < tile.image->levels) {
            std::push_heap(heap.begin(), heap.end(), less);
        } else {
            heap.pop_back();
        }
    }

    std::set<std::size_t> downsampled;

    _images.clear();
    _texels.clear();
    _texelWarnings.clear();
    if (_texelsFit) {
        _texels.reserve(total);
    }
    for (const Tile& tile : tiles) {
        if (!tile.image) {
            _images.insert(_images.end(), { 0u, 0u, 0u, 0u });
            continue;
        }
        std::size_t offset = 0;
        int width = 0;
        int height = 0;
        const std::size_t count = texelsFrom(*tile.image, tile.dropped, &offset, &width, &height);

        _images.insert(_images.end(), { static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), static_cast<std::uint32_t>(tile.image->levels - tile.dropped), static_cast<std::uint32_t>(_texels.size()) });
        _texels.insert(_texels.end(), tile.image->texels.begin() + static_cast<std::ptrdiff_t>(offset), tile.image->texels.begin() + static_cast<std::ptrdiff_t>(offset + count));
        if (tile.dropped > 0 && downsampled.insert(tile.asset).second) {
            _texelWarnings.push_back(TfStringPrintf("Texture %s was downsampled from %dx%d to %dx%d to fit the GPU's largest storage buffer",
                                                    _textureAssetNames[tile.asset].c_str(), tile.image->width, tile.image->height, width, height));
        }
    }
    if (!_texelsFit) {
        _texelWarnings.push_back(TfStringPrintf("The textures need %zu bytes even at one texel each, more than the GPU's largest storage buffer of %llu bytes",
                                                static_cast<std::size_t>(total * sizeof(GfVec4f)), static_cast<unsigned long long>(_texelLimit)));
    }
    ++_texelVersion;
}

bool
WavefrontShadeTables::hasBackground() const
{
    return _background;
}

bool
WavefrontShadeTables::hasHoles() const
{
    return _holes;
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

int
wavefrontBucketCount(const WavefrontMotion& motion,
                     const UtilityScene::Snapshot& snapshot,
                     int samplesPerPixel)
{
    if (!motion.isOpen() || (motion.cameraTimes.empty() && !UtilityScene::moves(snapshot))) {
        return 1;
    }

    return std::clamp(motion.segments, 1, std::max(1, samplesPerPixel));
}

double
wavefrontBucketTime(const WavefrontMotion& motion,
                    int bucket,
                    int buckets)
{
    // One bucket draws the unblurred frame, which an offset shutter's middle would move.
    if (!motion.isOpen() || buckets <= 1) {
        return 0.;
    }

    return motion.open + (motion.close - motion.open) * (bucket + 0.5) / buckets;
}

std::vector<int>
wavefrontSampleOrder(int samplesPerPixel,
                     int buckets)
{
    const int count = std::max(1, samplesPerPixel);
    std::vector<int> order(static_cast<std::size_t>(count));

    if (buckets <= 1) {
        std::iota(order.begin(), order.end(), 0);

        return order;
    }
    // A stride coprime with the count visits every sample once. Index s is cell (s % side, s / side)
    // of the pixel's grid, so a stride of rows * side + columns steps consecutive samples that many
    // columns and rows on; golden fractions of the side for both spread a bucket over both axes.
    const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(count))));
    int stride = 1;

    if (side > 1 && side * side == count) {
        int columns = std::max(1, static_cast<int>(std::lround(side * 0.6180339887)));

        while (std::gcd(columns, side) != 1) {
            ++columns;
        }
        stride = (static_cast<int>(std::lround(side * 0.3819660113)) * side + columns) % count;
    } else {
        stride = std::max(1, static_cast<int>(std::lround(count * 0.6180339887)));
        while (std::gcd(stride, count) != 1) {
            ++stride;
        }
    }
    for (int i = 0; i < count; ++i) {
        order[static_cast<std::size_t>(i)] = static_cast<int>((static_cast<long long>(i) * stride) % count);
    }

    return order;
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

    for (const auto& entry : snapshot.geometries) {
        if (entry.second.path == frame.unwrapPrim) {
            geometry = &entry.second;
            id = entry.first;
            break;
        }
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
    double footprintSum = 0.;
    std::size_t covered = 0;

    unwrap->window = window;
    unwrap->texels.assign(static_cast<std::size_t>(std::max(0, window.width)) * std::max(0, window.height) * kWavefrontUnwrapFloats, 0.f);

    const auto texelOf = [&](const GfVec2f& uv) {
        return GfVec2d(display.x + (uv[0] - tileOrigin[0]) * texelsPerU, display.y + (uv[1] - tileOrigin[1]) * texelsPerV);
    };
    const auto cross2 = [](const GfVec2d& a, const GfVec2d& b) { return a[0] * b[1] - a[1] * b[0]; };

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

        const GfVec2d ab = uv[1] - uv[0];
        const GfVec2d ac = uv[2] - uv[0];
        const GfVec3d perX = ((p[1] - p[0]) * ac[1] - (p[2] - p[0]) * ab[1]) / area;
        const GfVec3d perY = ((p[2] - p[0]) * ab[0] - (p[1] - p[0]) * ac[0]) / area;
        const double size = std::sqrt(GfCross(perX, perY).GetLength());
        const int left = std::max(window.x, static_cast<int>(std::floor(std::min({ uv[0][0], uv[1][0], uv[2][0] }) - 0.5)));
        const int right = std::min(window.x + window.width - 1, static_cast<int>(std::ceil(std::max({ uv[0][0], uv[1][0], uv[2][0] }) - 0.5)));
        const int bottom = std::max(window.y, static_cast<int>(std::floor(std::min({ uv[0][1], uv[1][1], uv[2][1] }) - 0.5)));
        const int top = std::min(window.y + window.height - 1, static_cast<int>(std::ceil(std::max({ uv[0][1], uv[1][1], uv[2][1] }) - 0.5)));
        const double tolerance = 1e-9;

        for (int y = bottom; y <= top; ++y) {
            for (int x = left; x <= right; ++x) {
                const GfVec2d centre(x + 0.5, y + 0.5);
                const double towardB = cross2(centre - uv[0], ac) / area;
                const double towardC = cross2(ab, centre - uv[0]) / area;

                if (towardB < -tolerance || towardC < -tolerance || towardB + towardC > 1. + tolerance) {
                    continue;
                }

                const GfVec3d position = p[0] + (p[1] - p[0]) * towardB + (p[2] - p[0]) * towardC;
                const double magnitude = std::max({ std::abs(position[0]), std::abs(position[1]), std::abs(position[2]) });
                // Far enough to clear the surface's float error, near enough to hit nothing else.
                const double reach = 2. * std::max(perX.GetLength(), perY.GetLength()) + 1e-4 * (1. + magnitude);
                float* texel = unwrap->texels.data() + (static_cast<std::size_t>(y - window.y) * window.width + (x - window.x)) * kWavefrontUnwrapFloats;
                const float values[kWavefrontUnwrapFloats] = {
                    static_cast<float>(position[0]),
                    static_cast<float>(position[1]),
                    static_cast<float>(position[2]),
                    1.f,
                    static_cast<float>(perX[0]),
                    static_cast<float>(perX[1]),
                    static_cast<float>(perX[2]),
                    static_cast<float>(reach),
                    static_cast<float>(perY[0]),
                    static_cast<float>(perY[1]),
                    static_cast<float>(perY[2]),
                    0.f,
                    static_cast<float>(normal[0]),
                    static_cast<float>(normal[1]),
                    static_cast<float>(normal[2]),
                    0.f,
                };

                std::copy(values, values + kWavefrontUnwrapFloats, texel);
                footprintSum += size;
                ++covered;
            }
        }
    }
    unwrap->footprint = covered ? footprintSum / covered : 0.;

    return unwrap;
}

bool
wavefrontDrawSamples(WavefrontExecutor& executor,
                     const UtilityScene::Snapshot& snapshot,
                     const WavefrontFrame& frame,
                     const WavefrontMotion& motion,
                     WavefrontFilm& film,
                     const std::atomic<bool>& stop,
                     int threads,
                     const std::function<void(std::size_t)>& progress,
                     std::vector<double>* bucketSeconds)
{
    const int samples = std::max(1, frame.sampling.samplesPerPixel());
    const int buckets = wavefrontBucketCount(motion, snapshot, samples);
    const std::vector<int> order = wavefrontSampleOrder(samples, buckets);
    std::size_t done = 0;

    if (bucketSeconds) {
        bucketSeconds->assign(static_cast<std::size_t>(buckets), 0.);
    }
    for (int bucket = 0; bucket < buckets; ++bucket) {
        const auto start = std::chrono::steady_clock::now();
        const int first = static_cast<int>(static_cast<long long>(bucket) * samples / buckets);
        const int last = static_cast<int>(static_cast<long long>(bucket + 1) * samples / buckets);
        const double time = wavefrontBucketTime(motion, bucket, buckets);
        UtilityScene::Snapshot evaluated;
        const bool evaluate = time != 0. && UtilityScene::moves(snapshot);

        if (evaluate) {
            evaluated = UtilityScene::evaluateAt(snapshot, time);
        }
        const UtilityScene::Snapshot& scene = evaluate ? evaluated : snapshot;
        WavefrontFrame atTime = (time != 0.) ? wavefrontFrameAt(frame, motion, time) : frame;

        if (atTime.mapping == WavefrontMapping::UvUnwrap) {
            std::string error;

            atTime.unwrap = wavefrontUnwrap(scene, atTime, stop, &error);
            if (!error.empty()) {
                throw std::runtime_error(error);
            }
            if (!atTime.unwrap) {
                return false;
            }
        }

        for (int i = first; i < last; ++i) {
            WavefrontFrame pass = atTime;

            pass.sampling.sampleIndex = order[static_cast<std::size_t>(i)];
            if (!executor.render(scene, pass, film, stop, threads)) {
                return false;
            }
            film.passDrawn();
            ++done;
            if (progress) {
                progress(done);
            }
        }
        if (bucketSeconds) {
            (*bucketSeconds)[static_cast<std::size_t>(bucket)] = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
    }

    return true;
}

NATRON_NAMESPACE_EXIT
