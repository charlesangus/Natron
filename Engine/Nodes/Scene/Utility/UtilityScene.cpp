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
#include <bit>
#include <cmath>
#include <set>

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
const std::uint64_t kShutterVersionBit = std::uint64_t(1) << 63;
// Points sampled between looks at the stop flag.
const std::size_t kEvaluateChunk = std::size_t(1) << 16;

std::uint64_t
mixVersion(std::uint64_t seed,
           std::uint64_t value)
{
    std::uint64_t z = seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;

    return z ^ (z >> 31);
}

std::uint64_t
bitsOf(double value)
{
    return std::bit_cast<std::uint64_t>(value);
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
UtilityScene::atShutter(const Snapshot& snapshot,
                        double open,
                        double close,
                        int segments)
{
    const std::atomic<bool> never(false);
    Snapshot out;

    (void)atShutter(snapshot, open, close, segments, never, &out);

    return out;
}

bool
UtilityScene::atShutter(const Snapshot& snapshot,
                        double open,
                        double close,
                        int segments,
                        const std::atomic<bool>& stop,
                        Snapshot* atShutter)
{
    const auto stopped = [&stop]() { return stop.load(std::memory_order_relaxed); };
    Snapshot& out = *atShutter;

    out = snapshot;
    out.timeSteps = 1;
    out.shutterOpen = open;
    out.shutterClose = close;
    if (!(close > open) || segments < 1 || !moves(snapshot)) {
        return true;
    }

    const int steps = segments + 1;
    const std::uint64_t shutterKey = mixVersion(mixVersion(mixVersion(0, bitsOf(open)), bitsOf(close)), static_cast<std::uint64_t>(segments));
    std::vector<double> times(static_cast<std::size_t>(steps));

    for (int k = 0; k < steps; ++k) {
        times[static_cast<std::size_t>(k)] = open + (close - open) * k / segments;
    }
    out.timeSteps = steps;
    out.version = kShutterVersionBit | (mixVersion(snapshot.version, shutterKey) & ~kShutterVersionBit);
    for (auto& entry : out.geometries) {
        if (stopped()) {
            return false;
        }

        Geometry& geometry = entry.second;

        if (!geometry.pointMotion) {
            continue;
        }
        const std::shared_ptr<const PointMotion> held = geometry.pointMotion;
        const PointMotion& motion = *held;

        geometry.pointMotion.reset();
        if (motion.points.empty() || motion.points.size() != motion.times.size()) {
            continue;
        }

        bool sized = true;

        for (const VtVec3fArray& sample : motion.points) {
            sized = sized && sample.size() == geometry.points.size();
        }
        if (!sized) {
            continue;
        }
        const bool withNormals = motion.normals.size() == motion.times.size() && geometry.normalBinding == eColorBindingVertex
            && std::all_of(motion.normals.begin(), motion.normals.end(), [&](const VtVec3fArray& n) { return n.size() == geometry.normals.size(); });

        geometry.pointSteps.resize(static_cast<std::size_t>(steps));
        if (withNormals) {
            geometry.normalSteps.resize(static_cast<std::size_t>(steps));
        }
        for (int k = 0; k < steps; ++k) {
            std::size_t index = 0;
            double u = 0.;

            locate(motion.times, times[static_cast<std::size_t>(k)], &index, &u);

            const std::size_t next = std::min(index + 1, motion.points.size() - 1);
            const VtVec3fArray& a = motion.points[index];
            const VtVec3fArray& b = motion.points[next];
            VtVec3fArray points(a.size());
            GfVec3f* written = points.data();
            const float weight = static_cast<float>(u);

            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i % kEvaluateChunk == 0 && stopped()) {
                    return false;
                }
                written[i] = a.cdata()[i] + (b.cdata()[i] - a.cdata()[i]) * weight;
            }
            geometry.pointSteps[static_cast<std::size_t>(k)] = points;
            if (!withNormals) {
                continue;
            }

            const VtVec3fArray& na = motion.normals[index];
            const VtVec3fArray& nb = motion.normals[next];
            VtVec3fArray normals(na.size());
            GfVec3f* blended = normals.data();

            for (std::size_t i = 0; i < na.size(); ++i) {
                if (i % kEvaluateChunk == 0 && stopped()) {
                    return false;
                }

                const GfVec3f n = na.cdata()[i] + (nb.cdata()[i] - na.cdata()[i]) * weight;
                const float length = n.GetLength();

                blended[i] = length > 0.f && std::isfinite(length) ? n / length : GfVec3f(0.f);
            }
            geometry.normalSteps[static_cast<std::size_t>(k)] = normals;
        }
        geometry.pointsVersion = kShutterVersionBit | (mixVersion(geometry.pointsVersion, shutterKey) & ~kShutterVersionBit);
        if (withNormals) {
            geometry.normalsVersion = kShutterVersionBit | (mixVersion(geometry.normalsVersion, shutterKey) & ~kShutterVersionBit);
        }
    }
    for (std::size_t i = 0; i < out.instances.size(); ++i) {
        Instance& instance = out.instances[i];

        if (i % kEvaluateChunk == 0 && stopped()) {
            return false;
        }
        if (!instance.motion) {
            continue;
        }
        const std::shared_ptr<const TransformMotion> held = instance.motion;
        const TransformMotion& motion = *held;
        const std::size_t which = static_cast<std::size_t>(instance.instanceIndex);

        instance.motion.reset();
        if (motion.transforms.empty() || motion.transforms.size() != motion.times.size()
            || !std::all_of(motion.transforms.begin(), motion.transforms.end(), [which](const std::vector<GfMatrix4d>& t) { return which < t.size(); })) {
            continue;
        }
        instance.transformSteps.resize(static_cast<std::size_t>(steps));
        for (int k = 0; k < steps; ++k) {
            std::size_t index = 0;
            double u = 0.;

            locate(motion.times, times[static_cast<std::size_t>(k)], &index, &u);

            const std::size_t next = std::min(index + 1, motion.transforms.size() - 1);
            const Decomposition a = decompose(motion.transforms[index][which]);
            Decomposition step = u > 0. ? blend(a, decompose(motion.transforms[next][which]), u) : a;

            if (k > 0 && GfDot(step.rotation, instance.transformSteps[static_cast<std::size_t>(k - 1)].rotation) < 0.) {
                step.rotation = -step.rotation;
            }
            instance.transformSteps[static_cast<std::size_t>(k)] = step;
        }
    }

    return true;
}

