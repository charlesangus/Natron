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
#include <atomic>
#include <cmath>
#include <set>

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/stringUtils.h>

#include "Engine/Nodes/Scene/Utility/UtilityLight.h"
#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"
#include "Engine/Nodes/Scene/Utility/UtilityTextures.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// Scene versions count up from 0, so these never meet one.
const std::uint64_t kEvaluatedVersionBit = std::uint64_t(1) << 63;
std::atomic<std::uint64_t> gEvaluatedVersions(0);

GfMatrix3d
upperLeft(const GfMatrix4d& m)
{
    return GfMatrix3d(m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
}

// The sample before time and how far time is towards the next, clamped to the samples' span.
void
locate(const std::vector<double>& times,
       double time,
       std::size_t* index,
       double* u)
{
    if (times.size() < 2 || time <= times.front()) {
        *index = 0;
        *u = 0.;

        return;
    }
    if (time >= times.back()) {
        *index = times.size() - 2;
        *u = 1.;

        return;
    }
    const std::size_t next = static_cast<std::size_t>(std::upper_bound(times.begin(), times.end(), time) - times.begin());

    *index = next - 1;
    *u = (time - times[next - 1]) / (times[next] - times[next - 1]);
}
} // anon namespace

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
UtilityScene::setNormals(GeometryId id,
                         ColorBinding binding,
                         const VtVec3fArray& normals)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (!TF_VERIFY(it != _records.end())) {
        return;
    }
    Geometry& geometry = it->second.geometry;

    if (geometry.normals.empty() && normals.empty()) {
        return;
    }
    if (geometry.normalBinding == binding && geometry.normals == normals) {
        return;
    }
    geometry.normalBinding = binding;
    geometry.normals = normals;
    ++geometry.normalsVersion;
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
                        const VtVec4fArray& colors,
                        bool hasDisplayColor,
                        bool hasDisplayOpacity)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (!TF_VERIFY(it != _records.end())) {
        return;
    }
    Geometry& geometry = it->second.geometry;

    if (geometry.colorBinding == binding && geometry.colors == colors && geometry.hasDisplayColor == hasDisplayColor
        && geometry.hasDisplayOpacity == hasDisplayOpacity) {
        return;
    }
    geometry.colorBinding = binding;
    geometry.colors = colors;
    geometry.hasDisplayColor = hasDisplayColor;
    geometry.hasDisplayOpacity = hasDisplayOpacity;
    ++geometry.colorVersion;
    ++_colorUpdates;
    ++_version;
}

