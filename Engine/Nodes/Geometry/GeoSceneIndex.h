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

#ifndef Engine_Nodes_Geometry_GeoSceneIndex_h
#define Engine_Nodes_Geometry_GeoSceneIndex_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <mutex>

#include "Engine/Nodes/Geometry/GeoUsdConvert.h"

#ifdef emit
#define NATRON_GEOSCENEINDEX_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/tf/refPtr.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/usd/sdf/path.h>
#ifdef NATRON_GEOSCENEINDEX_RESTORE_EMIT
#undef NATRON_GEOSCENEINDEX_RESTORE_EMIT
#define emit
#endif

NATRON_NAMESPACE_ENTER

class GeoSceneIndex;
typedef PXR_NS::TfRefPtr<GeoSceneIndex> GeoSceneIndexRefPtr;

/**
 * @brief A Hydra scene index serving a GeoUsdConvert's meshes, curves and points directly, with
 * no Sdf layer or UsdStage in between.
 *
 * It holds the prims the convert's natrongeo layer would hold once composed and read by
 * UsdImaging: one mesh, basisCurves or points prim per partition at the convert's path, its
 * GeomSubsets as geomSubset child prims, and the shared ancestors as typeless prims. Each prim
 * sits at the convert's path under the prefix, and its primOrigin scenePath is the convert's path,
 * which is where the staged path reported it. Packed geometry partitions (instancers, native
 * instances, overrides of packed USD) are left out.
 *
 * The data is already flattened, since renderer plugins see it after UsdImaging's flattening:
 * xform carries the world matrix with resetXformStack, and visibility and purpose the inherited
 * values. Primvar values are the convert's own arrays, shared rather than copied. A prim's data
 * sources are built on its first GetPrim(), which builds its partition's values.
 *
 * update() swaps the served convert in one step and then sends the narrowest notices that turn
 * the previous prims into the new ones (see update()). Hydra reads GetPrim() from its sync
 * threads, so update() runs where SceneDrawFilter's notices do: under the viewer lock, between
 * renders.
 **/
class GeoSceneIndex final
    : public PXR_NS::HdSceneIndexBase {
public:
    /** @brief An empty scene index; @p prefix must be an absolute prim path or the root. */
    static GeoSceneIndexRefPtr New(const PXR_NS::SdfPath& prefix = PXR_NS::SdfPath::AbsoluteRootPath());

    ~GeoSceneIndex() override;

    const PXR_NS::SdfPath& prefix() const
    {
        return _prefix;
    }

    /** @brief The convert served now, null when none is. */
    std::shared_ptr<const GeoUsdConvert> convert() const;

    /** @brief Where the prim for the convert's @p convertPath sits in this scene index. */
    PXR_NS::SdfPath indexPath(const PXR_NS::SdfPath& convertPath) const;

    /**
     * @brief Serves @p next instead of the current convert. It first adopts the current convert's
     * unchanged partitions, so call it before any field query on @p next. Null serves nothing.
     *
     * Notices: a prim whose path is new, or whose type changed, is removed (if it was there) and
     * added again; so is everything under a removed prim, since Hydra drops the whole subtree. A
     * prim on the same path with the same type whose partition kept its signature sends nothing.
     * Otherwise it is dirtied at the locators of what differs: primvars/<name> per primvar,
     * mesh/topology or basisCurves/topology for the topology, extent, geomSubset, and xform,
     * visibility or purpose when the flattened value moved. An identical detail sends nothing.
     **/
    void update(std::shared_ptr<GeoUsdConvert> next);

    PXR_NS::HdSceneIndexPrim GetPrim(const PXR_NS::SdfPath& primPath) const override;
    PXR_NS::SdfPathVector GetChildPrimPaths(const PXR_NS::SdfPath& primPath) const override;

private:
    struct Prim;
    struct Snapshot;

    explicit GeoSceneIndex(const PXR_NS::SdfPath& prefix);

    std::shared_ptr<const Snapshot> snapshot() const;

    PXR_NS::SdfPath _prefix;
    mutable std::mutex _mutex;
    std::shared_ptr<const Snapshot> _snapshot;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoSceneIndex_h
