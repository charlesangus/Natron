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

#ifndef Engine_Nodes_Scene_Utility_UtilityScene_h
#define Engine_Nodes_Scene_Utility_UtilityScene_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/usd/sdf/path.h>

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

struct UtilityMaterialDesc;
struct UtilityLightDesc;
class UtilityTexturePool;

/**
 * @brief What the utility renderer draws, in plain arrays: one geometry record per mesh and one
 * instance per placement of it. Hydra's prims fill it in as they sync and a render reads a snapshot,
 * rebuilding its own acceleration structures from the version numbers. MT-safe.
 **/
class UtilityScene {
public:
    typedef std::uint64_t GeometryId;

    // Which element Geometry::colors holds one value for.
    enum ColorBinding {
        eColorBindingConstant,
        eColorBindingTriangle,
        eColorBindingVertex,
        eColorBindingCorner, // three per triangle, in triangle order
    };

    // A prim's motion within the shutter: its values at times relative to the frame, increasing,
    // at least two of them.
    struct PointMotion {
        std::vector<double> times;
        std::vector<PXR_NS::VtVec3fArray> points; // one per time, each as many as the geometry's points
    };

    struct TransformMotion {
        std::vector<double> times;
        std::vector<std::vector<PXR_NS::GfMatrix4d>> transforms; // one per time, each one per instance
    };

    struct Geometry {
        PXR_NS::SdfPath path;
        int primId = -1;
        PXR_NS::VtVec3fArray points;
        PXR_NS::VtVec3iArray triangles;
        // Straight RGBA: displayColor with displayOpacity in alpha.
        PXR_NS::VtVec4fArray colors;
        ColorBinding colorBinding = eColorBindingConstant;
        // Empty when no material is bound.
        PXR_NS::SdfPath materialId;
        // The bound material's texture coordinates, bound like the colours; hasSt is false when the
        // mesh has none of that name.
        PXR_NS::VtVec2fArray st;
        ColorBinding stBinding = eColorBindingConstant;
        bool hasSt = false;
        // The light- and shadow-link categories the prim belongs to, and those of its instancer.
        PXR_NS::TfTokenVector categories;
        // Each counts the changes to that part, so a render can tell a refit from a rebuild.
        std::uint64_t topologyVersion = 0;
        std::uint64_t pointsVersion = 0;
        std::uint64_t colorVersion = 0;
        std::uint64_t stVersion = 0;
        // Null for a geometry that does not deform within the shutter; points are at the frame.
        std::shared_ptr<const PointMotion> pointMotion;
    };

    struct Instance {
        GeometryId geometry = 0;
        int instanceIndex = 0;
        PXR_NS::GfMatrix4d transform = PXR_NS::GfMatrix4d(1.);
        // The link categories of this placement alone; the geometry's own apply to every placement.
        PXR_NS::TfTokenVector categories;
        // Null for an instance that does not move within the shutter; instanceIndex indexes its
        // transforms, and transform is at the frame.
        std::shared_ptr<const TransformMotion> motion;
    };

    struct Stats {
        std::size_t geometryCount = 0;
        std::size_t instanceCount = 0;
        std::size_t triangleCount = 0;
        std::size_t topologyBuilds = 0;
        std::size_t pointUpdates = 0;
        std::size_t colorUpdates = 0;
        std::size_t version = 0;
        // The geometries whose topology changes within the shutter, so they do not deform there.
        std::vector<PXR_NS::SdfPath> topologyVaryingPaths;
    };

    struct Snapshot {
        std::uint64_t version = 0;
        std::map<GeometryId, Geometry> geometries;
        std::vector<Instance> instances;
        std::map<PXR_NS::SdfPath, std::shared_ptr<const UtilityMaterialDesc>> materials;
        std::map<PXR_NS::SdfPath, std::shared_ptr<const UtilityLightDesc>> lights;
        std::shared_ptr<UtilityTexturePool> textures;
    };

    UtilityScene();
    ~UtilityScene();