void
UtilityScene::setInstances(GeometryId id,
                           const std::vector<GfMatrix4d>& transforms,
                           const std::vector<TfTokenVector>& categories)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (TF_VERIFY(it != _records.end()) && (it->second.transforms != transforms || it->second.instanceCategories != categories)) {
        it->second.transforms = transforms;
        it->second.instanceCategories = categories;
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
UtilityScene::setUnwrapSt(GeometryId id,
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

    if (geometry.hasUnwrapSt == hasSt && geometry.unwrapStBinding == binding && geometry.unwrapSt == st) {
        return;
    }
    geometry.hasUnwrapSt = hasSt;
    geometry.unwrapStBinding = binding;
    geometry.unwrapSt = st;
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
UtilityScene::setCategories(GeometryId id,
                            const TfTokenVector& categories)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (TF_VERIFY(it != _records.end()) && it->second.geometry.categories != categories) {
        it->second.geometry.categories = categories;
        ++_version;
    }
}

void
UtilityScene::setLight(const SdfPath& path,
                       const std::shared_ptr<const UtilityLightDesc>& light)
{
    std::lock_guard<std::mutex> l(_mutex);

    _lights[path] = light;
    ++_version;
}

void
UtilityScene::removeLight(const SdfPath& path)
{
    std::lock_guard<std::mutex> l(_mutex);

    if (_lights.erase(path)) {
        ++_version;
    }
}

void
UtilityScene::setViewToWorld(const GfMatrix4d& viewToWorld)
{
    std::lock_guard<std::mutex> l(_mutex);

    if (viewToWorld == _viewToWorld) {
        return;
    }
    _viewToWorld = viewToWorld;
    for (const auto& entry : _lights) {
        if (entry.second && entry.second->cameraSpace) {
            ++_version;
            break;
        }
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
    for (const auto& entry : _records) {
        if (entry.second.topologyVaries) {
            stats.topologyVaryingPaths.push_back(entry.second.geometry.path);
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
    snapshot.materials = _materials;
    snapshot.lights = _lights;
    for (auto& entry : snapshot.lights) {
        if (entry.second && entry.second->cameraSpace) {
            auto placed = std::make_shared<UtilityLightDesc>(*entry.second);

            placed->transform = placed->transform * _viewToWorld;
            placed->cameraSpace = false;
            entry.second = placed;
        }
    }
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
            if (i < entry.second.instanceCategories.size()) {
                instance.categories = entry.second.instanceCategories[i];
            }
            instance.motion = entry.second.transformMotion;
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
    std::vector<std::string> iesProblems;

    {
        std::lock_guard<std::mutex> l(_mutex);

        for (const auto& light : _lights) {
            if (light.second && !light.second->iesProblem.empty()) {
                iesProblems.push_back(light.second->iesProblem);
            }
        }

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
    lines.insert(lines.end(), iesProblems.begin(), iesProblems.end());

    return lines;
}

void
UtilityScene::setPointMotion(GeometryId id,
                             const std::shared_ptr<const PointMotion>& motion)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (TF_VERIFY(it != _records.end()) && (it->second.geometry.pointMotion || motion)) {
        it->second.geometry.pointMotion = motion;
        ++_version;
    }
}

void
UtilityScene::setTransformMotion(GeometryId id,
                                 const std::shared_ptr<const TransformMotion>& motion)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (TF_VERIFY(it != _records.end()) && (it->second.transformMotion || motion)) {
        it->second.transformMotion = motion;
        ++_version;
    }
}

void
UtilityScene::setTopologyVaries(GeometryId id,
                                bool varies)
{
    std::lock_guard<std::mutex> l(_mutex);
    const auto it = _records.find(id);

    if (TF_VERIFY(it != _records.end())) {
        it->second.topologyVaries = varies;
    }
}

bool
UtilityScene::moves(const Snapshot& snapshot)
{
    for (const Instance& instance : snapshot.instances) {
        if (instance.motion || snapshot.geometries.at(instance.geometry).pointMotion) {
            return true;
        }
    }

    return false;
}

UtilityScene::Snapshot
UtilityScene::evaluateAt(const Snapshot& snapshot,
                         double time)
{
    Snapshot out = snapshot;

    out.version = kEvaluatedVersionBit | ++gEvaluatedVersions;
    for (auto& entry : out.geometries) {
        Geometry& geometry = entry.second;

        if (!geometry.pointMotion) {
            continue;
        }
        const std::shared_ptr<const PointMotion> held = geometry.pointMotion;
        const PointMotion& motion = *held;
        std::size_t index = 0;

        geometry.pointMotion.reset();
        if (motion.points.empty() || motion.points.size() != motion.times.size()) {
            continue;
        }
        double u = 0.;

        locate(motion.times, time, &index, &u);

        const VtVec3fArray& a = motion.points[index];
        const VtVec3fArray& b = motion.points[std::min(index + 1, motion.points.size() - 1)];

        if (a.size() != geometry.points.size() || b.size() != geometry.points.size()) {
            continue;
        }
        VtVec3fArray points(a.size());
        GfVec3f* written = points.data();
        const float weight = static_cast<float>(u);

        for (std::size_t i = 0; i < a.size(); ++i) {
            written[i] = a.cdata()[i] + (b.cdata()[i] - a.cdata()[i]) * weight;
        }
        geometry.points = points;
        geometry.pointsVersion = out.version;
        if (motion.normals.size() != motion.times.size() || geometry.normalBinding != eColorBindingVertex) {
            continue;
        }

        const VtVec3fArray& na = motion.normals[index];
        const VtVec3fArray& nb = motion.normals[std::min(index + 1, motion.normals.size() - 1)];

        if (na.size() != geometry.normals.size() || nb.size() != geometry.normals.size()) {
            continue;
        }
        VtVec3fArray normals(na.size());
        GfVec3f* blended = normals.data();

        for (std::size_t i = 0; i < na.size(); ++i) {
            const GfVec3f n = na.cdata()[i] + (nb.cdata()[i] - na.cdata()[i]) * weight;
            const float length = n.GetLength();

            blended[i] = length > 0.f && std::isfinite(length) ? n / length : GfVec3f(0.f);
        }
        geometry.normals = normals;
        geometry.normalsVersion = out.version;
    }
    for (Instance& instance : out.instances) {
        if (!instance.motion) {
            continue;
        }
        const std::shared_ptr<const TransformMotion> held = instance.motion;
        const TransformMotion& motion = *held;

        instance.motion.reset();
        if (motion.transforms.empty() || motion.transforms.size() != motion.times.size()) {
            continue;
        }
        const std::size_t which = static_cast<std::size_t>(instance.instanceIndex);
        std::size_t index = 0;
        double u = 0.;

        locate(motion.times, time, &index, &u);

        const std::size_t next = std::min(index + 1, motion.transforms.size() - 1);

        if (which < motion.transforms[index].size() && which < motion.transforms[next].size()) {
            instance.transform = interpolate(motion.transforms[index][which], motion.transforms[next][which], u);
        }
    }

    return out;
}

GfMatrix4d
UtilityScene::interpolate(const GfMatrix4d& a,
                          const GfMatrix4d& b,
                          double u)
{
    if (u <= 0. || a == b) {
        return a;
    }
    if (u >= 1.) {
        return b;
    }
    const GfMatrix4d rotationA = a.RemoveScaleShear();
    const GfMatrix4d rotationB = b.RemoveScaleShear();
    // Row vectors: the upper left is the stretch then the rotation.
    const GfMatrix3d stretchA = upperLeft(a) * upperLeft(rotationA).GetTranspose();
    const GfMatrix3d stretchB = upperLeft(b) * upperLeft(rotationB).GetTranspose();
    const GfQuatd from = rotationA.ExtractRotationQuat();
    GfQuatd to = rotationB.ExtractRotationQuat();

    if (GfDot(from, to) < 0.) {
        to = to * -1.;
    }
    GfMatrix3d rotation(1.);

    rotation.SetRotate(GfSlerp(u, from, to));

    const GfMatrix3d stretch = stretchA * (1. - u) + stretchB * u;
    const GfVec3d translation = a.ExtractTranslation() * (1. - u) + b.ExtractTranslation() * u;

    return GfMatrix4d(stretch * rotation, translation);
}

NATRON_NAMESPACE_EXIT
