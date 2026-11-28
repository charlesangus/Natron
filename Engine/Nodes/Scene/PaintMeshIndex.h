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

#ifndef Engine_Nodes_Scene_PaintMeshIndex_h
#define Engine_Nodes_Scene_PaintMeshIndex_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A ray-tracing index over the meshes a texture is painted on, for placing paint on them
 * directly: the surface under a point of the view, the triangles near a point, and whether points
 * are seen from the view.
 *
 * It holds two kinds of mesh. Targets take paint and are what raycast() and trianglesInSphere()
 * report. Occluders only hide what lies behind them from the view, as every drawn mesh of the stage
 * hides texels from TextureProjection. A target hides what lies behind it too.
 *
 * Built once, the index is only read, from any number of threads at once.
 **/
class PaintMeshIndex {
public:
    // What an index read from a stage was read from, to tell whether it must be read again.
    struct Key {
        std::uint64_t sceneHash = 0;
        std::vector<PXR_NS::SdfPath> meshes;
        PXR_NS::TfToken stName;
        PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default();

        bool operator==(const Key& other) const
        {
            return (sceneHash == other.sceneHash) && (meshes == other.meshes) && (stName == other.stName) && (time == other.time);
        }

        bool operator!=(const Key& other) const
        {
            return !(*this == other);
        }
    };

    struct Hit {
        // Indices into targets() and that mesh's triangles.
        int mesh = -1;
        int triangle = -1;
        // The weights of the triangle's corners 0, 1 and 2 at the hit; they sum to 1.
        PXR_NS::GfVec3f barycentrics = PXR_NS::GfVec3f(0.f);
        PXR_NS::GfVec3f position = PXR_NS::GfVec3f(0.f);
        // The triangle's normal, out of its front side, whichever side the ray came from.
        PXR_NS::GfVec3f normal = PXR_NS::GfVec3f(0.f);
        // The st at the hit and its UDIM tile (SceneRaster::udimTileOf); (0, 0) and 0 when the
        // triangle has no st.
        PXR_NS::GfVec2f st = PXR_NS::GfVec2f(0.f);
        bool hasSt = false;
        int udimTile = 0;
        // From the ray's origin, in world units.
        float distance = 0.f;
    };

    struct TriangleRef {
        int mesh = -1;
        int triangle = -1;

        bool operator==(const TriangleRef& other) const
        {
            return (mesh == other.mesh) && (triangle == other.triangle);
        }

        bool operator<(const TriangleRef& other) const
        {
            return (mesh < other.mesh) || ((mesh == other.mesh) && (triangle < other.triangle));
        }
    };

    PaintMeshIndex();
    ~PaintMeshIndex();

    PaintMeshIndex(const PaintMeshIndex&) = delete;
    PaintMeshIndex& operator=(const PaintMeshIndex&) = delete;

    /**
     * @brief Replaces the index with one over targets and occluders. The build runs on at most
     * threads threads, or Natron's budget for an effect when threads is 0 or less. False, with
     * *errorMessage set, when the ray tracer fails; the index is then empty.
     **/
    bool build(std::vector<SceneRaster::TriangulatedMesh> targets,
               std::vector<SceneRaster::TriangulatedMesh> occluders,
               int threads,
               std::string* errorMessage) WARN_UNUSED_RETURN;

    /**
     * @brief Replaces the index with one over the stage at key.time: the targets are key.meshes,
     * read with their key.stName, and the occluders every other drawn mesh of the stage
     * (SceneRaster::drawnMeshes). SceneRaster::triangulateMeshes' errors, and build()'s.
     **/
    bool buildFromStage(const PXR_NS::UsdStageRefPtr& stage,
                        const Key& key,
                        int threads,
                        std::string* errorMessage) WARN_UNUSED_RETURN;

    // Whether the index was last built by buildFromStage() with key, and that build succeeded.
    bool isBuiltFrom(const Key& key) const;

    void clear();

    const std::vector<SceneRaster::TriangulatedMesh>& targets() const;
    const std::vector<SceneRaster::TriangulatedMesh>& occluders() const;

    /**
     * @brief The nearest surface the camera sees at (u, v) of its view, (0, 0) at the bottom-left
     * of its window and (1, 1) at the top-right, from its near to its far plane. False when that
     * ray meets nothing, or meets an occluder first.
     **/
    bool raycast(const PXR_NS::GfFrustum& camera, double u, double v, Hit* hit) const;

    // The nearest surface along a ray from origin within [tNear, tFar] of the unit direction.
    bool raycast(const PXR_NS::GfVec3f& origin,
                 const PXR_NS::GfVec3f& direction,
                 float tNear,
                 float tFar,
                 Hit* hit) const;

    /**
     * @brief Every target triangle some point of which lies within radius of centre, ordered by
     * mesh then triangle.
     **/
    std::vector<TriangleRef> trianglesInSphere(const PXR_NS::GfVec3f& centre, float radius) const;

    /**
     * @brief For each of count points, whether the camera sees it, as TextureProjection decides
     * it: in front of the camera and within its window, facing it when normals is not null, and
     * with no surface between the point and the near plane closer to the camera than the point's
     * distance times (1 - bias). A bias below kMinimumBias counts as kMinimumBias, so that the
     * surface a point lies on does not hide it.
     **/
    void visible(const PXR_NS::GfVec3f* points,
                 const PXR_NS::GfVec3f* normals,
                 std::size_t count,
                 const PXR_NS::GfFrustum& camera,
                 double bias,
                 std::uint8_t* visible) const;

    static constexpr double kMinimumBias = 1e-5;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_PaintMeshIndex_h
