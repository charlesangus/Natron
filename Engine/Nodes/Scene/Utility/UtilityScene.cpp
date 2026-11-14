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

#include "UtilityScene.h"

#include <algorithm>
#include <set>

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/stringUtils.h>

#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"
#include "Engine/Nodes/Scene/Utility/UtilityTextures.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

UtilityScene::UtilityScene()
    : _textures(std::make_shared<UtilityTexturePool>())
{
}

UtilityScene::~UtilityScene()
{
}

UtilityScene::GeometryId
UtilityScene::addGeometry(const SdfPath& path,
                          int primId)
{
    std::lock_guard<std::mutex> l(_mutex);
    const GeometryId id = _nextId++;
    Record& record = _records[id];

    record.geometry.path = path;
    record.geometry.primId = primId;
    ++_version;

    return id;
}

void
UtilityScene::removeGeometry(GeometryId id)
{
    std::lock_guard<std::mutex> l(_mutex);

    if (_records.erase(id)) {
        ++_version;
    }
}

void
UtilityScene::setPrimId(GeometryId id,
                        int primId)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (TF_VERIFY(it != _records.end()) && it->second.geometry.primId != primId) {
        it->second.geometry.primId = primId;
        ++_version;
    }
}

void
UtilityScene::setTopology(GeometryId id,
                          const VtVec3fArray& points,
                          const VtVec3iArray& triangles)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (!TF_VERIFY(it != _records.end())) {
        return;
    }
    Geometry& geometry = it->second.geometry;

    geometry.points = points;
    geometry.triangles = triangles;
    ++geometry.topologyVersion;
    ++geometry.pointsVersion;
    ++_topologyBuilds;
    ++_version;
}

void
UtilityScene::setPoints(GeometryId id,
                        const VtVec3fArray& points)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (!TF_VERIFY(it != _records.end())) {
        return;
    }
    it->second.geometry.points = points;
    ++it->second.geometry.pointsVersion;
    ++_pointUpdates;
    ++_version;
}

void
UtilityScene::setColors(GeometryId id,
                        ColorBinding binding,
                        const VtVec4fArray& colors)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (!TF_VERIFY(it != _records.end())) {
        return;
    }
    Geometry& geometry = it->second.geometry;

    if (geometry.colorBinding == binding && geometry.colors == colors) {
        return;
    }
    geometry.colorBinding = binding;
    geometry.colors = colors;
    ++geometry.colorVersion;
    ++_colorUpdates;
    ++_version;
}

void
UtilityScene::setInstances(GeometryId id,
                           const std::vector<GfMatrix4d>& transforms)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (TF_VERIFY(it != _records.end()) && it->second.transforms != transforms) {
        it->second.transforms = transforms;
        ++_version;
    }
}

void
UtilityScene::setMaterialId(GeometryId id,
                            const SdfPath& materialId)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (TF_VERIFY(it != _records.end()) && it->second.geometry.materialId != materialId) {
        it->second.geometry.materialId = materialId;
        ++_version;
    }
}

void
UtilityScene::setSt(GeometryId id,
                    bool hasSt,
                    ColorBinding binding,
                    const VtVec2fArray& st)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (!TF_VERIFY(it != _records.end())) {
        return;
    }
    Geometry& geometry = it->second.geometry;

    if (geometry.hasSt == hasSt && geometry.stBinding == binding && geometry.st == st) {
        return;
    }
    geometry.hasSt = hasSt;
    geometry.stBinding = binding;
    geometry.st = st;
    ++geometry.stVersion;
    ++_version;
}

void
UtilityScene::setMaterial(const SdfPath& path,
                          const std::shared_ptr<const UtilityMaterialDesc>& material)
{
    std::lock_guard<std::mutex> l(_mutex);

    _materials[path] = material;
    ++_version;
}

void
UtilityScene::removeMaterial(const SdfPath& path)
{
    std::lock_guard<std::mutex> l(_mutex);

    if (_materials.erase(path)) {
        ++_version;
    }
}

