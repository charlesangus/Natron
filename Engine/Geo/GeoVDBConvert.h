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

#include <array>
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
 * Without a reference transform, grids are made with a uniform linear transform whose index-space
 * origin is the world origin.
 **/

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
    // The distance, fog and mask grids first, then one grid per rasterised attribute.
    std::vector<GeoVDBConvertGrid> grids;
    // What the node's info shows: the size estimate, what was skipped and why, and the memory
    // the grids use.
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

/**
 * @brief The grid of the first VDB primitive of @p detail whose name matches @p pattern (see
 * geoNameMatchesPattern()), or null.
 **/
GeoVDBGridBaseConstPtr geoFindVDB(const GeoDetail& detail,
                                  const std::string& pattern);

struct GeoPolygonsToSdfParams {
    double voxelSize = 0.1;
    // When set, the grids take this transform (a reference VDB's) and voxelSize is ignored.
    std::shared_ptr<const openvdb::math::Transform> transform;
    // The bands are exteriorBandWorld and interiorBandWorld in world units instead of
    // exteriorBand and interiorBand in voxels.
    bool useWorldSpaceForBand = false;
    double exteriorBand = 3.;
    double interiorBand = 3.;
    double exteriorBandWorld = 0.3;
    double interiorBandWorld = 0.3;
    // Distances fill the whole inside instead of stopping at the interior band; a fog volume then
    // ramps from 0 at the surface to 1 at the deepest point.
    bool fillInterior = false;
    // The distance grid holds unsigned distances, which need no inside; the fog grid stays signed.
    bool unsignedDistanceField = false;
    bool buildDistance = true;
    std::string distanceName = "surface";
    bool buildFog = false;
    std::string fogName = "density";
    // Space-separated globs, ^ excluding, over point, vertex and primitive attribute names; each
    // match becomes a grid of the band's voxels holding the value at the closest surface point.
    // A vertex attribute wins over a point or primitive attribute of the same name, and a point
    // attribute over a primitive one. Empty for none.
    std::string attributes;
};

/**
 * @brief Active voxels one grid of geoPolygonsToSdf() takes at least: the band around the
 * polygons' area plus, with Fill Interior, the volume they enclose. Cheap; read before building.
 **/
double geoPolygonsToSdfVoxelEstimate(const GeoDetail& source,
                                     const GeoPolygonsToSdfParams& params);

/**
 * @brief Rasterises the live polygons of @p source (fan-triangulated) into a distance grid named
 * params.distanceName and a fog grid named params.fogName, each when asked for, plus the
 * attribute grids. A mesh with boundary edges is not closed: its inside is guessed, with a note
 * that the sign and the fog may leak through the holes. Polylines, packed and VDB primitives are
 * ignored. A source without polygons gives empty grids. False with @p error set for a voxel size
 * that is not positive and finite or a negative band.
 *
 * Numeric attributes of one or three components convert: float to a float or vector grid, int32
 * and int64 to grids of the same type taking the value of the closest corner. Others are skipped
 * with a note.
 **/
bool geoPolygonsToSdf(const GeoDetail& source,
                      const GeoPolygonsToSdfParams& params,
                      GeoVDBConvertResult* result,
                      std::string* error);

enum GeoParticleFootprintEnum {
    eGeoParticleFootprintSpherical = 0,
    // Each sphere is stretched back along -v * velocityScale by spheres shrinking to the minimum
    // radius, trailSpacing half-radii apart.
    eGeoParticleFootprintVelocityTrail,
};