void
UtilityScene::stepAt(double time,
                     int steps,
                     int* span,
                     double* fraction)
{
    const int segments = std::max(1, steps - 1);
    const double scaled = std::min(std::max(time, 0.), 1.) * segments;
    const int s = std::min(static_cast<int>(std::floor(scaled)), segments - 1);

    *span = s;
    *fraction = scaled - s;
}

UtilityScene::Decomposition
UtilityScene::decompose(const GfMatrix4d& m)
{
    Decomposition d;

    d.translation = GfVec3d(m[3][0], m[3][1], m[3][2]);

    // Row vectors: m's row j is column j of the column-vector form, which Gram-Schmidt factors as R * U.
    const GfVec3d columns[3] = { GfVec3d(m[0][0], m[0][1], m[0][2]), GfVec3d(m[1][0], m[1][1], m[1][2]), GfVec3d(m[2][0], m[2][1], m[2][2]) };
    const double largest = std::max({ columns[0].GetLength(), columns[1].GetLength(), columns[2].GetLength() });
    GfVec3d q[3];
    double u[3][3] = { { 0., 0., 0. }, { 0., 0., 0. }, { 0., 0., 0. } };

    for (int j = 0; j < 3; ++j) {
        GfVec3d v = columns[j];

        for (int k = 0; k < j; ++k) {
            u[k][j] = GfDot(q[k], columns[j]);
            v -= u[k][j] * q[k];
        }

        const double length = v.GetLength();

        if (length > 1e-12 * largest && std::isfinite(length)) {
            q[j] = v / length;
            u[j][j] = length;
            continue;
        }
        // A flattened axis keeps a rotation by completing the basis.
        if (j == 2) {
            q[2] = GfCross(q[0], q[1]);
            continue;
        }
        GfVec3d axis(1., 0., 0.);

        if (j == 1) {
            const GfVec3d candidates[3] = { GfVec3d(1., 0., 0.), GfVec3d(0., 1., 0.), GfVec3d(0., 0., 1.) };

            for (const GfVec3d& c : candidates) {
                if (std::abs(GfDot(c, q[0])) < std::abs(GfDot(axis, q[0]))) {
                    axis = c;
                }
            }
            axis -= GfDot(axis, q[0]) * q[0];
            axis.Normalize();
        }
        q[j] = axis;
    }
    if (GfDot(GfCross(q[0], q[1]), q[2]) < 0.) {
        q[2] = -q[2];
        u[2][2] = -u[2][2];
    }

    // R's column j is q[j], so R[i][j] = q[j][i].
    const double r00 = q[0][0], r11 = q[1][1], r22 = q[2][2];
    const double r21 = q[1][2], r12 = q[2][1], r02 = q[2][0], r20 = q[0][2], r10 = q[0][1], r01 = q[1][0];
    const double trace = r00 + r11 + r22;
    GfVec4d quat;

    if (trace > 0.) {
        const double s = std::sqrt(trace + 1.) * 2.;

        quat = GfVec4d(0.25 * s, (r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s);
    } else if (r00 > r11 && r00 > r22) {
        const double s = std::sqrt(1. + r00 - r11 - r22) * 2.;

        quat = GfVec4d((r21 - r12) / s, 0.25 * s, (r01 + r10) / s, (r02 + r20) / s);
    } else if (r11 > r22) {
        const double s = std::sqrt(1. + r11 - r00 - r22) * 2.;

        quat = GfVec4d((r02 - r20) / s, (r01 + r10) / s, 0.25 * s, (r12 + r21) / s);
    } else {
        const double s = std::sqrt(1. + r22 - r00 - r11) * 2.;

        quat = GfVec4d((r10 - r01) / s, (r02 + r20) / s, (r12 + r21) / s, 0.25 * s);
    }
    d.rotation = quat.GetNormalized();
    d.stretch[0] = u[0][0];
    d.stretch[1] = u[0][1];
    d.stretch[2] = u[0][2];
    d.stretch[3] = u[1][1];
    d.stretch[4] = u[1][2];
    d.stretch[5] = u[2][2];

    return d;
}

GfMatrix4d
UtilityScene::compose(const Decomposition& d)
{
    const double r = d.rotation[0], i = d.rotation[1], j = d.rotation[2], k = d.rotation[3];
    const double n = r * r + i * i + j * j + k * k;
    const double s = n > 0. ? 2. / n : 0.;
    const double rotation[3][3] = {
        { 1. - s * (j * j + k * k), s * (i * j - k * r), s * (i * k + j * r) },
        { s * (i * j + k * r), 1. - s * (i * i + k * k), s * (j * k - i * r) },
        { s * (i * k - j * r), s * (j * k + i * r), 1. - s * (i * i + j * j) },
    };
    const double stretch[3][3] = {
        { d.stretch[0], d.stretch[1], d.stretch[2] },
        { 0., d.stretch[3], d.stretch[4] },
        { 0., 0., d.stretch[5] },
    };
    GfMatrix4d m(1.);

    // The column-vector product's entry (row, column) is m[column][row] in row vectors.
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            double sum = 0.;

            for (int x = 0; x < 3; ++x) {
                sum += rotation[row][x] * stretch[x][column];
            }
            m[column][row] = sum;
        }
        m[row][3] = 0.;
        m[3][row] = d.translation[row];
    }

    return m;
}

