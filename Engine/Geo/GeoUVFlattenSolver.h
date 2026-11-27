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

#ifndef Engine_Geo_GeoUVFlattenSolver_h
#define Engine_Geo_GeoUVFlattenSolver_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/Geo/GeoUVIslands.h"
#include "Engine/Geo/GeoUVPack.h"
#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_ENTER

enum GeoUVFlattenMethodEnum {
    // Spectral conformal parameterization: a generalized eigenproblem, always solvable.
    eGeoUVFlattenMethodSCP = 0,
    // Angle-based flattening (ABF++) followed by a least-squares layout of the solved angles.
    eGeoUVFlattenMethodABF,
};

/**
 * @brief One island to flatten: its corners merged into chart vertices wherever the island is
 * joined, and its polygons as rings of chart vertices.
 **/
struct GeoUVFlattenChart {
    std::vector<std::array<double, 3>> positions;
    // By chart vertex: one vertex offset of the island wired to it.
    std::vector<GeoOffset> vertexOffsets;
    std::vector<std::vector<int>> faces;
    // By face: the polygon's primitive offset.
    std::vector<GeoOffset> faceOffsets;
};

struct GeoUVFlattenPin {
    int vertex = -1;
    GeoUV uv = { 0., 0. };
    // Where the chart vertex after the pinned one in its polygon goes, when the pin is alone
    // in its chart; -1 for none.
    int referenceVertex = -1;
    GeoUV referenceUV = { 0., 0. };
};

enum GeoUVFlattenAxisEnum {
    // The vertices line up along u: they share v.
    eGeoUVFlattenAxisU = 0,
    // The vertices line up along v: they share u.
    eGeoUVFlattenAxisV,
};

struct GeoUVFlattenAlignment {
    std::vector<int> vertices;
    GeoUVFlattenAxisEnum axis = eGeoUVFlattenAxisU;
};

/** @brief The constraints on one chart, all in chart vertices. */
struct GeoUVFlattenChartConstraints {
    std::vector<GeoUVFlattenPin> pins;
    std::vector<GeoUVFlattenAlignment> alignments;
    // Each a sequence of chart edges (from, to) to lie on one straight line; ABF only.
    std::vector<std::vector<std::array<int, 2>>> straightenings;
    // Horizontal edges (from, to) of quad faces: each quad holding one becomes an axis-aligned
    // rectangle, its neighbours in the region taking the same orientation.
    std::vector<std::array<int, 2>> rectify;
    // By chart vertex, the uv its boundary vertices keep; SCP only, empty for none.
    std::vector<GeoUV> preservedBoundary;
};

struct GeoUVFlattenChartResult {
    // By chart vertex.
    std::vector<GeoUV> uv;
    bool solved = false;
    // True when an ABF solve failed and the chart was flattened with SCP instead.
    bool fellBack = false;
    // True when pins or a preserved boundary fix the result's position, so packing leaves it
    // where it is.
    bool placed = false;
    int degenerateFaces = 0;
};

/**
 * @brief Builds one chart per island: corners of a point are one chart vertex where the
 * island joins across their edges, so a point a seam runs through is split. Fills
 * *chartVertexOfVertex, by vertex offset, with the chart vertex of each island corner and -1
 * elsewhere.
 **/
void geoBuildUVFlattenCharts(const GeoDetail& detail,
                             const GeoUVIslands& islands,
                             std::vector<GeoUVFlattenChart>* charts,
                             std::vector<int>* chartVertexOfVertex);

/**
 * @brief Flattens one chart, minimizing angular distortion with the method and honouring the
 * constraints. Charts without placing constraints are scaled so their uv area equals their
 * surface area. Faces with no area are skipped and counted. An ABF solve that fails falls back
 * to SCP. Returns false with *errorMessage set when even SCP cannot solve.
 **/
bool geoFlattenUVChart(const GeoUVFlattenChart& chart,
                       const GeoUVFlattenChartConstraints& constraints,
                       GeoUVFlattenMethodEnum method,
                       GeoUVFlattenChartResult* result,
                       std::string* errorMessage);

/**
 * @brief geoFlattenUVChart() over every chart in parallel. Stops early, returning false with no
 * message, once aborted() returns true.
 **/
bool geoFlattenUVCharts(const std::vector<GeoUVFlattenChart>& charts,
                        const std::vector<GeoUVFlattenChartConstraints>& constraints,
                        GeoUVFlattenMethodEnum method,
                        const std::function<bool()>& aborted,
                        std::vector<GeoUVFlattenChartResult>* results,
                        std::string* errorMessage);

/**
 * @brief The mean, over the non-degenerate triangles of the chart's polygon fans, of the
 * absolute difference between each corner's angle in uv and on the surface, in radians.
 **/
double geoUVFlattenAngleDistortion(const GeoUVFlattenChart& chart,
                                   const std::vector<GeoUV>& uv);

/**
 * @brief The rotation, in radians, that turns the points' minimum-area bounding rectangle
 * onto the axes, choosing the smaller turn among equivalent ones.
 **/
double geoUVMinimumAreaRotation(const std::vector<GeoUV>& points);

/**
 * @brief The rotation, uniform scale and translation (no reflection) taking @p from closest to
 * @p to in least squares, as a placement.
 **/
GeoUVPackPlacement geoUVFitSimilarity(const std::vector<GeoUV>& from,
                                      const std::vector<GeoUV>& to);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoUVFlattenSolver_h
