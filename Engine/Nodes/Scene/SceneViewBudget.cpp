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

#include "Engine/Nodes/Scene/SceneViewBudget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

#ifdef emit
#define NATRON_SCENEVIEWBUDGET_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/vec4d.h>
#ifdef NATRON_SCENEVIEWBUDGET_RESTORE_EMIT
#undef NATRON_SCENEVIEWBUDGET_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
const double kBand = 0.05;
const std::size_t kBuckets = 1 << 16;
// How far past the view's sides a unit still counts as in view, as a share of the view's width.
const double kFrustumMargin = 0.1;

enum FlagEnum : std::uint8_t {
    eFlagDrawn = 1,
    eFlagCounted = 2,
    eFlagCulled = 4,
    eFlagFractioned = 8,
    eFlagDemoted = 16,
    // Drawn but out of view, with Count Whole Scene off: it gets what the counted units leave.
    eFlagOutside = 32
};

struct Sphere {
    GfVec3d centre;
    double radius = 0.;
};

// The planes a sphere must not lie wholly behind, as (normal, offset) with the normal unit length.
struct Frustum {
    GfVec4d planes[6];

    explicit Frustum(const GfMatrix4d& viewProjection)
    {
        // With row vectors, clip = p * M, so each clip coordinate is p against a column of M.
        GfVec4d columns[4];
        for (int c = 0; c < 4; ++c) {
            columns[c] = GfVec4d(viewProjection[0][c], viewProjection[1][c], viewProjection[2][c], viewProjection[3][c]);
        }
        for (int axis = 0; axis < 3; ++axis) {
            // Only the sides widen: the near and far planes stay where the view has them.
            const double widen = (axis < 2) ? (1. + kFrustumMargin) : 1.;
            planes[2 * axis] = columns[3] * widen + columns[axis];
            planes[2 * axis + 1] = columns[3] * widen - columns[axis];
        }
        for (GfVec4d& plane : planes) {
            const double length = GfVec3d(plane[0], plane[1], plane[2]).GetLength();
            if (length > 0.) {
                plane /= length;
            }
        }
    }

    bool meets(const Sphere& sphere) const
    {
        for (const GfVec4d& plane : planes) {
            if (plane[0] * sphere.centre[0] + plane[1] * sphere.centre[1] + plane[2] * sphere.centre[2] + plane[3] < -sphere.radius) {
                return false;
            }
        }

        return true;
    }
};

Sphere
sphereOf(const GfVec3f& min,
         const GfVec3f& max,
         bool bounded)
{
    Sphere sphere;

    if (!bounded) {
        sphere.centre = GfVec3d(min);

        return sphere;
    }
    sphere.centre = (GfVec3d(min) + GfVec3d(max)) * 0.5;
    sphere.radius = (GfVec3d(max) - GfVec3d(min)).GetLength() * 0.5;

    return sphere;
}

double
distanceTo(const GfVec3d& eye,
           const Sphere& sphere)
{
    return (std::max)(0., (sphere.centre - eye).GetLength() - sphere.radius);
}

// The largest factor by which the matrix's upper 3x3 stretches a vector's length, bounded above by
// the largest row length times sqrt(3).
double
maxStretch(const GfMatrix4d& matrix)
{
    double longest = 0.;

    for (int row = 0; row < 3; ++row) {
        longest = (std::max)(longest, GfVec3d(matrix[row][0], matrix[row][1], matrix[row][2]).GetLength());
    }

    return longest * std::sqrt(3.);
}

// Whether a unit at distance is beyond limit, a previous decision holding within the band of it.
bool
beyond(double distance,
       double limit,
       bool hadPrevious,
       bool previous)
{
    if (limit <= 0.) {
        return false;
    }
    if (hadPrevious && (std::abs(distance - limit) <= kBand * limit)) {
        return previous;
    }

    return distance > limit;
}

bool
bitAt(const std::vector<bool>& bits,
      std::size_t index,
      bool otherwise)
{
    return (index < bits.size()) ? bits[index] : otherwise;
}

void
setBit(std::vector<bool>& bits,
       std::size_t size,
       std::size_t index,
       bool value,
       bool otherwise)
{
    if (bits.empty()) {
        if (value == otherwise) {
            return;
        }
        bits.assign(size, otherwise);
    }
    bits[index] = value;
}

// What a gprim comes to before demotion.
struct Entry {
    float distance = 0.f;
    std::uint8_t flags = 0;
    U64 faces = 0;
};

