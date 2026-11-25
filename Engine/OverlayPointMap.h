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

#ifndef Engine_OverlayPointMap_h
#define Engine_OverlayPointMap_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/RectD.h"
#include "Engine/ViewIdx.h"
#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Mapping overlay points through the nodes between the node that owns an overlay and the
 * node the viewer shows. All points are canonical coordinates.
 *
 * A forward map takes a point in an input's space to the node's output space, which is where an
 * upstream overlay must be drawn. An inverse map takes an output point back to the input, which is
 * what a pen event over the viewer needs.
 **/
namespace OverlayPointMap {
enum Direction {
    eDirectionForward,
    eDirectionInverse,
};

enum Result {
    eMapped,
    // The point is unchanged: the node does not move pixels between this input and its output.
    eIdentity,
    // The point has no image under this map (outside a lens domain, a singular matrix, a failed
    // numeric inverse). Polylines break there.
    eUnmappable,
    // The map depends on an image still being fetched; the node emits overlayMapChanged when it
    // is ready.
    ePending,
};

enum Kind {
    eKindNone,
    eKindMatrix,
    eKindNonLinear,
};

typedef std::function<Result(const Point& in, Point* out)> PointFn;

inline bool
isUsable(Result r)
{
    return (r == eMapped) || (r == eIdentity);
}

Direction opposite(Direction direction);

/**
 * @brief Applies a 3x3 homogeneous matrix. Unmappable where the point lands at infinity or the
 * result is not finite.
 **/
Result applyMatrix(const Transform::Matrix3x3& m, const Point& in, Point* out);

struct InvertOptions {
    int maxIterations = 20;
    // Convergence threshold on the residual |fn(p) - target|, in canonical units.
    double tolerance = 1e-6;
    // When Newton from the seed fails, the best point of a grid over this region at this step
    // seeds a second attempt. An empty region skips the fallback.
    RectD seedRegion;
    double seedStep = 0.;
};

/**
 * @brief Finds p with fn(p) = target by damped Newton with a central-difference Jacobian, starting
 * at seed. Pending if fn is pending, unmappable when no attempt converges.
 **/
Result invertNumerically(const PointFn& fn, const Point& target, const Point& seed, Point* out, const InvertOptions& options = InvertOptions());

/**
 * @brief Row-major points over region from (x1, y1) to (x2, y2) inclusive, step apart, the last
 * row and column clamped onto the far edges.
 **/
std::vector<Point> sampleGrid(const RectD& region, double step);

/**
 * @brief Maps a polyline through fn into runs of mapped points, subdividing each segment until the
 * mapped midpoint lies within tolerancePx of the mapped chord, to a depth of 8. viewportScale is
 * screen pixels per canonical unit of the mapped space. A run ends at an unmappable sample.
 * seededFn receives the previous sample's mapped point as a seed for numeric inverses.
 **/
typedef std::function<Result(const Point& in, const Point& seed, Point* out)> SeededPointFn;

Result mapPolyline(const SeededPointFn& fn,
                   const std::vector<Point>& points,
                   bool closed,
                   double tolerancePx,
                   double viewportScale,
                   std::vector<std::vector<Point>>* runs);

/**
 * @brief A regular grid over region holding, at each cell, the value of the scattered sample that
 * fell nearest its centre, for seeding the numeric inverse of a map known only forward. fillHoles()
 * gives every empty cell its nearest filled neighbour's value.
 **/
class SeedGrid {
public:
    SeedGrid(const RectD& region, int nx, int ny);

    void add(const Point& at, const Point& value);

    void fillHoles();

    // The value of the cell under at, clamped onto the grid; false while the grid is empty.
    bool lookup(const Point& at, Point* value) const;

private:
    bool cellOf(const Point& at, bool clamp, int* i, int* j) const;

    RectD _region;
    int _nx;
    int _ny;
    std::vector<Point> _values;
    std::vector<double> _distances;
    bool _any = false;
};

struct Leg {
    EffectInstanceWPtr effect;
    int inputNb = 0;
    Direction direction = eDirectionForward;
};

/**
 * @brief The legs between an overlay's node and the viewed node, applied in order. A forward chain
 * lists the nodes from upstream to downstream; an inverse chain lists them downstream first.
 **/
class OverlayMapChain {
public:
    void append(const EffectInstancePtr& effect, int inputNb, Direction direction);

    const std::vector<Leg>& legs() const
    {
        return _legs;
    }

    bool empty() const
    {
        return _legs.empty();
    }

    // The same legs reversed and each leg's direction flipped.
    OverlayMapChain inverted() const;

    /**
     * @brief Folds in through every leg. A pending leg passes the point through unchanged so the
     * remaining legs still start their own fetches, and the whole result is pending; an
     * unmappable leg ends the fold. seeds, when given, holds one seed per leg and receives each
     * leg's output.
     **/
    Result mapPoint(double time, ViewIdx view, const Point& in, Point* out, std::vector<Point>* seeds = 0) const;

    Result mapPolyline(double time,
                       ViewIdx view,
                       const std::vector<Point>& points,
                       bool closed,
                       double tolerancePx,
                       double viewportScale,
                       std::vector<std::vector<Point>>* runs) const;

private:
    std::vector<Leg> _legs;
};
} // namespace OverlayPointMap

NATRON_NAMESPACE_EXIT

#endif // Engine_OverlayPointMap_h
