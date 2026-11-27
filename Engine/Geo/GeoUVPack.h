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

#ifndef Engine_Geo_GeoUVPack_h
#define Engine_Geo_GeoUVPack_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

typedef std::array<double, 2> GeoUV;

/** @brief An island as its polygons in uv, each a ring of corners. */
struct GeoUVPackIsland {
    std::vector<std::vector<GeoUV>> polygons;
    // Multiplies the packing scale for this island alone.
    double scale = 1.;
    // The target it must go to: a rectangle index, a UDIM number or a target island index; -1
    // lets the packer choose (or use the default target).
    int target = -1;
    // Left where it is. With avoidObstacles the others are packed round it; with
    // stackOnObstacles identical islands may be stacked onto it.
    bool obstacle = false;
    // False for islands that must not be stacked, such as those merged from several.
    bool stackable = true;
};

enum GeoUVPackScaleEnum {
    // As large as every island still fits, searched to scaleTolerance.
    eGeoUVPackScaleLargest = 0,
    // fixedScale; the islands that do not fit are lined up beside the targets.
    eGeoUVPackScaleFixed,
};

enum GeoUVPackTargetTypeEnum {
    eGeoUVPackTargetRectangles = 0,
    eGeoUVPackTargetUDIM,
    eGeoUVPackTargetIslands,
};

struct GeoUVPackRect {
    double x0 = 0.;
    double y0 = 0.;
    double x1 = 1.;
    double y1 = 1.;
};

struct GeoUVPackParams {
    GeoUVPackScaleEnum scaleMode = eGeoUVPackScaleLargest;
    double fixedScale = 1.;
    // The rotations tried, as divisions of a full turn: 1 tries none, 4 every 90 degrees.
    int rotationDivisions = 4;
    // Off, each island is packed as if its holes and hollows were filled.
    bool packInCavities = true;
    // The least gap between islands, in pixels of the search resolution.
    int padding = 1;
    // Also keeps the padding from the targets' edges.
    bool padBoundaries = false;
    // Widens the padding as far as the same islands still fit, spreading them over the targets.
    bool spread = false;
    // Packings tried at each scale, each in its own random island order; they run in parallel.
    int iterations = 1;
    // Pixels along the longest side of the largest target (of a tile, for UDIM).
    int resolution = 1024;
    // The largest scale is searched until a step smaller than this fraction of it.
    double scaleTolerance = 0.005;
    std::uint32_t seed = 5489;

    GeoUVPackTargetTypeEnum targetType = eGeoUVPackTargetRectangles;
    std::vector<GeoUVPackRect> rectangles = { GeoUVPackRect() };
    double tileSize = 1.;
    int columns = 10;
    int startingUdim = 1001;
    std::vector<GeoUVPackIsland> targetIslands;
    // The target an island without one goes to; -1 to let the packer choose.
    int defaultTarget = -1;

    bool avoidObstacles = false;
    bool stackIdentical = false;
    bool stackMirrored = false;
    bool stackOnObstacles = false;
    // How far apart, as a fraction of an island's bounding diagonal, matching corners may be.
    double stackTolerance = 0.01;

    // Polled throughout; returning true stops the packing as soon as it can.
    std::function<bool()> aborted;
};

/** @brief Where an island went: (u, v) maps to (m[0] u + m[1] v + t[0], m[2] u + m[3] v + t[1]). */
struct GeoUVPackPlacement {
    std::array<double, 4> m = { 1., 0., 0., 1. };
    GeoUV t = { 0., 0. };
    bool packed = false;
    // The target it was packed into, as GeoUVPackIsland::target names one; -1 when not packed.
    int target = -1;
    // The island it was stacked onto, or -1.
    int stackedOn = -1;
};

struct GeoUVPackResult {
    // By island; obstacles keep the identity.
    std::vector<GeoUVPackPlacement> placements;
    double scale = 1.;
    int padding = 0;
    // The islands that did not fit and were lined up beside the targets.
    int nonpacked = 0;
    // The highest UDIM used, for the warning past 1100; 0 for other targets.
    int maxUdim = 0;
    bool aborted = false;
};

GeoUV geoUVPackApply(const GeoUVPackPlacement& placement,
                     const GeoUV& uv);

/**
 * @brief Packs the islands into the targets by rasterising them at the search resolution and
 * placing each, largest first, at the lowest place it fits, trying every rotation and target.
 * Iterations repeat the packing in random orders seeded from seed, in parallel, keeping the one
 * that packs the most and then stays lowest. Identical (and with stackMirrored, mirrored)
 * islands are stacked onto the first of them instead of being packed, when that first is an
 * obstacle or has the same scale. When no scale fits every island, the largest scale that fit the
 * most is kept, the search stopping once the islands are smaller than a cell. Fails, with *errorMessage
 * set, for an empty target, a resolution below 1 or a scale that is not positive; fails with
 * result->aborted set and no message when params.aborted stopped it.
 **/
bool geoPackUVIslands(const std::vector<GeoUVPackIsland>& islands,
                      const GeoUVPackParams& params,
                      GeoUVPackResult* result,
                      std::string* errorMessage);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoUVPack_h
