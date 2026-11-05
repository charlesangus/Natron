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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpMesh_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpMesh_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <utility>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCache.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpTps.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A map from destination positions to source positions through constraint pairs, in
 * canonical coordinates: a constrained Delaunay triangulation of the destinations, each point's
 * source interpolated barycentrically in its triangle.
 *
 * The edges between consecutive samples of every polyline are constraints of the triangulation,
 * and crossing constraints are split where they cross; the new vertex takes its source from the
 * first-inserted edge through it, interpolated along that edge. The corners of a box around
 * everything are vertices that do not move, so the warp eases to nothing at the box, and a point
 * outside the box is its own source.
 *
 * Fewer than three destinations, or destinations on a line, cannot be triangulated: the map is
 * then the thin-plate solver's fallback for them (translation, similarity or affine).
 *
 * Immutable once built, so it is evaluated from any thread.
 **/
class SplineWarpMesh {
public:
    struct Triangle {
        int v[3];
    };

    /**
     * @brief The identity.
     **/
    SplineWarpMesh();

    /**
     * @param polylines Indices into pairs; consecutive indices, and the last and first of a closed
     * polyline, are constrained edges.
     * @param box Its corners are pinned; it is grown to hold every destination.
     **/
    static SplineWarpMesh build(const std::vector<SplineWarpSamplePair>& pairs,
                                const std::vector<SplineWarpSamplePolyline>& polylines,
                                const RectD& box);

    /**
     * @brief The box whose corners a mesh pins: the union of bounds, when it is finite and not
     * empty, and of every destination and source, grown on each side by a quarter of its larger
     * side.
     **/
    static RectD paddedBox(const std::vector<SplineWarpSamplePair>& pairs, const RectD& bounds);

    bool isIdentity() const
    {
        return _triangles.empty() && (!_useFallback || _fallback.isIdentity());
    }

    /**
     * @brief Whether the map is the thin-plate solver's fallback rather than a triangulation.
     **/
    bool usesFallback() const
    {
        return _useFallback;
    }

    const SplineWarpTps& getFallback() const
    {
        return _fallback;
    }

    /**
     * @brief The triangulation's vertices: the merged destinations, the box corners, then the
     * vertices added where constraints cross.
     **/
    const std::vector<Point>& getDestinations() const
    {
        return _destinations;
    }

    const std::vector<Point>& getSources() const
    {
        return _sources;
    }

    const std::vector<Triangle>& getTriangles() const
    {
        return _triangles;
    }

    /**
     * @brief The constrained edges of the triangulation, as vertex index pairs, split where they
     * cross or pass through a vertex.
     **/
    const std::vector<std::pair<int, int>>& getConstrainedEdges() const
    {
        return _constrainedEdges;
    }

    const RectD& getBox() const
    {
        return _box;
    }

    Point evaluate(double x, double y) const;

    /**
     * @brief Writes the source of every point (i0 + i, j0 + j) * step, 0 <= i < nx, 0 <= j < ny,
     * row-major into sourceX and sourceY, each holding nx * ny values.
     **/
    void rasterize(int i0, int j0, int nx, int ny, double step, double* sourceX, double* sourceY) const;

private:
    bool _useFallback;
    SplineWarpTps _fallback;
    RectD _box;
    std::vector<Point> _destinations;
    std::vector<Point> _sources;
    std::vector<Triangle> _triangles;
    std::vector<std::pair<int, int>> _constrainedEdges;
};

typedef std::shared_ptr<const SplineWarpMesh> SplineWarpMeshConstPtr;

/**
 * @brief Built meshes by sample-set hash and box, shared by every render of a node.
 **/
class SplineWarpMeshCache {
public:
    explicit SplineWarpMeshCache(std::size_t capacity = 4)
        : _cache(capacity)
    {
    }

    SplineWarpMeshConstPtr get(U64 sampleHash,
                               const std::vector<SplineWarpSamplePair>& pairs,
                               const std::vector<SplineWarpSamplePolyline>& polylines,
                               const RectD& box);

    void clear()
    {
        _cache.clear();
    }

private:
    struct Key {
        U64 hash;
        double x1, y1, x2, y2;

        bool operator==(const Key& o) const
        {
            return hash == o.hash && x1 == o.x1 && y1 == o.y1 && x2 == o.x2 && y2 == o.y2;
        }
    };

    SplineWarpLruCache<Key, SplineWarpMesh> _cache;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpMesh_h
