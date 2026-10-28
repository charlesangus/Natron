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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpSampling_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpSampling_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <utility>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

class SplineWarpContext;
class SplineWarpCurveShape;

/**
 * @brief The most constraint pairs a warp solves with, links and boundaries together. Above it
 * every span and boundary is decimated by the same factor.
 **/
#define kSplineWarpSampleBudget 1000

/**
 * @brief Pairs whose destinations are closer than this, in canonical pixels, merge into one.
 **/
#define kSplineWarpMergeDistance 1e-3

/**
 * @brief One constraint of the warp, in canonical coordinates: the output pixel at destination
 * samples the input at source.
 **/
struct SplineWarpSamplePair {
    Point destination;
    Point source;
};

/**
 * @brief A boundary as closed polylines in canonical coordinates. Nothing outside feather moves;
 * inside curve the warp is whole, and between the two it eases to zero along the Roto feather
 * fall-off profile. With no feather both polylines are the same.
 **/
struct SplineWarpBoundaryShape {
    std::vector<Point> curve;
    std::vector<Point> feather;
    double fallOff;

    SplineWarpBoundaryShape()
        : curve()
        , feather()
        , fallOff(1.)
    {
    }
};

/**
 * @brief Whether p is inside the closed polygon, by the even-odd rule.
 **/
bool splineWarpPointInPolygon(const std::vector<Point>& polygon, double x, double y);

/**
 * @brief The distance from p to the closed polyline.
 **/
double splineWarpDistanceToPolyline(const std::vector<Point>& polyline, double x, double y);

/**
 * @brief Appends to crossings the x of every edge of the closed polygon crossing the horizontal
 * line at y, under the even-odd rule's half-open convention, so that between consecutive sorted
 * crossings a point is alternately outside and inside.
 **/
void splineWarpPolygonRowCrossings(const std::vector<Point>& polygon, double y, std::vector<double>* crossings);

/**
 * @brief One link, ready to sample: the curves' shapes at the sampling time, its alignment, its
 * correspondences as (source parameter, destination parameter) and its amount.
 **/
struct SplineWarpLinkSamplingInput {
    const SplineWarpCurveShape* source;
    const SplineWarpCurveShape* destination;
    double startOffset;
    bool reversed;
    std::vector<std::pair<double, double>> correspondences;
    double amount;

    SplineWarpLinkSamplingInput()
        : source(0)
        , destination(0)
        , startOffset(0.)
        , reversed(false)
        , correspondences()
        , amount(1.)
    {
    }
};

/**
 * @brief One boundary, ready to sample: its curve and feather-point shapes, the feather distance
 * that expands the feather outward as the Roto renderer does, and the fall-off.
 **/
struct SplineWarpBoundarySamplingInput {
    const SplineWarpCurveShape* curve;
    const SplineWarpCurveShape* feather;
    double featherDistance;
    double fallOff;

    SplineWarpBoundarySamplingInput()
        : curve(0)
        , feather(0)
        , featherDistance(0.)
        , fallOff(1.)
    {
    }
};

struct SplineWarpSamplingSettings {
    double time;
    // Multiplies every link's amount.
    double globalAmount;
    bool invert;
    // The arc length between consecutive samples along a curve, in canonical pixels.
    double spacing;
    int budget;

    SplineWarpSamplingSettings()
        : time(0.)
        , globalAmount(1.)
        , invert(false)
        , spacing(4.)
        , budget(kSplineWarpSampleBudget)
    {
    }
};

/**
 * @brief The constraints of a warp at one time.
 **/
struct SplineWarpSampleSet {
    // Link pairs first, then boundary pairs, after merging and decimation.
    std::vector<SplineWarpSamplePair> pairs;
    int linkPairCount;
    std::vector<SplineWarpBoundaryShape> boundaries;
    // With boundaries: whether a link pair's destination lies inside a boundary's feather. When
    // none does the warp is the identity, as everything a link moves is held fixed.
    bool linkSamplesInsideBoundaries;
    // Of the pairs and the boundary shapes.
    U64 hash;

    SplineWarpSampleSet()
        : pairs()
        , linkPairCount(0)
        , boundaries()
        , linkSamplesInsideBoundaries(false)
        , hash(0)
    {
    }

    bool hasBoundaries() const
    {
        return !boundaries.empty();
    }
};

/**
 * @brief Samples links and boundaries into constraint pairs.
 *
 * Each link walks its source from parameter 0 and its destination from its aligned start. Its
 * correspondences split both walks into spans, and each span pairs the two curves by arc length,
 * a sample every settings.spacing along the longer side. Two closed curves pair one lap with one
 * lap; a closed curve linked to an open one is cut at its start and both ends are paired. A curve
 * with no segment pairs with its single point. Each pair's destination is then lerped toward its
 * source by globalAmount times the link's amount, after swapping the sides when inverted.
 *
 * Each boundary contributes zero-displacement pairs along its feather outline.
 **/
SplineWarpSampleSet sampleSplineWarp(const std::vector<SplineWarpLinkSamplingInput>& links,
                                     const std::vector<SplineWarpBoundarySamplingInput>& boundaries,
                                     const SplineWarpSamplingSettings& settings);

/**
 * @brief Same, on the activated links and boundaries of context at settings.time.
 **/
SplineWarpSampleSet sampleSplineWarp(const SplineWarpContext& context,
                                     const SplineWarpSamplingSettings& settings);

/**
 * @brief The closed polyline through shape, perSegment points per segment.
 **/
std::vector<Point> splineWarpPolyline(const SplineWarpCurveShape& shape, int perSegment);

/**
 * @brief The parameter on the other curve of link that its pairing, the arc-length spans between
 * its correspondences, puts with param: on the destination when fromSource, on the source otherwise.
 **/
double splineWarpPairedParam(const SplineWarpLinkSamplingInput& link, bool fromSource, double param);

/**
 * @brief param, on the source curve of link when onSource and on its destination otherwise, moved
 * along that curve's walk to lie strictly between the ends of the correspondences next to
 * correspondence index, in source order, so no span inverts.
 **/
double splineWarpClampCorrespondenceParam(const SplineWarpLinkSamplingInput& link, int index, bool onSource, double param);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpSampling_h
