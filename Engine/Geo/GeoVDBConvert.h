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

#ifndef Engine_Geo_GeoVDBConvert_h
#define Engine_Geo_GeoVDBConvert_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <vector>

#include <openvdb/version.h>

#include "Global/GlobalDefines.h"

#include "Engine/Geo/GeoVDB.h"
#include "Engine/GeoDetail.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
    namespace math {
        class Transform;
    }
}
}

NATRON_NAMESPACE_ENTER

/**
 * Conversions between native geometry and VDB primitives, over OpenVDB's tools. Distances, band
 * widths and radii given in voxels are multiples of the voxel size; the rest are world units.
 * Grids are made with a uniform linear transform whose index-space origin is the world origin.
 **/

enum GeoVDBVolumeKindEnum {
    // A narrow-band signed distance field, negative inside (grid class level set).
    eGeoVDBVolumeSDF = 0,
    // A density from 0 outside to 1 inside, ramping across the inner band (grid class fog volume).
    eGeoVDBVolumeFog,
};

enum GeoVDBAxisEnum {
    eGeoVDBAxisX = 0,
    eGeoVDBAxisY,
    eGeoVDBAxisZ,
    eGeoVDBAxisLongest,
};

enum GeoVDBInterpolationEnum {
    eGeoVDBInterpolationPoint = 0,
    eGeoVDBInterpolationLinear,
    eGeoVDBInterpolationQuadratic,
};

// Owns its grid, unlike GeoVDBNamedGrid, because a result outlives the conversion that made it.
struct GeoVDBConvertGrid {
    std::string name;
    GeoVDBGridBaseConstPtr grid;
};

struct GeoVDBConvertResult {
    // The distance or density grid first, then one grid per rasterised attribute.
    std::vector<GeoVDBConvertGrid> grids;
    // What the node's info shows: what was skipped and why, and the memory the grids use.
    std::vector<std::string> notes;
};

/** @brief Appends one VDB primitive per grid of @p result, in order. Returns the first one's offset. */
GeoOffset geoAppendVDBGrids(GeoDetailBuilder& builder,
                            const GeoVDBConvertResult& result);

/**
 * @brief The voxel size that fits @p count voxels along @p axis of the bounds of @p detail's live
 * points; 0 when the detail has no point, the count is below 1 or the extent along the axis is 0.
 **/
double geoVoxelSizeForCount(const GeoDetail& detail,
                            int count,
                            GeoVDBAxisEnum axis);

struct GeoPolygonsToSdfParams {
    double voxelSize = 0.1;
    // Voxels.
    double exteriorBand = 3.;
    double interiorBand = 3.;
    // Distances fill the whole inside instead of stopping at the interior band.
    bool fillInterior = false;
    GeoVDBVolumeKindEnum kind = eGeoVDBVolumeSDF;
    std::string name = "surface";
    // Space-separated globs, ^ excluding, over point, vertex and primitive attribute names; each
    // match becomes a grid of the band's voxels holding the value at the closest surface point.
    // A vertex attribute wins over a point or primitive attribute of the same name, and a point
    // attribute over a primitive one. Empty for none.
    std::string attributes;
};

/**
 * @brief Rasterises the live polygons of @p source (fan-triangulated) into a distance or density
 * grid named params.name, plus the attribute grids. A mesh with boundary edges is not closed and
 * has no inside, so as an SDF it gets an unsigned distance band and a note. Polylines, packed and
 * VDB primitives are ignored. A source without polygons gives an empty grid. False with @p error
 * set for a voxel size that is not positive and finite or a negative band.
 *
 * Numeric attributes of one or three components convert: float to a float or vector grid, int
 * to an int32 grid taking the value of the closest corner. Others are skipped with a note.
 **/
bool geoPolygonsToSdf(const GeoDetail& source,
                      const GeoPolygonsToSdfParams& params,
                      GeoVDBConvertResult* result,
                      std::string* error);