UtilityScene::Decomposition
UtilityScene::blend(const Decomposition& a,
                    const Decomposition& b,
                    double u)
{
    Decomposition out;
    GfVec4d to = b.rotation;
    double cosine = GfDot(a.rotation, to);

    if (cosine < 0.) {
        to = -to;
        cosine = -cosine;
    }
    out.translation = a.translation * (1. - u) + b.translation * u;
    for (int i = 0; i < 6; ++i) {
        out.stretch[i] = a.stretch[i] * (1. - u) + b.stretch[i] * u;
    }
    // Nearly parallel rotations lose slerp's precision in its sine, so they blend linearly as Embree's do.
    if (cosine > 0.9995) {
        out.rotation = (a.rotation * (1. - u) + to * u).GetNormalized();

        return out;
    }

    const double theta = std::acos(std::min(cosine, 1.));
    const double phi = u * theta;
    const GfVec4d perpendicular = (to - cosine * a.rotation).GetNormalized();

    out.rotation = std::cos(phi) * a.rotation + std::sin(phi) * perpendicular;

    return out;
}

GfMatrix4d
UtilityScene::transformAt(const Instance& instance,
                          double time)
{
    if (instance.transformSteps.empty()) {
        return instance.transform;
    }
    if (instance.transformSteps.size() == 1) {
        return compose(instance.transformSteps.front());
    }

    int span = 0;
    double fraction = 0.;

    stepAt(time, static_cast<int>(instance.transformSteps.size()), &span, &fraction);

    return compose(blend(instance.transformSteps[static_cast<std::size_t>(span)], instance.transformSteps[static_cast<std::size_t>(span) + 1], fraction));
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

    return compose(blend(decompose(a), decompose(b), u));
}

NATRON_NAMESPACE_EXIT
