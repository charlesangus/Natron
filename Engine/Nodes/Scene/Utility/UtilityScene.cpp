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

#include <pxr/base/tf/diagnostic.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

UtilityScene::UtilityScene()
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
UtilityScene::setNormals(GeometryId id,
                         const VtVec3fArray& normals)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (!TF_VERIFY(it != _records.end())) {
        return;
    }
    if (it->second.geometry.normals.empty() && normals.empty()) {
        return;
    }
    it->second.geometry.normals = normals;
    ++_version;
}

void
UtilityScene::setWarning(GeometryId id,
                         const std::string& warning)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (TF_VERIFY(it != _records.end())) {
        it->second.warning = warning;
    }
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
        if (!entry.second.warning.empty()) {
            stats.warnings.push_back(entry.second.warning);
        }
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

NATRON_NAMESPACE_EXIT