// An instancer's instances, kept as narrow as they can be since an instancer may hold tens of
// millions. An instance's faces are its prototype's.
struct InstancerWork {
    const SceneViewBudget::Instances* instances = nullptr;
    std::vector<float> distance;
    std::vector<std::uint8_t> flags;
};

struct Work {
    // One per unit; an instancer unit's stays undrawn.
    std::vector<Entry> gprims;
    // Units by index, for the instancers.
    std::vector<std::pair<std::size_t, InstancerWork>> instancers;
};

std::uint8_t
withFlag(std::uint8_t flags,
         FlagEnum flag,
         bool on)
{
    return on ? std::uint8_t(flags | flag) : std::uint8_t(flags & ~std::uint8_t(flag));
}

// The distance and faces of every gprim and instance with the flag.
template <typename Visit>
void
forFlagged(const Work& work,
           FlagEnum flag,
           Visit&& visit)
{
    for (const Entry& entry : work.gprims) {
        if (entry.flags & flag) {
            visit(entry.distance, entry.faces);
        }
    }
    for (const std::pair<std::size_t, InstancerWork>& instancer : work.instancers) {
        const InstancerWork& at = instancer.second;
        for (std::size_t index = 0; index < at.flags.size(); ++index) {
            if (at.flags[index] & flag) {
                visit(at.distance[index], at.instances->prototypes[at.instances->protoIndices[index]].faces);
            }
        }
    }
}

// The distance from which the units with the flag must go, farthest first, for the faces of the
// nearer ones to fit limit; infinite when all fit.
double
demoteDistance(const Work& work,
               FlagEnum flag,
               U64 counted,
               U64 limit,
               double farthest)
{
    if (counted <= limit) {
        return std::numeric_limits<double>::infinity();
    }
    // Faces by distance in buckets, then the bucket where the nearer faces pass the limit sorted.
    std::vector<U64> buckets(kBuckets, 0);
    const double scale = (farthest > 0.) ? (double(kBuckets - 1) / farthest) : 0.;
    const auto bucketOf = [&](float distance) {
        return (std::min)(std::size_t(double(distance) * scale), kBuckets - 1);
    };
    forFlagged(work, flag, [&](float distance, U64 faces) { buckets[bucketOf(distance)] += faces; });
    U64 nearer = 0;
    std::size_t crossing = 0;
    for (; crossing < kBuckets; ++crossing) {
        if (nearer + buckets[crossing] > limit) {
            break;
        }
        nearer += buckets[crossing];
    }
    std::vector<std::pair<float, U64>> inside;
    forFlagged(work, flag, [&](float distance, U64 faces) {
        if (bucketOf(distance) == crossing) {
            inside.emplace_back(distance, faces);
        }
    });
    std::sort(inside.begin(), inside.end());
    for (const std::pair<float, U64>& item : inside) {
        if (nearer + item.second > limit) {
            return item.first;
        }
        nearer += item.second;
    }

    return std::numeric_limits<double>::infinity();
}

// Marks the gprims and instances with the flag that the distance demotes, a previous decision
// holding within the band of it when banded, and returns the faces left.
template <typename Previous>
U64
demote(Work& work,
       FlagEnum flag,
       double distance,
       bool banded,
       Previous&& previous)
{
    U64 drawn = 0;
    const bool finite = std::isfinite(distance);
    const auto decide = [&](float at, std::size_t unit, std::size_t index) {
        if (banded && finite && (std::abs(double(at) - distance) <= kBand * distance)) {
            return bool(previous(unit, index));
        }

        return finite && (double(at) >= distance);
    };

    for (std::size_t unit = 0; unit < work.gprims.size(); ++unit) {
        Entry& entry = work.gprims[unit];
        if (!(entry.flags & flag)) {
            continue;
        }
        const bool demoted = decide(entry.distance, unit, std::size_t(-1));
        entry.flags = withFlag(entry.flags, eFlagDemoted, demoted);
        drawn += demoted ? 0 : entry.faces;
    }
    for (std::pair<std::size_t, InstancerWork>& instancer : work.instancers) {
        InstancerWork& at = instancer.second;
        for (std::size_t index = 0; index < at.flags.size(); ++index) {
            if (!(at.flags[index] & flag)) {
                continue;
            }
            const bool demoted = decide(at.distance[index], instancer.first, index);
            at.flags[index] = withFlag(at.flags[index], eFlagDemoted, demoted);
            drawn += demoted ? 0 : at.instances->prototypes[at.instances->protoIndices[index]].faces;
        }
    }

    return drawn;
}
} // anon namespace

