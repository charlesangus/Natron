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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpField_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpField_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCache.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

class SplineWarpMesh;
class SplineWarpTps;

/**
 * @brief Where SplineWarpField::getGpuTables() puts each table, in 32-bit words from the start.
 **/
struct SplineWarpFieldGpuLayout {
    uint32_t nodes = 0;
    uint32_t columns = 0;
    uint32_t rows = 0;
    // Both 0 without boundaries.
    uint32_t feathers = 0;
    uint32_t featherStride = 0;
    // Grid points per grid row.
    uint32_t nx = 0;
    // The grid for a scattered lookup: its rows, its first lattice index, the lattice step and
    // the canonical-to-pixel scales, as cellOf() and mapPoints() use them.
    uint32_t ny = 0;
    int i0 = 0;
    int j0 = 0;
    double step = 1.;
    double scaleX = 1.;
    double scaleY = 1.;
    // The window the column, row and feather tables cover, in pixels at the render scale.
    RectI window;
};

/**
 * @brief A warp's source coordinates over a render window: the solved map evaluated on a grid,
 * bilinear between grid points, with its Jacobian from the grid's central differences.
 *
 * The grid points lie on a lattice of step fieldResolution canonical pixels anchored at the
 * canonical origin, so a pixel's coordinate is the same whatever window it is rendered in and at
 * any render scale. The grid covers the window plus a margin of grid points.
 *
 * With boundaries, the displacement at a grid point is weighted: 1 inside a boundary's curve,
 * easing to 0 at its feather along the Roto feather fall-off profile, 0 outside every feather; the
 * largest weight of the boundaries wins. A pixel outside every feather is the identity exactly.
 *
 * Immutable once built, so it is read from any thread.
 **/
class SplineWarpField {
public:
    /**
     * @brief The identity on an empty window.
     **/
    SplineWarpField();

    /**
     * @param window Pixels at the render scale.
     * @param scaleX, scaleY Pixels per canonical unit along each axis.
     * @param fieldResolution The grid step, in canonical pixels.
     **/
    SplineWarpField(const SplineWarpTps& map,
                    const std::vector<SplineWarpBoundaryShape>& boundaries,
                    const RectI& window,
                    double scaleX,
                    double scaleY,
                    double fieldResolution);

    /**
     * @brief The same over a mesh: its triangles are scan-converted onto the grid.
     **/
    SplineWarpField(const SplineWarpMesh& mesh,
                    const std::vector<SplineWarpBoundaryShape>& boundaries,
                    const RectI& window,
                    double scaleX,
                    double scaleY,
                    double fieldResolution);

    /**
     * @brief What the field built over window gives, taken from from without solving again; from's
     * window must hold window. Grid points sit on the same lattice whatever the window, so their
     * sources are copied and the Jacobians worked out from them as a solve over window works them
     * out, one-sided at the same grid edge, so the field is the one that solve gives, bit for bit.
     **/
    SplineWarpField(const SplineWarpField& from,
                    const RectI& window);

    /**
     * @brief Whether every pixel of the window is its own source.
     **/
    bool isIdentity() const
    {
        return _identity;
    }

    const RectI& getWindow() const
    {
        return _window;
    }

    /**
     * @brief A canonical box holding the source of every pixel of the window, grown by half the
     * pixel footprint its Jacobian gives, and the window.
     **/
    RectD getSourceBounds() const;

    /**
     * @brief The largest Resampler::kernelScale() of the pixel Jacobian over the field, per source
     * axis. The Jacobian is bilinear between grid points and a norm is convex, so the grid holds
     * the largest of every pixel's.
     **/
    void getLargestKernelScales(double* scaleX, double* scaleY) const;

    /**
     * @brief The source of canonical point (x, y), which should lie within the window.
     **/
    Point getSource(double x, double y) const;

    /**
     * @brief The Jacobian at canonical point (x, y): Jxx = dsx/dx, Jxy = dsx/dy, Jyx = dsy/dx,
     * Jyy = dsy/dy.
     **/
    void getJacobian(double x, double y, double jacobian[4]) const;

    /**
     * @brief The boundary weight at canonical point (x, y): 1 without boundaries.
     **/
    double getWeight(double x, double y) const;

    /**
     * @brief The sources of the pixels x1 to x2 - 1 of row y, in pixel coordinates at the render
     * scale with pixel centres at +0.5 as Resampler::resampleRowFromCoords() takes them, and, when
     * jacobian is not null, their Jacobians in its layout. The row should lie within the window.
     **/
    void getRow(int y, int x1, int x2, float* sx, float* sy, float* jacobian) const;

    /**
     * @brief Maps the n points (sx[i], sy[i]), pixel coordinates at the render scale, in place to
     * their sources, the value getRow() gives at a pixel centre. When jacobian is not null it
     * holds four doubles per point in getRow()'s layout, each becoming D·J with D the field's
     * pixel Jacobian at the point. A NaN point stays NaN with its Jacobian untouched. Points
     * should lie within the window. With boundaries a point moves when the centre of the pixel
     * containing it lies inside a feather, which is the bit getGpuTables() holds for that pixel,
     * so a point in a window pixel moves as warpchain.slang moves it.
     **/
    void mapPoints(std::size_t n, double* sx, double* sy, double* jacobian) const;