    GeometryId addGeometry(const PXR_NS::SdfPath& path,
                           int primId);
    void removeGeometry(GeometryId id);
    void setPrimId(GeometryId id,
                   int primId);
    void setTopology(GeometryId id,
                     const PXR_NS::VtVec3fArray& points,
                     const PXR_NS::VtVec3iArray& triangles);
    void setPoints(GeometryId id,
                   const PXR_NS::VtVec3fArray& points);
    void setColors(GeometryId id,
                   ColorBinding binding,
                   const PXR_NS::VtVec4fArray& colors);
    // One instance per transform, replacing the geometry's previous ones. categories, when not
    // empty, holds each instance's own link categories.
    void setInstances(GeometryId id,
                      const std::vector<PXR_NS::GfMatrix4d>& transforms,
                      const std::vector<PXR_NS::TfTokenVector>& categories = std::vector<PXR_NS::TfTokenVector>());
    void setMaterialId(GeometryId id,
                       const PXR_NS::SdfPath& materialId);
    void setSt(GeometryId id,
               bool hasSt,
               ColorBinding binding,
               const PXR_NS::VtVec2fArray& st);
    void setMaterial(const PXR_NS::SdfPath& path,
                     const std::shared_ptr<const UtilityMaterialDesc>& material);
    void removeMaterial(const PXR_NS::SdfPath& path);
    void setCategories(GeometryId id,
                       const PXR_NS::TfTokenVector& categories);
    void setLight(const PXR_NS::SdfPath& path,
                  const std::shared_ptr<const UtilityLightDesc>& light);
    void removeLight(const PXR_NS::SdfPath& path);
    // The camera that camera-space lights follow, as its view-to-world matrix.
    void setViewToWorld(const PXR_NS::GfMatrix4d& viewToWorld);
    void setRenderTag(GeometryId id,
                      const PXR_NS::TfToken& tag);
    // The render tags whose geometries contribute instances to snapshots and stats, all of them when
    // empty as for Hydra. An excluded geometry stays in snapshots, so showing it again builds nothing.
    void setRenderTags(const PXR_NS::TfTokenVector& tags);

    // Bumps on any change to the scene.
    std::uint64_t version() const;
    Stats stats() const;
    Snapshot snapshot() const;
    // The textures every snapshot's materials read from.
    const std::shared_ptr<UtilityTexturePool>& textures() const;
    /**
     * @brief What the scene draws other than as authored, one line per problem: the unsupported
     * materials bound to visible geometry, textured geometry without texture coordinates, textures
     * that could not be read, and IES profiles that could not be parsed.
     **/
    std::vector<std::string> warnings() const;

    // Null for none. The transform motion has a transform per instance setInstances() gave.
    void setPointMotion(GeometryId id,
                        const std::shared_ptr<const PointMotion>& motion);
    void setTransformMotion(GeometryId id,
                            const std::shared_ptr<const TransformMotion>& motion);
    // Whether the geometry's topology changes within the shutter, for stats to name it.
    void setTopologyVaries(GeometryId id,
                           bool varies);

    // Whether any of the snapshot's instances or geometries moves within the shutter.
    static bool moves(const Snapshot& snapshot);

    /**
     * @brief The snapshot as it stands at time, relative to the frame: each moving instance's
     * transform and each deforming geometry's points interpolated between their samples, held at
     * the first or last outside them, and none of it moving. Its version, and each deforming
     * geometry's pointsVersion, differ from every scene snapshot's, so a tracer handed it refits
     * what deforms and moves what moves.
     **/
    static Snapshot evaluateAt(const Snapshot& snapshot,
                               double time);

    // Between a and b by u in [0, 1]: translation and stretch linearly, rotation along the shortest arc.
    static PXR_NS::GfMatrix4d interpolate(const PXR_NS::GfMatrix4d& a,
                                          const PXR_NS::GfMatrix4d& b,
                                          double u);

private:
    struct Record {
        Geometry geometry;
        std::vector<PXR_NS::GfMatrix4d> transforms;
        std::vector<PXR_NS::TfTokenVector> instanceCategories;
        std::shared_ptr<const TransformMotion> transformMotion;
        PXR_NS::TfToken renderTag;
        bool topologyVaries = false;
    };

    bool isIncluded(const Record& record) const;

    mutable std::mutex _mutex;
    GeometryId _nextId = 1;
    std::map<GeometryId, Record> _records;
    PXR_NS::TfTokenVector _renderTags;
    std::map<PXR_NS::SdfPath, std::shared_ptr<const UtilityMaterialDesc>> _materials;
    std::map<PXR_NS::SdfPath, std::shared_ptr<const UtilityLightDesc>> _lights;
    std::shared_ptr<UtilityTexturePool> _textures;
    PXR_NS::GfMatrix4d _viewToWorld = PXR_NS::GfMatrix4d(1.);
    std::uint64_t _version = 0;
    std::size_t _topologyBuilds = 0;
    std::size_t _pointUpdates = 0;
    std::size_t _colorUpdates = 0;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityScene_h