struct GeoPointsToSdfParams {
    double voxelSize = 0.1;
    // As GeoPolygonsToSdfParams::transform.
    std::shared_ptr<const openvdb::math::Transform> transform;
    // A float SDF the spheres are unioned into, resampled onto the grids' transform. May be null.
    GeoVDBGridBaseConstPtr mergeInto;
    // A sphere's radius is the point's pscale (1 without one) times this.
    double radiusScale = 1.;
    // Voxels: smaller spheres are skipped and counted in a note.
    double minRadius = 1.5;
    // Half the width of the narrow band: bandWidthWorld in world units, or bandWidth voxels.
    bool useWorldSpaceForBand = false;
    double bandWidth = 3.;
    double bandWidthWorld = 0.3;
    // Collapses constant regions of the grids into tiles.
    bool prune = false;
    GeoParticleFootprintEnum footprint = eGeoParticleFootprintSpherical;
    double velocityScale = 1.;
    double trailSpacing = 1.;
    bool buildDistance = true;
    std::string distanceName = "surface";
    bool buildFog = false;
    std::string fogName = "density";
    // A fog volume of the shell between the spheres shrunk and grown by half maskWidth radii.
    bool buildMask = false;
    std::string maskName = "boundingvolume";
    double maskWidth = 1.;
    // Patterns over point attribute names, as GeoPolygonsToSdfParams::attributes; each voxel
    // takes the value of the sphere whose surface is closest.
    std::string attributes;
};

/** @brief Active voxels the distance grid of geoPointsToSdf() takes at most. Cheap; read before building. */
double geoPointsToSdfVoxelEstimate(const GeoDetail& source,
                                   const GeoPointsToSdfParams& params);

/**
 * @brief Unions spheres on the live points of @p source into the distance, fog and mask grids
 * asked for, plus the attribute grids. A source without points gives empty grids. False with
 * @p error set for a voxel size that is not positive and finite, or a band, minimum radius, trail
 * spacing or mask width that is not positive.
 **/
bool geoPointsToSdf(const GeoDetail& source,
                    const GeoPointsToSdfParams& params,
                    GeoVDBConvertResult* result,
                    std::string* error);

/** @brief The note giving a voxel estimate and the memory a float grid of that many voxels takes. */
std::string geoVDBEstimateNote(double voxels);

struct GeoSdfToPolygonsParams {
    // The surface level: 0 for an SDF, typically 0.5 for a fog volume.
    double isovalue = 0.;
    // 0 keeps every polygon voxel-sized; towards 1, flat regions are merged into larger ones.
    double adaptivity = 0.;
    // Vertex normals N from the polygons around each point within 60 degrees of each other; with
    // a reference, the surface and the interior polygons do not share theirs.
    bool computeVertexNormals = false;

    // These need a reference mesh. The adaptivity inside the reference surface.
    double internalAdaptivity = 0.01;
    // Every point takes the point attributes matching referenceAttributes (see
    // GeoPolygonsToSdfParams::attributes) of the closest reference point; the surface polygons
    // take the primitive and vertex attributes of the closest reference triangle, float vertex
    // values interpolated across it.
    bool transferSurfaceAttributes = true;
    std::string referenceAttributes = "*";
    // The seam points move onto the reference's edges and corners.
    bool sharpenFeatures = true;
    // Voxels where the reference surface turns sharper than this normal dot product keep their
    // full resolution whatever the adaptivity.
    double edgeTolerance = 0.5;
    // Groups for the polygons on the reference surface, those inside it, those along the seams
    // between the two, and the seams' points. Empty for none.
    std::string surfaceGroup = "surface_polygons";
    std::string interiorGroup = "interior_polygons";
    std::string seamLineGroup = "seam_polygons";
    std::string seamPoints = "seam_points";

    // Only the region inside this grid (below maskOffset for an SDF, above it for any other) is
    // meshed, or only the region outside it when inverted. May be null.
    GeoVDBGridBaseConstPtr surfaceMask;
    double maskOffset = 0.;
    bool invertSurfaceMask = false;
    // A float or double grid whose values multiply the adaptivity where it is sampled. May be null.
    GeoVDBGridBaseConstPtr adaptivityField;
};

/**
 * @brief Appends the isosurface of the scalar grid @p grid to @p out as quads and triangles on
 * points of their own, facing out of the region below the isovalue for an SDF and above it for
 * any other grid. False with @p error set for a grid that is not float or double. @p reference,
 * the polygons the grid was made from, may be null.
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
 * @brief A narrow-band SDF, 3 voxels each side, of the float or double fog volume @p grid, whose
 * surface is where it crosses @p isovalue, at the fog's voxel size. Null with @p error set for
 * other value types.
 **/
GeoVDBGridBaseConstPtr geoFogToSdf(const openvdb::GridBase& grid,
                                   double isovalue,
                                   std::string* error);