void
UtilityScene::setRenderTag(GeometryId id,
                           const TfToken& tag)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (TF_VERIFY(it != _records.end()) && it->second.renderTag != tag) {
        it->second.renderTag = tag;
        ++_version;
    }
}

void
UtilityScene::setRenderTags(const TfTokenVector& tags)
{
    std::lock_guard<std::mutex> l(_mutex);

    if (_renderTags != tags) {
        _renderTags = tags;
        ++_version;
    }
}

bool
UtilityScene::isIncluded(const Record& record) const
{
    return _renderTags.empty() || std::find(_renderTags.begin(), _renderTags.end(), record.renderTag) != _renderTags.end();
}

std::uint64_t
UtilityScene::version() const
{
    std::lock_guard<std::mutex> l(_mutex);

    return _version;
}

UtilityScene::Stats
UtilityScene::stats() const
{
    std::lock_guard<std::mutex> l(_mutex);
    Stats stats;

    stats.geometryCount = _records.size();
    for (const auto& entry : _records) {
        if (isIncluded(entry.second)) {
            stats.instanceCount += entry.second.transforms.size();
        }
        stats.triangleCount += entry.second.geometry.triangles.size();
    }
    stats.topologyBuilds = _topologyBuilds;
    stats.pointUpdates = _pointUpdates;
    stats.colorUpdates = _colorUpdates;
    stats.version = _version;

    return stats;
}

UtilityScene::Snapshot
UtilityScene::snapshot() const
{
    std::lock_guard<std::mutex> l(_mutex);
    Snapshot snapshot;

    snapshot.version = _version;
    snapshot.materials = _materials;
    snapshot.textures = _textures;
    for (const auto& entry : _records) {
        snapshot.geometries.emplace_hint(snapshot.geometries.end(), entry.first, entry.second.geometry);
        if (!isIncluded(entry.second)) {
            continue;
        }
        for (std::size_t i = 0; i < entry.second.transforms.size(); ++i) {
            Instance instance;

            instance.geometry = entry.first;
            instance.instanceIndex = static_cast<int>(i);
            instance.transform = entry.second.transforms[i];
            snapshot.instances.push_back(instance);
        }
    }

    return snapshot;
}

const std::shared_ptr<UtilityTexturePool>&
UtilityScene::textures() const
{
    return _textures;
}

std::vector<std::string>
UtilityScene::warnings() const
{
    std::set<std::string> unsupported;
    std::set<std::string> withoutSt;

    {
        std::lock_guard<std::mutex> l(_mutex);

        for (const auto& entry : _records) {
            const Geometry& geometry = entry.second.geometry;

            if (entry.second.transforms.empty() || !isIncluded(entry.second) || geometry.materialId.IsEmpty()) {
                continue;
            }
            const auto material = _materials.find(geometry.materialId);

            if (material == _materials.end() || !material->second) {
                continue;
            }
            if (material->second->kind == UtilityMaterialDesc::eKindUnsupported) {
                unsupported.insert(geometry.materialId.GetString());
            } else if (material->second->readsTextureCoordinates() && !geometry.hasSt) {
                withoutSt.insert(geometry.path.GetString());
            }
        }
    }

    std::vector<std::string> lines;

    if (!unsupported.empty()) {
        lines.push_back("Natron Utility cannot shade these materials and draws them magenta: "
                        + TfStringJoin(std::vector<std::string>(unsupported.begin(), unsupported.end()), ", ") + ".");
    }
    if (!withoutSt.empty()) {
        lines.push_back("These prims are textured but have no texture coordinates, so their textures read their fallbacks: "
                        + TfStringJoin(std::vector<std::string>(withoutSt.begin(), withoutSt.end()), ", ") + ".");
    }
    for (const std::string& failure : _textures->failures()) {
        lines.push_back(failure);
    }

    return lines;
}

NATRON_NAMESPACE_EXIT
