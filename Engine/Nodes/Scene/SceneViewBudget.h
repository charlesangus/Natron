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

#ifndef Engine_Nodes_Scene_SceneViewBudget_h
#define Engine_Nodes_Scene_SceneViewBudget_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Global/GlobalDefines.h"

#ifdef emit
#define NATRON_SCENEVIEWBUDGET_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/usd/sdf/path.h>
#ifdef NATRON_SCENEVIEWBUDGET_RESTORE_EMIT
#undef NATRON_SCENEVIEWBUDGET_RESTORE_EMIT
#define emit
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief The 3D viewer's polygon budget: which drawn gprims and PointInstancer instances to hide or
 * replace by budget boxes so that what Storm draws stays under a polygon limit.
 *
 * A unit is a gprim, or one instance of a PointInstancer. Its distance is from the eye to its
 * bounding sphere, zero inside. In order:
 * - beyond Cull Distance a unit is culled;
 * - beyond Fraction Distance an instancer keeps a hashed Fraction Percent of its instances, the
 *   rest being budget boxes, and a mesh keeps that fraction of its faces;
 * - the faces of the units left whose sphere meets the frustum, or of all of them when Count Whole
 *   Scene is on, are counted, and the farthest are demoted to budget boxes until the count fits.
 * Only drawn, loaded units change; a boxed, hidden or unloaded one counts nothing and is never in a
 * result. The hashes depend on the path and the instance or face index alone, so the kept subset
 * never changes with the camera.
 *
 * Near each boundary the previous result holds: a unit within 5% of the demotion, cull or fraction
 * distance keeps its previous decision, and the previous demotion distance stays while the count it
 * gives is within 5% of the limit. The count may then exceed the limit by up to 5%.
 */
class SceneViewBudget {
public:
    struct Prototype {
        // In the instancer's space, the prototype's own transform applied.
        PXR_NS::GfVec3f min;
        PXR_NS::GfVec3f max;
        // False when the prototype has no bounds; its instances are then points at their origin.
        bool bounded = false;
        U64 faces = 0;
        PXR_NS::SdfPath path;
    };

    // A PointInstancer's instances as authored. The arrays share the stage's values rather than
    // copying them; an empty orientation or scale array means none authored.
    struct Instances {
        PXR_NS::GfMatrix4d toWorld { 1. };
        std::vector<Prototype> prototypes;
        PXR_NS::VtIntArray protoIndices;
        PXR_NS::VtVec3fArray positions;
        PXR_NS::VtQuathArray orientations;
        // Preferred to orientations when not empty, as the schema has it.
        PXR_NS::VtQuatfArray orientationsf;
        PXR_NS::VtVec3fArray scales;
        // Empty when every instance draws; otherwise false for the ones invisibleIds or
        // inactiveIds turn off.
        std::vector<bool> mask;

        std::size_t size() const { return protoIndices.size(); }

        // Whether the instance draws: its prototype index is valid and the mask keeps it.
        bool draws(std::size_t index) const;

        // The instance's prototype-space to world transform, the prototype's own transform excluded,
        // as GfMatrix4d composes it, row vectors on the left.
        PXR_NS::GfMatrix4d instanceTransform(std::size_t index) const;
    };

    using InstancesPtr = std::shared_ptr<const Instances>;

    enum StateEnum {
        // Full, or the row's own prim of a Children row: the budget may change it.
        eStateDrawn,
        eStateBoxed,
        eStateHidden,
        // An unloaded payload, which draws nothing.
        eStateUnloaded
    };

    struct Unit {
        PXR_NS::SdfPath path;
        // World space.
        PXR_NS::GfVec3f min;
        PXR_NS::GfVec3f max;
        // False with no bounds: a point at min.
        bool bounded = false;
        StateEnum state = eStateDrawn;
        // Mesh faces, curve segments or points; for an instancer, the sum over its drawn instances.
        U64 faces = 0;
        // Whether the faces are a mesh's, which a fraction can omit.
        bool mesh = false;
        // Set on a PointInstancer, whose instances are the units.
        InstancesPtr instances;
        // The nearest native instance above a gprim inside one, which the gprim's demotion hides
        // whole.
        PXR_NS::SdfPath nativeInstance;
    };

    struct Camera {
        PXR_NS::GfVec3d eye { 0. };
        // World to clip space, row vectors on the left.
        PXR_NS::GfMatrix4d viewProjection { 1. };
        bool valid = false;

        bool operator==(const Camera& other) const
        {
            return (valid == other.valid) && (eye == other.eye) && (viewProjection == other.viewProjection);
        }
    };

    struct Settings {
        U64 limit = 20000000;
        bool countWholeScene = false;
        // 0 is off.
        double cullDistance = 0.;
        // 0 is off.
        double fractionDistance = 0.;
        // Of 100.
        double fractionPercent = 25.;
    };

    struct InstancerResult {
        // Each empty when it holds no instance, otherwise one per instance.
        std::vector<bool> demoted;
        std::vector<bool> culled;
        // False for the instances the fraction turns into budget boxes.
        std::vector<bool> kept;
        std::size_t demotedCount = 0;
        std::size_t culledCount = 0;
        std::size_t fractionedCount = 0;
    };

    struct Result {
        bool valid = false;
        std::unordered_set<PXR_NS::SdfPath, PXR_NS::SdfPath::Hash> demoted;
        std::unordered_set<PXR_NS::SdfPath, PXR_NS::SdfPath::Hash> culled;
        // The meshes beyond Fraction Distance, with the share of their faces they keep.
        std::unordered_map<PXR_NS::SdfPath, double, PXR_NS::SdfPath::Hash> keptFaceFraction;
        // Only the instancers with an instance demoted, culled or fractioned.
        std::unordered_map<PXR_NS::SdfPath, InstancerResult, PXR_NS::SdfPath::Hash> instancers;
        // The faces counted before demotion, and those left after it.
        U64 counted = 0;
        U64 drawn = 0;
        U64 limit = 0;
        // Units, an instance being one.
        std::size_t demotedCount = 0;
        std::size_t culledCount = 0;
        // Units are demoted from this distance on; infinite when none is.
        double demoteDistance = std::numeric_limits<double>::infinity();
        // The nearest unit whose faces alone exceed the limit, empty for none. For an instance,
        // the instancer's path and the instance's index.
        PXR_NS::SdfPath aloneOverLimit;
        std::size_t aloneOverLimitInstance = 0;
        bool aloneOverLimitIsInstance = false;

        bool isDemoted(const PXR_NS::SdfPath& path) const;
        bool isCulled(const PXR_NS::SdfPath& path) const;
        bool isInstanceDemoted(const PXR_NS::SdfPath& instancer, std::size_t index) const;
        bool isInstanceCulled(const PXR_NS::SdfPath& instancer, std::size_t index) const;
        // True when no fraction turned the instance into a budget box.
        bool isInstanceKept(const PXR_NS::SdfPath& instancer, std::size_t index) const;
    };

    // An invalid camera gives an invalid result, which changes nothing.
    static Result solve(const std::vector<Unit>& units,
                        const Camera& camera,
                        const Settings& settings,
                        const Result& previous);

    // Whether a fraction keeps the instance, or the mesh's face. The same for any camera.
    static bool keepsInstance(const PXR_NS::SdfPath& instancer, std::size_t index, double percent);
    static bool keepsFace(const PXR_NS::SdfPath& mesh, std::size_t face, double fraction);
    // What those two compute, with the path's seed taken once for a loop over its indices.
    static U64 pathSeed(const PXR_NS::SdfPath& path);
    static bool keeps(U64 seed, std::size_t index, double fraction);
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneViewBudget_h