enum GeoVDBValueTypeEnum {
    eGeoVDBValueTypeSame = 0,
    eGeoVDBValueTypeFloat,
    eGeoVDBValueTypeInteger,
    eGeoVDBValueTypeVector,
    eGeoVDBValueTypeBool,
};

enum GeoVDBPrecisionEnum {
    eGeoVDBPrecisionSame = 0,
    eGeoVDBPrecision32,
    eGeoVDBPrecision64,
};

/**
 * @brief @p grid with its values converted to another type or precision, its topology kept: a
 * scalar goes into each component of a vector, a vector to a scalar keeps only the topology, and
 * a bool is 1 or 0. Only float grids keep their SDF or fog class. @p grid itself when nothing
 * changes. Null with @p error set for a value type it cannot read.
 **/
GeoVDBGridBaseConstPtr geoConvertVDBValues(const GeoVDBGridBaseConstPtr& grid,
                                           GeoVDBValueTypeEnum type,
                                           GeoVDBPrecisionEnum precision,
                                           std::string* error);

struct GeoVDBCleanupParams {
    // Voxels within this of the background are deactivated, then constant regions collapse.
    double pruneTolerance = 0.;
    // SDFs only: the inactive values take the sign of the side they are on.
    bool signedFloodFill = true;
    // SDFs only: every voxel inside the surface is made active.
    bool activateInsideVoxels = false;
};

/** @brief A copy of @p grid pruned and, for an SDF, flood filled as @p params asks. */
GeoVDBGridBaseConstPtr geoCleanupVDB(const openvdb::GridBase& grid,
                                     const GeoVDBCleanupParams& params);

struct GeoVDBResampleParams {
    GeoVDBInterpolationEnum interpolation = eGeoVDBInterpolationLinear;
    // A resampled SDF is rebuilt as one of the same band width in voxels instead of sampled.
    bool rebuildSdf = true;
    double pruneTolerance = 0.;
};

/**
 * @brief @p grid resampled into @p target's index space. Float, double, vector and integer grids
 * convert, integer ones always with the nearest voxel; null with @p error set for others.
 **/
GeoVDBGridBaseConstPtr geoResampleVDB(const openvdb::GridBase& grid,
                                      const openvdb::math::Transform& target,
                                      const GeoVDBResampleParams& params,
                                      std::string* error);

struct GeoVDBIndexTransform {
    // Three of 's', 'r' and 't', the first applied first.
    std::string order = "srt";
    // Three of 'x', 'y' and 'z', the first applied first.
    std::string rotateOrder = "xyz";
    // Voxels.
    std::array<double, 3> translate { 0., 0., 0. };
    // Degrees.
    std::array<double, 3> rotate { 0., 0., 0. };
    std::array<double, 3> scale { 1., 1., 1. };
    // Index space.
    std::array<double, 3> pivot { 0., 0., 0. };
    // Vector grids' values are transformed too, as their vector type says.
    bool transformVectors = false;
};

/**
 * @brief @p grid's values moved by @p xform in index space, on the same transform. Types as
 * geoResampleVDB().
 **/
GeoVDBGridBaseConstPtr geoTransformVDB(const openvdb::GridBase& grid,
                                       const GeoVDBIndexTransform& xform,
                                       const GeoVDBResampleParams& params,
                                       std::string* error);

/**
 * @brief @p grid's transform with its voxels scaled by @p scale about the index-space origin, or
 * an axis-aligned linear transform of the scaled voxel size through the same world origin.
 **/
std::shared_ptr<openvdb::math::Transform> geoScaledVDBTransform(const openvdb::GridBase& grid,
                                                                double scale,
                                                                bool axisAligned = false);

/** @brief geoScaledVDBTransform() resizing the voxels to @p voxelSize along their first axis. */
std::shared_ptr<openvdb::math::Transform> geoVoxelSizedVDBTransform(const openvdb::GridBase& grid,
                                                                    double voxelSize,
                                                                    bool axisAligned = false);

/** @brief A note naming the grid, its active voxel count and the memory it uses. */
std::string geoVDBMemoryNote(const std::string& name,
                             const openvdb::GridBase& grid);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoVDBConvert_h