struct GeoPointsToSdfParams {
    double voxelSize = 0.1;
    // A sphere's radius is the point's pscale (1 without one) times this.
    double radiusScale = 1.;
    // Voxels: smaller spheres are skipped and counted in a note.
    double minRadius = 1.5;
    // Half the width of the narrow band, in voxels.
    double bandWidth = 3.;
    // Each sphere is stretched back along -v * velocityScale by spheres shrinking to the minimum
    // radius, trailSpacing radii apart.
    bool trails = false;
    double velocityScale = 1.;
    double trailSpacing = 1.;
    GeoVDBVolumeKindEnum kind = eGeoVDBVolumeSDF;
    std::string name = "surface";
    // Patterns over point attribute names, as GeoPolygonsToSdfParams::attributes; each voxel
    // takes the value of the sphere whose surface is closest.
    std::string attributes;
};

/**
 * @brief Unions spheres on the live points of @p source into a distance or density grid named
 * params.name, plus the attribute grids. A source without points gives an empty grid. False
 * with @p error set for a voxel size that is not positive and finite, or a band, minimum radius
 * or trail spacing that is not positive.
 **/
bool geoPointsToSdf(const GeoDetail& source,
                    const GeoPointsToSdfParams& params,
                    GeoVDBConvertResult* result,
                    std::string* error);

struct GeoSdfToPolygonsParams {
    // The surface level: 0 for an SDF, typically 0.5 for a fog volume.
    double isovalue = 0.;
    // 0 keeps every polygon voxel-sized; towards 1, flat regions are merged into larger ones.
    double adaptivity = 0.;
    // With a reference, the surface's points copy the point attributes matching this pattern
    // (see GeoPolygonsToSdfParams::attributes) from the reference's closest point.
    std::string referenceAttributes = "*";
};

/**
 * @brief Appends the isosurface of the scalar grid @p grid to @p out as quads and triangles on
 * points of their own, facing out of the region above the isovalue for a fog volume and below it
 * for any other grid. False with @p error set for a grid that is not float or double.
 * @p reference may be null.
 **/
bool geoSdfToPolygons(const openvdb::GridBase& grid,
                      const GeoSdfToPolygonsParams& params,
                      const GeoDetail* reference,
                      GeoDetailBuilder& out,
                      std::string* error);

/**
 * @brief A fog volume from the float or double SDF @p grid: 0 outside, 1 inside, the inner band
 * ramping between. Null with @p error set for other value types.
 **/
GeoVDBGridBaseConstPtr geoSdfToFog(const openvdb::GridBase& grid,
                                   std::string* error);

/**
 * @brief An SDF of the float or double fog volume @p grid, whose surface is where it crosses
 * @p isovalue. Null with @p error set for other value types.
 **/
GeoVDBGridBaseConstPtr geoFogToSdf(const openvdb::GridBase& grid,
                                   double isovalue,
                                   std::string* error);

/**
 * @brief @p grid resampled into @p target's index space. A level set is rebuilt as one with the
 * same band width in voxels; other grids are sampled. Float, double and float and double vector
 * grids convert; null with @p error set for others.
 **/
GeoVDBGridBaseConstPtr geoResampleVDB(const openvdb::GridBase& grid,
                                      const openvdb::math::Transform& target,
                                      GeoVDBInterpolationEnum interpolation,
                                      std::string* error);

/** @brief @p grid's transform with its voxels scaled by @p scale about the index-space origin. */
std::shared_ptr<openvdb::math::Transform> geoScaledVDBTransform(const openvdb::GridBase& grid,
                                                                double scale);

/** @brief @p grid's transform with its voxels resized to @p voxelSize along their first axis. */
std::shared_ptr<openvdb::math::Transform> geoVoxelSizedVDBTransform(const openvdb::GridBase& grid,
                                                                    double voxelSize);

/** @brief A note naming the grid, its active voxel count and the memory it uses. */
std::string geoVDBMemoryNote(const std::string& name,
                             const openvdb::GridBase& grid);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoVDBConvert_h