    /**
     * @brief The field over its window as splinewarp.slang reads it to give what getRow() gives:
     * per grid point, 8 words, its source in pixels at the render scale as two-float pairs (hi,
     * lo) for x then y, then its Jacobian in getRow()'s layout; per window column, 3 words, the
     * grid cell and the fraction across it as a pair; the same per window row; and with
     * boundaries, rows of featherStride words holding a bit per pixel, set inside a feather.
     * Empty for an identity field.
     **/
    void getGpuTables(std::vector<uint32_t>* words, SplineWarpFieldGpuLayout* layout) const;

    /**
     * @brief The Roto feather fall-off profile: the weight at fraction t of the way from a curve
     * (1) to its feather (0). A fallOff of 1 is linear; above 1 the weight drops sooner.
     **/
    static double featherRamp(double t, double fallOff);

private:
    // Solves the grid through map, or through mesh when map is null.
    void build(const SplineWarpTps* map, const SplineWarpMesh* mesh);
    // The grid over the window: _i0, _j0, _nx, _ny.
    void layOutGrid();
    // From the grid's sources: whether any moved, and if so their Jacobians.
    void finish();
    // Lattice coordinates of canonical (x, y) relative to the grid's first point, clamped so the
    // cell (ix, iy)-(ix + 1, iy + 1) is in the grid.
    void cellOf(double x, double y, int* ix, int* iy, double* fx, double* fy) const;
    bool insideAnyFeather(double x, double y) const;
    // Whether the window pixel containing the render-scale point (x, y) has its centre inside a
    // boundary's feather; false outside the window.
    bool containingPixelInsideFeathers(double x, double y) const;
    // Whether each pixel x1 to x2 - 1 of row y lies inside a boundary's feather, by the even-odd
    // crossings of the row with each feather, as splineWarpPointInPolygon() decides.
    void rowInsideFeathers(int y, int x1, int x2, std::vector<unsigned char>* inside) const;
    double nodeWeight(double x, double y) const;

    RectI _window;
    double _scaleX;
    double _scaleY;
    double _step;
    bool _identity;
    std::vector<SplineWarpBoundaryShape> _boundaries;
    // Lattice index of the grid's first point.
    int _i0;
    int _j0;
    int _nx;
    int _ny;
    // Per grid point, row-major: the source, the weight, and the Jacobian.
    std::vector<double> _sourceX;
    std::vector<double> _sourceY;
    std::vector<double> _weight;
    std::vector<double> _jacobian;
};

typedef std::shared_ptr<const SplineWarpField> SplineWarpFieldConstPtr;

/**
 * @brief Built fields by solver, warp, render scale and window, shared by every render of a node.
 * A window held by a cached field's window is cut from that field rather than solved again.
 **/
class SplineWarpFieldCache {
public:
    // Frames rendering concurrently each ask for several windows.
    explicit SplineWarpFieldCache(std::size_t capacity = 16)
        : _cache(capacity)
        , _solves(0)
    {
    }

    /**
     * @param warpHash Identifies the map and the boundaries: the sample set's hash combined with
     * whatever else the map was solved with. A field depends on nothing else, so a warp that holds
     * still across frames reuses one field for all of them.
     **/
    SplineWarpFieldConstPtr get(U64 warpHash,
                                const SplineWarpTps& map,
                                const std::vector<SplineWarpBoundaryShape>& boundaries,
                                const RectI& window,
                                double scaleX,
                                double scaleY,
                                double fieldResolution);

    SplineWarpFieldConstPtr get(U64 warpHash,
                                const SplineWarpMesh& mesh,
                                const std::vector<SplineWarpBoundaryShape>& boundaries,
                                const RectI& window,
                                double scaleX,
                                double scaleY,
                                double fieldResolution);

    void clear()
    {
        _cache.clear();
    }

    // The field steps of the cached fields, most recently used first.
    std::vector<double> getResolutions() const;

    // How many fields were solved rather than cut from a cached one.
    std::size_t getSolveCount() const
    {
        return _solves.load();
    }

private:
    struct Key {
        bool mesh;
        U64 hash;
        double scaleX;
        double scaleY;
        double resolution;
        RectI window;

        bool operator==(const Key& o) const
        {
            return sameWarp(o) && window == o.window;
        }

        bool sameWarp(const Key& o) const
        {
            return mesh == o.mesh && hash == o.hash && scaleX == o.scaleX && scaleY == o.scaleY && resolution == o.resolution;
        }
    };

    SplineWarpFieldConstPtr get(const Key& key, const std::function<SplineWarpField()>& solve);

    SplineWarpLruCache<Key, SplineWarpField> _cache;
    std::atomic<std::size_t> _solves;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpField_h