bool
SceneViewBudget::Instances::draws(std::size_t index) const
{
    if (index >= protoIndices.size()) {
        return false;
    }
    const int prototype = protoIndices[index];
    if ((prototype < 0) || (std::size_t(prototype) >= prototypes.size())) {
        return false;
    }

    return mask.empty() || ((index < mask.size()) && mask[index]);
}

GfMatrix4d
SceneViewBudget::Instances::instanceTransform(std::size_t index) const
{
    GfMatrix4d scale(1.);
    if (index < scales.size()) {
        scale.SetScale(GfVec3d(scales[index]));
    }
    GfMatrix4d rotate(1.);
    if (!orientationsf.empty()) {
        if (index < orientationsf.size()) {
            rotate.SetRotate(GfQuatd(orientationsf[index]));
        }
    } else if (index < orientations.size()) {
        rotate.SetRotate(GfQuatd(orientations[index]));
    }
    GfMatrix4d translate(1.);
    if (index < positions.size()) {
        translate.SetTranslate(GfVec3d(positions[index]));
    }

    return scale * rotate * translate * toWorld;
}

SceneViewBudget::Instances::Spheres
SceneViewBudget::Instances::prototypeSpheres() const
{
    Spheres spheres;

    spheres.stretch = maxStretch(toWorld);
    spheres.centres.reserve(prototypes.size());
    spheres.radii.reserve(prototypes.size());
    for (const Prototype& prototype : prototypes) {
        const Sphere sphere = sphereOf(prototype.min, prototype.max, prototype.bounded);
        spheres.centres.push_back(sphere.centre);
        spheres.radii.push_back(sphere.radius);
    }

    return spheres;
}

// For a drawn instance.
void
SceneViewBudget::Instances::instanceSphere(const Spheres& spheres,
                                           std::size_t index,
                                           GfVec3d* centre,
                                           double* radius) const
{
    const int prototype = protoIndices[index];
    const GfVec3d& local = spheres.centres[prototype];
    GfVec3d scale(1.);
    if (index < scales.size()) {
        scale = GfVec3d(scales[index]);
    }
    GfVec3d at(local[0] * scale[0], local[1] * scale[1], local[2] * scale[2]);
    if (!orientationsf.empty()) {
        if (index < orientationsf.size()) {
            at = GfQuatd(orientationsf[index]).GetNormalized().Transform(at);
        }
    } else if (index < orientations.size()) {
        at = GfQuatd(orientations[index]).GetNormalized().Transform(at);
    }
    if (index < positions.size()) {
        at += GfVec3d(positions[index]);
    }
    *centre = toWorld.Transform(at);
    *radius = spheres.radii[prototype] * (std::max)({ std::abs(scale[0]), std::abs(scale[1]), std::abs(scale[2]) }) * spheres.stretch;
}

bool
SceneViewBudget::Result::isDemoted(const SdfPath& path) const
{
    return demoted.count(path) > 0;
}

bool
SceneViewBudget::Result::isCulled(const SdfPath& path) const
{
    return culled.count(path) > 0;
}

bool
SceneViewBudget::Result::isInstanceDemoted(const SdfPath& instancer,
                                           std::size_t index) const
{
    const auto found = instancers.find(instancer);

    return (found != instancers.end()) && bitAt(found->second.demoted, index, false);
}

bool
SceneViewBudget::Result::isInstanceCulled(const SdfPath& instancer,
                                          std::size_t index) const
{
    const auto found = instancers.find(instancer);

    return (found != instancers.end()) && bitAt(found->second.culled, index, false);
}

bool
SceneViewBudget::Result::isInstanceKept(const SdfPath& instancer,
                                        std::size_t index) const
{
    const auto found = instancers.find(instancer);

    return (found == instancers.end()) || bitAt(found->second.kept, index, true);
}

U64
SceneViewBudget::pathSeed(const SdfPath& path)
{
    // FNV-1a over the path's text, which unlike SdfPath::Hash is the same in every session.
    U64 hash = 14695981039346656037ull;

    for (const char c : path.GetString()) {
        hash ^= U64(static_cast<unsigned char>(c));
        hash *= 1099511628211ull;
    }

    return hash;
}

