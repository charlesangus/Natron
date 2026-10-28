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

#include <memory>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCache.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

class SplineWarpTps;

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
     * @brief The Roto feather fall-off profile: the weight at fraction t of the way from a curve
     * (1) to its feather (0). A fallOff of 1 is linear; above 1 the weight drops sooner.
     **/
    static double featherRamp(double t, double fallOff);

private:
    // Lattice coordinates of canonical (x, y) relative to the grid's first point, clamped so the
    // cell (ix, iy)-(ix + 1, iy + 1) is in the grid.
    void cellOf(double x, double y, int* ix, int* iy, double* fx, double* fy) const;
    bool insideAnyFeather(double x, double y) const;
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
 * @brief Built fields by warp, time, render scale and window, shared by every render of a node.
 **/
class SplineWarpFieldCache {
public:
    /**
     * @param warpHash Identifies the map and the boundaries: the sample set's hash combined with
     * whatever else the map was solved with.
     **/
    SplineWarpFieldConstPtr get(U64 warpHash,
                                double time,
                                const SplineWarpTps& map,
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

private:
    struct Key {
        U64 hash;
        double time;
        double scaleX;
        double scaleY;
        double resolution;
        RectI window;

        bool operator==(const Key& o) const
        {
            return hash == o.hash && time == o.time && scaleX == o.scaleX && scaleY == o.scaleY && resolution == o.resolution && window == o.window;
        }
    };

    SplineWarpLruCache<Key, SplineWarpField> _cache;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpField_h
