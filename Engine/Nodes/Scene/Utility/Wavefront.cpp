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
#include <set>

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3i.h>

#include "Engine/Nodes/Scene/Utility/UtilityLight.h"
#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
const double kNdcNear = -1.;
const double kNdcFar = 1.;
const float kFallbackGrey = 0.18f;
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
    destination[3] = 0.f;
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
            bool udim)
{
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
    binding.image[2] = ref.wrapS;
    binding.image[3] = ref.wrapT;
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

/**
 * @brief Appends to data the dome's importance table for image: a conditional CDF per row, then the
 * CDF over rows, each texel weighted by its luminance and by the solid angle its row covers. Returns
 * false, appending nothing, when the image has no light in it.
 **/
bool
appendDomeCdf(const UtilityTextureImage& image,
              std::vector<float>* data,
              std::uint32_t* width,
              std::uint32_t* height)
{
    if (image.width <= 0 || image.height <= 0 || image.texels.size() < static_cast<std::size_t>(image.width) * image.height) {
        return false;
    }

    const int step = std::max({ 1, (image.width + kDomeCdfMaxWidth - 1) / kDomeCdfMaxWidth, (image.height + kDomeCdfMaxHeight - 1) / kDomeCdfMaxHeight });
    const int w = std::max(1, image.width / step);
    const int h = std::max(1, image.height / step);
    std::vector<double> rows(h, 0.);
    std::vector<float> table(static_cast<std::size_t>(w) * h + h);

    for (int y = 0; y < h; ++y) {
        const double cosLat = std::cos(((y + 0.5) / h - 0.5) * M_PI);
        double sum = 0.;
        float* conditional = table.data() + static_cast<std::size_t>(y) * w;

        for (int x = 0; x < w; ++x) {
            double luminance = 0.;

            for (int dy = 0; dy < step; ++dy) {
                for (int dx = 0; dx < step; ++dx) {
                    const GfVec4f& t = image.texels[static_cast<std::size_t>(y * step + dy) * image.width + x * step + dx];

                    luminance += 0.2126 * t[0] + 0.7152 * t[1] + 0.0722 * t[2];
                }
            }
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

    for (int plane = 0; plane < 2; ++plane) {
        const double ndcZ = plane == 0 ? kNdcNear : kNdcFar;
        const GfVec3d centre = toWorld.Transform(GfVec3d(0., 0., ndcZ));
        const double perX = (toWorld.Transform(GfVec3d(1., 0., ndcZ)) - centre).GetLength() * 2. / width;
        const double perY = (toWorld.Transform(GfVec3d(0., 1., ndcZ)) - centre).GetLength() * 2. / height;

        params.pixelFootprint[plane] = static_cast<float>(std::sqrt(perX * perY));
    }
    params.pixelFootprint[2] = 0.f;
    params.pixelFootprint[3] = 0.f;

    return params;
}

void
WavefrontShadeTables::updateMaterials(const UtilityScene::Snapshot& snapshot)
{
    std::vector<UtilityTextureAssetConstPtr> assets;
    std::map<const UtilityTextureAsset*, int> firstImages;
    std::set<UtilityTexturePool::Key> keys;
    int imageCount = 0;

    _materials.clear();
    _textureBindings.clear();
    _projections.clear();
    _materialIndices.clear();

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
            } else {
                image = found->second;
            }
        }
        _textureBindings.push_back(makeBinding(ref, image, asset && asset->udim));

        return asset;
    };
    const auto addMaterial = [&](const UtilityMaterialDesc& desc) {
        KernelMaterialRecord record {};
        const int firstBinding = static_cast<int>(_textureBindings.size());

        for (const UtilityTextureRef& ref : desc.textureRefs) {
            addBinding(ref);
        }

        store(desc.diffuseColor, 0.f, record.diffuseColor);
        store(desc.emissiveColor, 0.f, record.emissiveColor);
        store(desc.specularColor, desc.useSpecularWorkflow ? 1.f : 0.f, record.specularColor);
        store(GfVec4f(desc.metallic, desc.roughness, desc.clearcoat, desc.clearcoatRoughness), record.surface);
        store(GfVec4f(desc.opacity, desc.opacityThreshold, desc.ior, 0.f), record.coverage);
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
                if (appendDomeCdf(*asset->tiles[0], &_lightData, &record.data[1], &record.data[2])) {
                    record.kind[1] |= kKernelLightDomeImportance;
                }
            }
        }
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

    _images.clear();
    _texels.clear();
    for (const UtilityTextureAssetConstPtr& asset : assets) {
        for (const UtilityTextureImageConstPtr& tile : asset->tiles) {
            if (!tile) {
                _images.insert(_images.end(), { 0u, 0u, 0u, 0u });
                continue;
            }
            _images.insert(_images.end(), { static_cast<std::uint32_t>(tile->width), static_cast<std::uint32_t>(tile->height), static_cast<std::uint32_t>(tile->levels), static_cast<std::uint32_t>(_texels.size()) });
            _texels.insert(_texels.end(), tile->texels.begin(), tile->texels.end());
        }
    }
    _textureAssets.swap(assets);
    ++_texelVersion;
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

        sources.push_back(SourceVersion { entry.first, geometry.colorVersion, geometry.topologyVersion, geometry.pointsVersion, geometry.stVersion, geometry.points.size() });
    }

    const bool recopy = !_synced || sources != _sources;

    _synced = false;
    updateMaterials(snapshot);
    if (recopy) {
        _colors.clear();
        _triangles.clear();
        _points.clear();
        _sts.clear();
    }
    _geometries.clear();
    _geometries.reserve(snapshot.geometries.size());

    std::uint32_t colorOffset = 0;
    std::uint32_t triangleOffset = 0;
    std::uint32_t pointOffset = 0;
    std::uint32_t stOffset = 0;

    for (const auto& entry : snapshot.geometries) {
        if (stop && stop->load(std::memory_order_relaxed)) {
            return false;
        }

        const UtilityScene::Geometry& geometry = entry.second;
        const bool fits = valuesFitBinding(geometry.colorBinding, geometry.colors.size(), geometry.triangles);
        const bool stFits = geometry.hasSt && valuesFitBinding(geometry.stBinding, geometry.st.size(), geometry.triangles);
        const std::uint32_t colorCount = fits ? static_cast<std::uint32_t>(geometry.colors.size()) : 1;
        const std::uint32_t stCount = stFits ? static_cast<std::uint32_t>(geometry.st.size()) : 0;
        const std::uint32_t triangleCount = static_cast<std::uint32_t>(geometry.triangles.size());
        const std::uint32_t pointCount = static_cast<std::uint32_t>(geometry.points.size());
        const auto material = geometry.materialId.IsEmpty() ? _materialIndices.end() : _materialIndices.find(geometry.materialId);
        const KernelGeometryRecord record {
            colorOffset,
            static_cast<std::uint32_t>(fits ? geometry.colorBinding : UtilityScene::eColorBindingConstant),
            geometry.primId,
            triangleOffset,
            material == _materialIndices.end() ? 0u : material->second,
            stOffset,
            stFits ? static_cast<std::uint32_t>(geometry.stBinding) : kKernelStBindingNone,
            pointOffset,
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
            for (const GfVec3f& point : geometry.points) {
                _points.push_back(GfVec4f(point[0], point[1], point[2], 1.f));
            }
            if (stFits) {
                _sts.insert(_sts.end(), geometry.st.cbegin(), geometry.st.cend());
            }
        }
        colorOffset += colorCount;
        triangleOffset += triangleCount * 3;
        pointOffset += pointCount;
        stOffset += stCount;
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
            return category.IsEmpty() || std::find(categories.begin(), categories.end(), category) != categories.end();
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

NATRON_NAMESPACE_EXIT