bool
SceneViewBudget::keeps(U64 seed,
                       std::size_t index,
                       double fraction)
{
    // splitmix64's finaliser, so neighbouring indices fall far apart.
    U64 x = seed + 0x9E3779B97F4A7C15ull * (U64(index) + 1);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;

    return double(x >> 11) * (1. / 9007199254740992.) < fraction;
}

bool
SceneViewBudget::keepsInstance(const SdfPath& instancer,
                               std::size_t index,
                               double percent)
{
    return keeps(pathSeed(instancer), index, percent / 100.);
}

bool
SceneViewBudget::keepsFace(const SdfPath& mesh,
                           std::size_t face,
                           double fraction)
{
    return keeps(pathSeed(mesh), face, fraction);
}

SceneViewBudget::Result
SceneViewBudget::solve(const std::vector<Unit>& units,
                       const Camera& camera,
                       const Settings& settings,
                       const Result& previous)
{
    Result result;

    if (!camera.valid) {
        return result;
    }
    result.valid = true;
    result.limit = settings.limit;
    const bool hadPrevious = previous.valid;
    const Frustum frustum(camera.viewProjection);
    const double fraction = (std::min)((std::max)(settings.fractionPercent, 0.), 100.) / 100.;
    Work work;
    work.gprims.resize(units.size());
    U64 counted = 0;
    double farthest = 0.;
    // The faces the previous demotion distance would leave drawn.
    U64 underPrevious = 0;
    double aloneDistance = std::numeric_limits<double>::infinity();
    U64 outside = 0;
    double outsideFarthest = 0.;
    // Whether the unit counts; it then joins the count, and otherwise those out of view.
    const auto count = [&](double distance, U64 faces, bool inFrustum) {
        if (!inFrustum && !settings.countWholeScene) {
            outside += faces;
            outsideFarthest = (std::max)(outsideFarthest, distance);

            return false;
        }
        counted += faces;
        farthest = (std::max)(farthest, distance);
        if (!(float(distance) >= previous.demoteDistance)) {
            underPrevious += faces;
        }

        return true;
    };

    for (std::size_t unitIndex = 0; unitIndex < units.size(); ++unitIndex) {
        const Unit& unit = units[unitIndex];
        if (unit.state != eStateDrawn) {
            continue;
        }
        if (!unit.instances) {
            Entry& entry = work.gprims[unitIndex];
            const Sphere sphere = sphereOf(unit.min, unit.max, unit.bounded);
            const double distance = distanceTo(camera.eye, sphere);
            entry.distance = float(distance);
            entry.flags = eFlagDrawn;
            entry.faces = unit.faces;
            if (beyond(distance, settings.cullDistance, hadPrevious, previous.isCulled(unit.path))) {
                entry.flags = withFlag(entry.flags, eFlagCulled, true);
                result.culled.insert(unit.path);
                ++result.culledCount;
                continue;
            }
            if (unit.mesh && beyond(distance, settings.fractionDistance, hadPrevious, previous.keptFaceFraction.count(unit.path) > 0)) {
                result.keptFaceFraction[unit.path] = fraction;
                entry.faces = U64(std::ceil(double(unit.faces) * fraction));
            }
            if (count(entry.distance, entry.faces, frustum.meets(sphere))) {
                entry.flags = withFlag(entry.flags, eFlagCounted, true);
                if ((entry.faces > settings.limit) && (distance < aloneDistance)) {
                    aloneDistance = distance;
                    result.aloneOverLimit = unit.path;
                    result.aloneOverLimitIsInstance = false;
                }
            } else {
                entry.flags = withFlag(entry.flags, eFlagOutside, true);
            }
            continue;
        }
        const Instances& instances = *unit.instances;
        const std::size_t size = instances.size();
        work.instancers.emplace_back(unitIndex, InstancerWork());
        InstancerWork& at = work.instancers.back().second;
        at.instances = &instances;
        at.distance.assign(size, 0.f);
        at.flags.assign(size, 0);
        InstancerResult record;
        const U64 seed = pathSeed(unit.path);
        const Instances::Spheres spheres = instances.prototypeSpheres();
        // Looked up once: a hash per instance costs seconds over tens of millions.
        const auto before = hadPrevious ? previous.instancers.find(unit.path) : previous.instancers.end();
        const InstancerResult* was = (before != previous.instancers.end()) ? &before->second : nullptr;
        for (std::size_t index = 0; index < size; ++index) {
            if (!instances.draws(index)) {
                continue;
            }
            const int prototype = instances.protoIndices[index];
            Sphere sphere;
            instances.instanceSphere(spheres, index, &sphere.centre, &sphere.radius);
            const double distance = distanceTo(camera.eye, sphere);
            at.distance[index] = float(distance);
            at.flags[index] = eFlagDrawn;
            if (beyond(distance, settings.cullDistance, hadPrevious, was && bitAt(was->culled, index, false))) {
                at.flags[index] = withFlag(at.flags[index], eFlagCulled, true);
                setBit(record.culled, size, index, true, false);
                ++record.culledCount;
                continue;
            }
            if (!keeps(seed, index, fraction) && beyond(distance, settings.fractionDistance, hadPrevious, was && !bitAt(was->kept, index, true))) {
                at.flags[index] = withFlag(at.flags[index], eFlagFractioned, true);
                setBit(record.kept, size, index, false, true);
                ++record.fractionedCount;
                continue;
            }
            const U64 faces = instances.prototypes[prototype].faces;
            if (count(at.distance[index], faces, frustum.meets(sphere))) {
                at.flags[index] = withFlag(at.flags[index], eFlagCounted, true);
                if ((faces > settings.limit) && (distance < aloneDistance)) {
                    aloneDistance = distance;
                    result.aloneOverLimit = unit.path;
                    result.aloneOverLimitInstance = index;
                    result.aloneOverLimitIsInstance = true;
                }
            } else {
                at.flags[index] = withFlag(at.flags[index], eFlagOutside, true);
            }
        }
        result.culledCount += record.culledCount;
        if (record.culledCount || record.fractionedCount) {
            result.instancers[unit.path] = std::move(record);
        }
    }
    result.counted = counted;

    // The previous demotion distance holds while what it leaves is within the band of the limit.
    const double band = kBand * double(settings.limit);
    double distance = demoteDistance(work, eFlagCounted, counted, settings.limit, farthest);
    // A unit over the limit alone always goes, however near the boundary, and nothing does when
    // everything fits.
    bool banded = hadPrevious && result.aloneOverLimit.IsEmpty() && (counted > settings.limit);
    if (banded) {
        const bool finite = std::isfinite(previous.demoteDistance);
        if ((double(underPrevious) <= double(settings.limit) + band) && (!finite || (double(underPrevious) >= double(settings.limit) - band))) {
            distance = previous.demoteDistance;
        }
    }
    const auto wasDemoted = [&](std::size_t unit, std::size_t index) {
        if (index == std::size_t(-1)) {
            return previous.isDemoted(units[unit].path);
        }

        return previous.isInstanceDemoted(units[unit].path, index);
    };
    U64 drawn = demote(work, eFlagCounted, distance, banded, wasDemoted);
    if (double(drawn) > double(settings.limit) + band) {
        distance = demoteDistance(work, eFlagCounted, counted, settings.limit, farthest);
        banded = false;
        drawn = demote(work, eFlagCounted, distance, banded, wasDemoted);
    }
    // What is out of view draws as budget boxes, so a pan onto it never shows more than the limit
    // before the next budget, except the nearest of it that fits in the faces the view leaves.
    if (outside != 0) {
        const U64 left = (drawn < settings.limit) ? (settings.limit - drawn) : 0;
        const double outsideDistance = demoteDistance(work, eFlagOutside, outside, left, outsideFarthest);
        drawn += demote(work, eFlagOutside, outsideDistance, false, wasDemoted);
    }
    result.drawn = drawn;
    result.demoteDistance = distance;

    for (std::size_t unitIndex = 0; unitIndex < units.size(); ++unitIndex) {
        if (work.gprims[unitIndex].flags & eFlagDemoted) {
            result.demoted.insert(units[unitIndex].path);
            ++result.demotedCount;
        }
    }
    for (const std::pair<std::size_t, InstancerWork>& instancer : work.instancers) {
        const InstancerWork& at = instancer.second;
        InstancerResult* record = nullptr;
        for (std::size_t index = 0; index < at.flags.size(); ++index) {
            if (!(at.flags[index] & eFlagDemoted)) {
                continue;
            }
            if (!record) {
                record = &result.instancers[units[instancer.first].path];
            }
            setBit(record->demoted, at.flags.size(), index, true, false);
            ++record->demotedCount;
            ++result.demotedCount;
        }
    }

    return result;
} // SceneViewBudget::solve

bool
SceneViewBudget::loadChunkWaits(U64 followUp)
{
    return (followUp != 0) && ((followUp & (followUp - 1)) == 0);
}

NATRON_NAMESPACE_EXIT
